#include "ddrtiming/ddrtiming.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

#include "../core/config.hpp"
#include "../core/engine.hpp"
#include "../core/log_parser.hpp"
#include "../core/report.hpp"

namespace {
thread_local std::string g_create_error;
}

// One mutex per engine, held for the duration of every call that touches
// engine->engine or engine->last_error. The scheduling algorithm itself is
// inherently sequential (a single event-driven simulation), so serializing
// calls costs nothing beyond what the algorithm already required -- this
// exists purely so that a caller modeling N concurrent DMA cores as N OS
// threads, each calling straight into this same handle, gets correct
// accounting instead of a silent data race on results()/summary()/internal
// state. Concurrent DMA cores are already modeled via AxiTxn::core_id; you
// do not need real OS-level parallelism to represent them -- see README.
// This does NOT make ddrt_destroy() safe to call while another thread might
// still be calling into the same handle -- that's an object-lifetime race
// no amount of internal locking can fix; the caller must ensure no other
// thread is mid-call before destroying.
struct ddrt_engine {
    std::unique_ptr<ddrtiming::Engine> engine;
    std::string last_error;
    std::mutex mutex;
};

ddrt_engine_t* ddrt_create(const char* config_json_path) {
    if (!config_json_path) { g_create_error = "config_json_path is null"; return nullptr; }
    try {
        ddrtiming::DdrcConfig cfg = ddrtiming::DdrcConfig::load_from_file(config_json_path);
        auto* h = new ddrt_engine();
        h->engine = std::make_unique<ddrtiming::Engine>(std::move(cfg));
        return h;
    } catch (const std::exception& e) {
        g_create_error = e.what();
        return nullptr;
    }
}

void ddrt_destroy(ddrt_engine_t* engine) { delete engine; }

int ddrt_push_txn(ddrt_engine_t* engine, const ddrt_axi_txn_t* txn, uint64_t* out_txn_id) {
    if (!engine || !txn) return -1;
    std::lock_guard<std::mutex> lock(engine->mutex);
    try {
        ddrtiming::AxiTxn t;
        t.core_id = txn->core_id;
        t.type = (txn->type == DDRT_WRITE) ? ddrtiming::TxnType::Write : ddrtiming::TxnType::Read;
        t.axi_id = txn->axi_id;
        t.addr = txn->addr;
        t.size_bytes = txn->size_bytes;
        t.len_beats = txn->len_beats;
        if (txn->wstrb && txn->wstrb_len > 0) {
            t.wstrb.assign(txn->wstrb, txn->wstrb + txn->wstrb_len);
        }
        uint64_t id = engine->engine->push_txn(t);
        if (out_txn_id) *out_txn_id = id;
        return 0;
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return -1;
    }
}

int ddrt_push_barrier(ddrt_engine_t* engine, int core_id) {
    if (!engine) return -1;
    std::lock_guard<std::mutex> lock(engine->mutex);
    try {
        engine->engine->push_barrier(core_id);
        return 0;
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return -1;
    }
}

int ddrt_load_log_file(ddrt_engine_t* engine, int core_id, const char* log_csv_path) {
    if (!engine || !log_csv_path) return -1;
    std::lock_guard<std::mutex> lock(engine->mutex);
    try {
        auto entries = ddrtiming::parse_axi_log_file(log_csv_path, core_id);
        for (const auto& e : entries) {
            if (e.is_barrier) engine->engine->push_barrier(core_id);
            else engine->engine->push_txn(e.txn);
        }
        return 0;
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return -1;
    }
}

int ddrt_run(ddrt_engine_t* engine) {
    if (!engine) return -1;
    std::lock_guard<std::mutex> lock(engine->mutex);
    try {
        engine->engine->run();
        return 0;
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return -1;
    }
}

int ddrt_get_summary(ddrt_engine_t* engine, ddrt_summary_t* out) {
    if (!engine || !out) return -1;
    std::lock_guard<std::mutex> lock(engine->mutex);
    const ddrtiming::SummaryStats& s = engine->engine->summary();
    out->total_txns = s.total_txns;
    out->total_bytes = s.total_bytes;
    out->total_dram_bytes = s.total_dram_bytes;
    out->total_cycles = s.total_cycles;
    out->sim_time_ns = s.sim_time_ns;
    out->avg_bandwidth_gbps = s.avg_bandwidth_gbps;
    out->avg_dram_bandwidth_gbps = s.avg_dram_bandwidth_gbps;
    out->peak_bandwidth_gbps = s.peak_bandwidth_gbps;
    out->bandwidth_utilization_pct = s.bandwidth_utilization_pct;
    out->burst_efficiency_pct = s.burst_efficiency_pct;
    out->avg_latency_ns = s.avg_latency_ns;
    out->page_hit_rate_pct = s.page_hit_rate_pct;
    out->row_conflict_rate_pct = s.row_conflict_rate_pct;
    out->row_empty_rate_pct = s.row_empty_rate_pct;
    out->refresh_overhead_pct = s.refresh_overhead_pct;
    out->turnaround_overhead_pct = s.turnaround_overhead_pct;
    out->bankgroup_reuse_rate_pct = s.bankgroup_reuse_rate_pct;
    out->mapped_address_bits = s.mapped_address_bits;
    out->high_address_regions = s.high_address_regions;
    return 0;
}

uint64_t ddrt_get_num_results(ddrt_engine_t* engine) {
    if (!engine) return 0;
    std::lock_guard<std::mutex> lock(engine->mutex);
    return engine->engine->results().size();
}

int ddrt_get_result_at(ddrt_engine_t* engine, uint64_t index, ddrt_txn_result_t* out) {
    if (!engine || !out) return -1;
    std::lock_guard<std::mutex> lock(engine->mutex);
    const auto& results = engine->engine->results();
    if (index >= results.size()) return -1;
    const ddrtiming::TxnResult& r = results[index];
    out->txn_id = r.txn_id;
    out->core_id = r.core_id;
    out->type = (r.type == ddrtiming::TxnType::Write) ? DDRT_WRITE : DDRT_READ;
    out->addr = r.addr;
    out->issue_cycle = r.issue_cycle;
    out->complete_cycle = r.complete_cycle;
    out->latency_ns = r.latency_ns;
    out->dominant_row_status = static_cast<ddrt_row_status_t>(r.dominant_row_status);
    out->bytes = r.bytes;
    out->dram_bytes = r.dram_bytes;
    out->hits = r.hits;
    out->conflicts = r.conflicts;
    out->empties = r.empties;
    return 0;
}

int ddrt_prune_results_before(ddrt_engine_t* engine, uint64_t max_txn_id) {
    if (!engine) return -1;
    std::lock_guard<std::mutex> lock(engine->mutex);
    try {
        engine->engine->prune_results_before(max_txn_id);
        return 0;
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return -1;
    }
}

uint64_t ddrt_get_num_windows(ddrt_engine_t* engine) {
    if (!engine) return 0;
    std::lock_guard<std::mutex> lock(engine->mutex);
    return engine->engine->windows().size();
}

int ddrt_get_window_at(ddrt_engine_t* engine, uint64_t index, ddrt_window_stats_t* out) {
    if (!engine || !out) return -1;
    std::lock_guard<std::mutex> lock(engine->mutex);
    const auto& windows = engine->engine->windows();
    if (index >= windows.size()) return -1;
    const ddrtiming::WindowStats& w = windows[index];
    const ddrtiming::DdrcConfig& cfg = engine->engine->config();

    out->window_index = index;
    out->start_ns = static_cast<double>(index) * cfg.history_window_ns;
    out->duration_ns = cfg.history_window_ns;
    out->bytes_read = w.bytes_read;
    out->bytes_written = w.bytes_written;
    out->dram_bytes = w.dram_bytes;
    out->txn_count = w.txn_count;
    out->hits = w.hits;
    out->conflicts = w.conflicts;
    out->empties = w.empties;
    out->avg_bandwidth_gbps = (out->duration_ns > 0.0)
        ? static_cast<double>(w.bytes_read + w.bytes_written) / out->duration_ns
        : 0.0;
    out->outstanding_high_water = w.max_outstanding_count;
    int max_out = std::max(1, cfg.max_outstanding_per_id);
    out->outstanding_occupancy_pct = static_cast<double>(w.max_outstanding_count) / max_out * 100.0;
    out->active_bank_count = w.active_banks.size();
    out->bank_utilization_pct = static_cast<double>(w.active_banks.size()) / cfg.total_banks() * 100.0;
    return 0;
}

uint64_t ddrt_get_num_channels(ddrt_engine_t* engine) {
    if (!engine) return 0;
    std::lock_guard<std::mutex> lock(engine->mutex);
    return static_cast<uint64_t>(std::max(1, engine->engine->config().channels));
}

int ddrt_get_window_channel_stats(ddrt_engine_t* engine, uint64_t window_index,
                                   uint64_t channel_index, ddrt_channel_window_stats_t* out) {
    if (!engine || !out) return -1;
    std::lock_guard<std::mutex> lock(engine->mutex);
    const auto& windows = engine->engine->windows();
    if (window_index >= windows.size()) return -1;
    const ddrtiming::DdrcConfig& cfg = engine->engine->config();
    if (channel_index >= static_cast<uint64_t>(std::max(1, cfg.channels))) return -1;

    const ddrtiming::WindowStats& w = windows[window_index];
    uint64_t bytes = (channel_index < w.dram_bytes_per_channel.size()) ? w.dram_bytes_per_channel[channel_index] : 0;

    out->dram_bytes = bytes;
    double duration_ns = cfg.history_window_ns;
    out->avg_bandwidth_gbps = duration_ns > 0.0 ? static_cast<double>(bytes) / duration_ns : 0.0;
    double peak = cfg.peak_bandwidth_per_channel_gbps();
    out->utilization_pct = peak > 0.0 ? out->avg_bandwidth_gbps / peak * 100.0 : 0.0;
    return 0;
}

uint64_t ddrt_get_num_cores(ddrt_engine_t* engine) {
    if (!engine) return 0;
    std::lock_guard<std::mutex> lock(engine->mutex);
    return engine->engine->num_cores_with_burst_stats();
}

int ddrt_get_core_burst_stats_at(ddrt_engine_t* engine, uint64_t index, ddrt_core_burst_stats_t* out) {
    if (!engine || !out) return -1;
    std::lock_guard<std::mutex> lock(engine->mutex);
    if (index >= engine->engine->num_cores_with_burst_stats()) return -1;
    const ddrtiming::CoreBurstStats s = engine->engine->core_burst_stats_at(index);
    out->core_id = s.core_id;
    out->txn_count = s.txn_count;
    out->total_bytes = s.total_bytes;
    out->mean_bytes = s.mean_bytes;
    out->min_bytes = s.min_bytes;
    out->p25_bytes = s.p25_bytes;
    out->p50_bytes = s.p50_bytes;
    out->p75_bytes = s.p75_bytes;
    out->max_bytes = s.max_bytes;
    return 0;
}

int ddrt_write_report_json(ddrt_engine_t* engine, const char* out_path) {
    if (!engine || !out_path) return -1;
    std::lock_guard<std::mutex> lock(engine->mutex);
    try {
        ddrtiming::write_report_json(*engine->engine, out_path);
        return 0;
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return -1;
    }
}

const char* ddrt_last_error(ddrt_engine_t* engine) {
    if (!engine) return g_create_error.c_str();
    std::lock_guard<std::mutex> lock(engine->mutex);
    return engine->last_error.c_str();
}

const char* ddrt_version(void) { return "0.3.0"; }
