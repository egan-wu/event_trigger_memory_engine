#include "report.hpp"

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
            double bw = window_ns > 0.0 ? static_cast<double>(w.bytes_read + w.bytes_written) / window_ns : 0.0;
            wv.set("avg_bandwidth_gbps", bw);
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

    f << "window_index,start_ns,bytes_read,bytes_written,dram_bytes,txn_count,"
         "hits,conflicts,empties,avg_bandwidth_gbps\n";
    for (size_t i = 0; i < engine.windows().size(); ++i) {
        const WindowStats& w = engine.windows()[i];
        double start_ns = static_cast<double>(i) * window_ns;
        double bw = window_ns > 0.0 ? static_cast<double>(w.bytes_read + w.bytes_written) / window_ns : 0.0;
        f << i << ',' << start_ns << ',' << w.bytes_read << ',' << w.bytes_written << ','
          << w.dram_bytes << ',' << w.txn_count << ',' << w.hits << ',' << w.conflicts << ','
          << w.empties << ',' << bw << '\n';
    }
}

} // namespace ddrtiming
