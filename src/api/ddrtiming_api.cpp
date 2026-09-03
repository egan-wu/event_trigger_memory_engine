#include "ddrtiming/ddrtiming.h"

#include <memory>
#include <string>

#include "../core/config.hpp"
#include "../core/engine.hpp"
#include "../core/log_parser.hpp"
#include "../core/report.hpp"

namespace {
thread_local std::string g_create_error;
}

struct ddrt_engine {
    std::unique_ptr<ddrtiming::Engine> engine;
    std::string last_error;
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
    return 0;
}

uint64_t ddrt_get_num_results(ddrt_engine_t* engine) {
    if (!engine) return 0;
    return engine->engine->results().size();
}

int ddrt_get_result_at(ddrt_engine_t* engine, uint64_t index, ddrt_txn_result_t* out) {
    if (!engine || !out) return -1;
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
    try {
        engine->engine->prune_results_before(max_txn_id);
        return 0;
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return -1;
    }
}

int ddrt_write_report_json(ddrt_engine_t* engine, const char* out_path) {
    if (!engine || !out_path) return -1;
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
    return engine->last_error.c_str();
}

const char* ddrt_version(void) { return "0.1.0"; }
