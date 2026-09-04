// Standalone analyzer for windowed-history CSVs (the same files
// --windowed-csv writes): reads the CSV alone, no engine/config needed, and
// emits structured findings as JSON -- built for a caller (an AI agent, a
// CI check, a human piping into jq) that needs to reason about the data
// without visually reading a chart. Every finding is self-contained: what
// was detected, where (window range), and why it matters, so it can be
// acted on directly from the JSON alone.
//
// Usage: windowed_history_analyze --csv <path> [--out <path>]
// (writes to stdout if --out is omitted)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "../src/core/json.hpp"

namespace {

struct Row {
    uint64_t window_index = 0;
    double start_ns = 0.0;
    uint64_t bytes_read = 0, bytes_written = 0, dram_bytes = 0, txn_count = 0;
    uint64_t hits = 0, conflicts = 0, empties = 0;
    double avg_bandwidth_gbps = 0.0;
    double outstanding_occupancy_pct = -1.0; // -1 = column absent
    double bank_utilization_pct = -1.0;      // -1 = column absent
    std::vector<double> channel_bandwidth_gbps; // empty if columns absent
};

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

std::vector<Row> parse_csv(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open CSV file: " + path);

    std::string header_line;
    if (!std::getline(f, header_line)) throw std::runtime_error("empty CSV file: " + path);
    std::vector<std::string> header = split_csv_line(header_line);
    std::map<std::string, size_t> idx;
    for (size_t i = 0; i < header.size(); ++i) idx[header[i]] = i;

    auto has = [&](const std::string& name) { return idx.count(name) > 0; };

    int num_channels = 0;
    while (has("ch" + std::to_string(num_channels) + "_avg_bandwidth_gbps")) ++num_channels;

    std::vector<Row> rows;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<std::string> cells = split_csv_line(line);
        auto get = [&](const std::string& name, double def = 0.0) -> double {
            auto it = idx.find(name);
            if (it == idx.end() || it->second >= cells.size() || cells[it->second].empty()) return def;
            return std::strtod(cells[it->second].c_str(), nullptr);
        };
        Row r;
        r.window_index = static_cast<uint64_t>(get("window_index"));
        r.start_ns = get("start_ns");
        r.bytes_read = static_cast<uint64_t>(get("bytes_read"));
        r.bytes_written = static_cast<uint64_t>(get("bytes_written"));
        r.dram_bytes = static_cast<uint64_t>(get("dram_bytes"));
        r.txn_count = static_cast<uint64_t>(get("txn_count"));
        r.hits = static_cast<uint64_t>(get("hits"));
        r.conflicts = static_cast<uint64_t>(get("conflicts"));
        r.empties = static_cast<uint64_t>(get("empties"));
        r.avg_bandwidth_gbps = get("avg_bandwidth_gbps");
        r.outstanding_occupancy_pct = has("outstanding_occupancy_pct") ? get("outstanding_occupancy_pct") : -1.0;
        r.bank_utilization_pct = has("bank_utilization_pct") ? get("bank_utilization_pct") : -1.0;
        for (int c = 0; c < num_channels; ++c) {
            r.channel_bandwidth_gbps.push_back(get("ch" + std::to_string(c) + "_avg_bandwidth_gbps"));
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t mid = v.size() / 2;
    return (v.size() % 2 == 0) ? (v[mid - 1] + v[mid]) / 2.0 : v[mid];
}

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

// Groups consecutive window indices where pred(i) is true into [start,end]
// (inclusive) ranges, dropping any run shorter than min_run (noise floor).
std::vector<std::pair<size_t, size_t>> find_runs(size_t n, size_t min_run, const std::function<bool(size_t)>& pred) {
    std::vector<std::pair<size_t, size_t>> runs;
    size_t i = 0;
    while (i < n) {
        if (!pred(i)) { ++i; continue; }
        size_t start = i;
        while (i < n && pred(i)) ++i;
        if (i - start >= min_run) runs.emplace_back(start, i - 1);
    }
    return runs;
}

std::string fmt_ns(double ns) {
    std::ostringstream os;
    if (ns >= 1e6) os << (ns / 1e6) << " ms";
    else if (ns >= 1e3) os << (ns / 1e3) << " us";
    else os << ns << " ns";
    return os.str();
}

} // namespace

int main(int argc, char** argv) {
    std::string csv_path, out_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << flag << "\n"; std::exit(1); }
            return argv[++i];
        };
        if (arg == "--csv") csv_path = need("--csv");
        else if (arg == "--out") out_path = need("--out");
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: windowed_history_analyze --csv <windowed_history.csv> [--out <findings.json>]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 1;
        }
    }
    if (csv_path.empty()) {
        std::cerr << "Usage: windowed_history_analyze --csv <windowed_history.csv> [--out <findings.json>]\n";
        return 1;
    }

    std::vector<Row> rows;
    try {
        rows = parse_csv(csv_path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    if (rows.empty()) {
        std::cerr << "error: no data rows in " << csv_path << "\n";
        return 1;
    }

    const size_t n = rows.size();
    const int num_channels = static_cast<int>(rows[0].channel_bandwidth_gbps.size());
    const bool has_outstanding = rows[0].outstanding_occupancy_pct >= 0.0;
    const bool has_bank = rows[0].bank_utilization_pct >= 0.0;

    // ---- summary ----
    uint64_t total_bytes = 0;
    double sum_bw = 0.0, sum_occ = 0.0, sum_bank = 0.0;
    uint64_t total_hits = 0, total_conflicts = 0, total_empties = 0;
    for (const auto& r : rows) {
        total_bytes += r.bytes_read + r.bytes_written;
        sum_bw += r.avg_bandwidth_gbps;
        if (has_outstanding) sum_occ += std::max(0.0, r.outstanding_occupancy_pct);
        if (has_bank) sum_bank += std::max(0.0, r.bank_utilization_pct);
        total_hits += r.hits; total_conflicts += r.conflicts; total_empties += r.empties;
    }
    uint64_t total_cmds = total_hits + total_conflicts + total_empties;

    ddrtiming::json::Value summary = ddrtiming::json::Value::make_object();
    summary.set("num_windows", static_cast<int64_t>(n));
    summary.set("num_channels", static_cast<int64_t>(num_channels));
    summary.set("total_bytes", static_cast<int64_t>(total_bytes));
    summary.set("avg_bandwidth_gbps", sum_bw / static_cast<double>(n));
    if (has_outstanding) summary.set("avg_outstanding_occupancy_pct", sum_occ / static_cast<double>(n));
    if (has_bank) summary.set("avg_bank_utilization_pct", sum_bank / static_cast<double>(n));
    if (total_cmds > 0) {
        summary.set("hit_rate_pct", static_cast<double>(total_hits) / total_cmds * 100.0);
        summary.set("conflict_rate_pct", static_cast<double>(total_conflicts) / total_cmds * 100.0);
    }

    ddrtiming::json::Array findings;

    auto add_finding = [&](const std::string& type, const std::string& severity, size_t w0, size_t w1, const std::string& detail) {
        ddrtiming::json::Value f = ddrtiming::json::Value::make_object();
        f.set("type", type);
        f.set("severity", severity);
        f.set("window_start", static_cast<int64_t>(w0));
        f.set("window_end", static_cast<int64_t>(w1));
        f.set("start_ns", rows[w0].start_ns);
        f.set("end_ns", rows[w1].start_ns);
        f.set("detail", detail);
        findings.push_back(std::move(f));
    };

    // ---- rule 1: channel imbalance ----
    // A window is "imbalanced" if its busiest channel is carrying traffic
    // while another channel is essentially idle relative to it.
    if (num_channels >= 2) {
        auto imbalanced = [&](size_t i) {
            const auto& cb = rows[i].channel_bandwidth_gbps;
            double mx = *std::max_element(cb.begin(), cb.end());
            if (mx <= 0.0) return false; // nothing moving at all -- not a channel-spread problem
            double mn = *std::min_element(cb.begin(), cb.end());
            return mn < mx * 0.3; // busiest channel carries >3x its quietest peer
        };
        for (auto& run : find_runs(n, 2, imbalanced)) {
            size_t w0 = run.first, w1 = run.second;
            // Report the min/max channel averaged over the run for a concrete number.
            std::vector<double> min_avg(1, 0.0), max_avg(1, 0.0);
            std::vector<double> mins, maxs;
            int busiest_ch = 0, quietest_ch = 0;
            double best_max = -1, best_min = 1e18;
            for (int c = 0; c < num_channels; ++c) {
                double s = 0;
                for (size_t i = w0; i <= w1; ++i) s += rows[i].channel_bandwidth_gbps[c];
                double avg = s / static_cast<double>(w1 - w0 + 1);
                if (avg > best_max) { best_max = avg; busiest_ch = c; }
                if (avg < best_min) { best_min = avg; quietest_ch = c; }
            }
            std::ostringstream d;
            d << "channel " << quietest_ch << " averaged " << best_min << " GB/s while channel " << busiest_ch
              << " averaged " << best_max << " GB/s across windows " << w0 << "-" << w1
              << " (" << fmt_ns(rows[w0].start_ns) << " to " << fmt_ns(rows[w1].start_ns)
              << ") -- traffic is not spread across available channels in this range, even though "
                 "aggregate bandwidth alone would not show this.";
            add_finding("channel_imbalance", "warning", w0, w1, d.str());
        }
    }

    // ---- rule 2: outstanding cap saturation ----
    if (has_outstanding) {
        size_t saturated_count = 0;
        for (const auto& r : rows) if (r.outstanding_occupancy_pct >= 90.0) ++saturated_count;
        double frac = static_cast<double>(saturated_count) / static_cast<double>(n);
        if (frac >= 0.5) {
            std::ostringstream d;
            d << "outstanding_occupancy_pct was >= 90% in " << saturated_count << "/" << n << " windows ("
              << (frac * 100.0) << "%) -- the per-(core, axi_id) outstanding pool stayed at or near "
                 "max_outstanding_per_id for most of the trace, i.e. the stream never ran dry. That alone "
                 "doesn't establish max_outstanding_per_id as the throughput limiter -- a workload with no "
                 "supply shortage looks identical whether or not the cap is the binding constraint "
                 "elsewhere in the pipeline; only a re-run at a different cap, compared on achieved "
                 "bandwidth, can establish that.";
            add_finding("outstanding_saturated", "warning", 0, n - 1, d.str());
        }
    }

    // ---- rule 3: bank underutilization ----
    if (has_bank) {
        double avg_bank = sum_bank / static_cast<double>(n);
        if (avg_bank < 25.0) {
            std::ostringstream d;
            d << "bank_utilization_pct averaged " << avg_bank << "% across the whole trace -- traffic is "
                 "concentrated on a small fraction of available banks. This caps the number of banks "
                 "available for concurrent scheduling, independent of bus or outstanding-cap saturation.";
            add_finding("bank_underutilized", "info", 0, n - 1, d.str());
        }
    }

    // ---- rule 4: bandwidth drop relative to the trace's own median ----
    {
        std::vector<double> bws;
        for (const auto& r : rows) bws.push_back(r.avg_bandwidth_gbps);
        double med = median(bws);
        if (med > 0.0) {
            auto is_drop = [&](size_t i) { return rows[i].avg_bandwidth_gbps < med * 0.4; };
            for (auto& run : find_runs(n, 3, is_drop)) {
                size_t w0 = run.first, w1 = run.second;
                std::vector<double> seg;
                uint64_t seg_hits = 0, seg_conf = 0, seg_emp = 0;
                for (size_t i = w0; i <= w1; ++i) {
                    seg.push_back(rows[i].avg_bandwidth_gbps);
                    seg_hits += rows[i].hits; seg_conf += rows[i].conflicts; seg_emp += rows[i].empties;
                }
                double seg_avg = mean(seg);
                uint64_t seg_cmds = seg_hits + seg_conf + seg_emp;
                std::ostringstream d;
                d << "bandwidth averaged " << seg_avg << " GB/s across windows " << w0 << "-" << w1
                  << " (" << fmt_ns(rows[w0].start_ns) << " to " << fmt_ns(rows[w1].start_ns)
                  << "), vs the trace's overall median of " << med << " GB/s ("
                  << (seg_avg / med * 100.0) << "% of median).";
                if (seg_cmds > 0) {
                    double conf_pct = static_cast<double>(seg_conf) / seg_cmds * 100.0;
                    d << " Row-conflict rate in this range was " << conf_pct << "%.";
                }
                add_finding("bandwidth_drop", "warning", w0, w1, d.str());
            }
        }
    }

    ddrtiming::json::Value root = ddrtiming::json::Value::make_object();
    root.set("summary", std::move(summary));
    root.set("findings", ddrtiming::json::Value(std::move(findings)));
    if (findings.empty()) {
        // ddrtiming::json::Value(Array) above already reflects size 0 correctly;
        // this branch is just documentation that "no findings" is a valid,
        // meaningful result (the trace looked healthy by these rules), not an error.
    }

    std::string out_text = root.dump(2);
    if (out_path.empty()) {
        std::cout << out_text << "\n";
    } else {
        std::ofstream f(out_path, std::ios::binary);
        if (!f) { std::cerr << "error: cannot open output path: " << out_path << "\n"; return 1; }
        f << out_text;
        std::cout << "wrote " << out_path << " (" << findings.size() << " finding" << (findings.size() == 1 ? "" : "s") << ")\n";
    }
    return 0;
}
