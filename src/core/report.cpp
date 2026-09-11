#include "report.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "json.hpp"

namespace ddrtiming {

namespace {
const char* row_status_str(RowStatus s) {
    switch (s) {
        case RowStatus::Hit: return "hit";
        case RowStatus::Conflict: return "conflict";
        case RowStatus::Empty: return "empty";
    }
    return "unknown";
}
const char* txn_type_str(TxnType t) { return t == TxnType::Read ? "AR" : "AW"; }
} // namespace

void write_report_json(const Engine& engine, const std::string& out_path) {
    const SummaryStats& s = engine.summary();

    json::Value summary = json::Value::make_object();
    summary.set("total_txns", static_cast<int64_t>(s.total_txns));
    summary.set("total_bytes", static_cast<int64_t>(s.total_bytes));
    summary.set("total_dram_bytes", static_cast<int64_t>(s.total_dram_bytes));
    summary.set("total_cycles", static_cast<int64_t>(s.total_cycles));
    summary.set("sim_time_ns", s.sim_time_ns);
    summary.set("avg_bandwidth_gbps", s.avg_bandwidth_gbps);
    summary.set("avg_dram_bandwidth_gbps", s.avg_dram_bandwidth_gbps);
    summary.set("peak_bandwidth_gbps", s.peak_bandwidth_gbps);
    summary.set("bandwidth_utilization_pct", s.bandwidth_utilization_pct);
    summary.set("burst_efficiency_pct", s.burst_efficiency_pct);
    summary.set("avg_latency_ns", s.avg_latency_ns);
    summary.set("page_hit_rate_pct", s.page_hit_rate_pct);
    summary.set("row_conflict_rate_pct", s.row_conflict_rate_pct);
    summary.set("row_empty_rate_pct", s.row_empty_rate_pct);
    summary.set("refresh_overhead_pct", s.refresh_overhead_pct);
    summary.set("turnaround_overhead_pct", s.turnaround_overhead_pct);
    summary.set("bankgroup_reuse_rate_pct", s.bankgroup_reuse_rate_pct);
    summary.set("mapped_address_bits", static_cast<int64_t>(s.mapped_address_bits));
    summary.set("high_address_regions", static_cast<int64_t>(s.high_address_regions));

    json::Value txns = json::Value::make_array();
    for (const auto& r : engine.results()) {
        json::Value tv = json::Value::make_object();
        tv.set("txn_id", static_cast<int64_t>(r.txn_id));
        tv.set("core_id", r.core_id);
        tv.set("type", txn_type_str(r.type));
        tv.set("addr", static_cast<int64_t>(r.addr));
        tv.set("bytes", static_cast<int64_t>(r.bytes));
        tv.set("dram_bytes", static_cast<int64_t>(r.dram_bytes));
        tv.set("issue_cycle", static_cast<int64_t>(r.issue_cycle));
        tv.set("complete_cycle", static_cast<int64_t>(r.complete_cycle));
        tv.set("latency_ns", r.latency_ns);
        tv.set("dominant_row_status", row_status_str(r.dominant_row_status));
        tv.set("hits", static_cast<int64_t>(r.hits));
        tv.set("conflicts", static_cast<int64_t>(r.conflicts));
        tv.set("empties", static_cast<int64_t>(r.empties));
        txns.push_back(std::move(tv));
    }

    json::Value root = json::Value::make_object();
    root.set("summary", std::move(summary));
    root.set("transactions", std::move(txns));

    if (!engine.windows().empty()) {
        double window_ns = engine.config().history_window_ns;
        json::Value windows = json::Value::make_array();
        for (size_t i = 0; i < engine.windows().size(); ++i) {
            const WindowStats& w = engine.windows()[i];
            json::Value wv = json::Value::make_object();
            wv.set("window_index", static_cast<int64_t>(i));
            wv.set("start_ns", static_cast<double>(i) * window_ns);
            wv.set("bytes_read", static_cast<int64_t>(w.bytes_read));
            wv.set("bytes_written", static_cast<int64_t>(w.bytes_written));
            wv.set("dram_bytes", static_cast<int64_t>(w.dram_bytes));
            wv.set("txn_count", static_cast<int64_t>(w.txn_count));
            wv.set("hits", static_cast<int64_t>(w.hits));
            wv.set("conflicts", static_cast<int64_t>(w.conflicts));
            wv.set("empties", static_cast<int64_t>(w.empties));
            wv.set("bankgroup_reuse_count", static_cast<int64_t>(w.bankgroup_reuse_count));
            double bw = window_ns > 0.0 ? static_cast<double>(w.bytes_read + w.bytes_written) / window_ns : 0.0;
            wv.set("avg_bandwidth_gbps", bw);
            wv.set("outstanding_high_water", static_cast<int64_t>(w.max_outstanding_count));
            int max_out = std::max(1, engine.config().max_outstanding_per_id);
            wv.set("outstanding_occupancy_pct", static_cast<double>(w.max_outstanding_count) / max_out * 100.0);
            wv.set("active_bank_count", static_cast<int64_t>(w.active_banks.size()));
            wv.set("bank_utilization_pct", static_cast<double>(w.active_banks.size()) / engine.config().total_banks() * 100.0);

            // Per-channel breakdown -- the aggregate dram_bytes/avg_bandwidth_gbps
            // above can't distinguish "every channel at 50%" from "one channel
            // maxed, one idle"; both sum to the same aggregate number.
            double peak_per_channel = engine.config().peak_bandwidth_per_channel_gbps();
            json::Value channels = json::Value::make_array();
            int nchannels = std::max(1, engine.config().channels);
            for (int ch = 0; ch < nchannels; ++ch) {
                uint64_t ch_bytes = (static_cast<size_t>(ch) < w.dram_bytes_per_channel.size()) ? w.dram_bytes_per_channel[ch] : 0;
                double ch_bw = window_ns > 0.0 ? static_cast<double>(ch_bytes) / window_ns : 0.0;
                json::Value cv = json::Value::make_object();
                cv.set("dram_bytes", static_cast<int64_t>(ch_bytes));
                cv.set("avg_bandwidth_gbps", ch_bw);
                cv.set("utilization_pct", peak_per_channel > 0.0 ? ch_bw / peak_per_channel * 100.0 : 0.0);
                channels.push_back(std::move(cv));
            }
            wv.set("channels", std::move(channels));

            windows.push_back(std::move(wv));
        }
        root.set("windows", std::move(windows));
    }

    std::ofstream f(out_path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open output path for report: " + out_path);
    f << root.dump(2);
}

std::string format_summary_text(const Engine& engine) {
    const SummaryStats& s = engine.summary();
    std::ostringstream os;
    os << "==== DDR Timing Estimate Summary ====\n";
    os << "Transactions:            " << s.total_txns << "\n";
    os << "Total bytes (requested): " << s.total_bytes << "\n";
    os << "Total bytes (DRAM/phys): " << s.total_dram_bytes << "\n";
    os << "Simulated time:          " << s.sim_time_ns << " ns (" << s.total_cycles << " cycles)\n";
    os << "Avg bandwidth (useful):  " << s.avg_bandwidth_gbps << " GB/s\n";
    os << "Avg bandwidth (DRAM):    " << s.avg_dram_bandwidth_gbps << " GB/s\n";
    os << "Peak bandwidth (config): " << s.peak_bandwidth_gbps << " GB/s\n";
    os << "Bandwidth utilization:   " << s.bandwidth_utilization_pct << " %\n";
    os << "Burst efficiency:        " << s.burst_efficiency_pct << " %\n";
    os << "Avg latency:             " << s.avg_latency_ns << " ns\n";
    os << "Page-hit rate:           " << s.page_hit_rate_pct << " %\n";
    os << "Row-conflict rate:       " << s.row_conflict_rate_pct << " %\n";
    os << "Row-empty rate:          " << s.row_empty_rate_pct << " %\n";
    os << "Refresh overhead:        " << s.refresh_overhead_pct << " %\n";
    os << "R/W turnaround overhead: " << s.turnaround_overhead_pct << " %\n";
    os << "Bank-group reuse rate:   " << s.bankgroup_reuse_rate_pct << " % (tCCD_L instead of tCCD_S)\n";
    os << "Address map decodes:     bits [0, " << s.mapped_address_bits << ")  -- "
       << s.high_address_regions << " distinct region(s) above that\n";
    return os.str();
}

void write_windowed_csv(const Engine& engine, const std::string& out_path) {
    if (engine.windows().empty()) {
        throw std::runtime_error(
            "no windowed history available -- set \"reporting\": {\"history_window_ns\": N} in the config");
    }
    double window_ns = engine.config().history_window_ns;

    std::ofstream f(out_path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open output path for windowed CSV: " + out_path);

    int max_out = std::max(1, engine.config().max_outstanding_per_id);
    int total_banks = engine.config().total_banks();
    int nchannels = std::max(1, engine.config().channels);

    f << "window_index,start_ns,bytes_read,bytes_written,dram_bytes,txn_count,"
         "hits,conflicts,empties,bankgroup_reuse_count,avg_bandwidth_gbps,outstanding_high_water,outstanding_occupancy_pct,"
         "active_bank_count,bank_utilization_pct";
    // Per-channel columns -- lets a viewer distinguish "every channel at 50%"
    // from "one channel maxed, one idle", both invisible in the aggregate above.
    for (int ch = 0; ch < nchannels; ++ch) f << ",ch" << ch << "_dram_bytes,ch" << ch << "_avg_bandwidth_gbps";
    f << '\n';

    for (size_t i = 0; i < engine.windows().size(); ++i) {
        const WindowStats& w = engine.windows()[i];
        double start_ns = static_cast<double>(i) * window_ns;
        double bw = window_ns > 0.0 ? static_cast<double>(w.bytes_read + w.bytes_written) / window_ns : 0.0;
        double occ_pct = static_cast<double>(w.max_outstanding_count) / max_out * 100.0;
        uint64_t active_banks = w.active_banks.size();
        double bank_util_pct = static_cast<double>(active_banks) / total_banks * 100.0;
        f << i << ',' << start_ns << ',' << w.bytes_read << ',' << w.bytes_written << ','
          << w.dram_bytes << ',' << w.txn_count << ',' << w.hits << ',' << w.conflicts << ','
          << w.empties << ',' << w.bankgroup_reuse_count << ',' << bw << ',' << w.max_outstanding_count << ',' << occ_pct << ','
          << active_banks << ',' << bank_util_pct;
        for (int ch = 0; ch < nchannels; ++ch) {
            uint64_t ch_bytes = (static_cast<size_t>(ch) < w.dram_bytes_per_channel.size()) ? w.dram_bytes_per_channel[ch] : 0;
            double ch_bw = window_ns > 0.0 ? static_cast<double>(ch_bytes) / window_ns : 0.0;
            f << ',' << ch_bytes << ',' << ch_bw;
        }
        f << '\n';
    }
}

} // namespace ddrtiming
