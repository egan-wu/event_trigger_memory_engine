// Standalone analyzer for windowed-history CSVs (the same files
// --windowed-csv writes): reads the CSV alone, no engine/config needed, and
// emits an objective statistical export as JSON -- built for a caller (an AI
// agent, a CI check, a script piping into jq) that needs to reason about a
// trace without visually reading a chart.
//
// Design note: this deliberately does NOT emit domain-specific "findings"
// with severity labels or fixed thresholds (an earlier version did). A
// threshold like "bank_utilization_pct < 25% is bad" doesn't generalize
// across configs with different channel/bank counts or workload shapes, so
// baking one in makes the tool wrong for some caller some of the time.
// Instead this reports: whole-trace percentiles, the top/bottom-K most
// extreme windows per metric (so a rare anomaly is never averaged away),
// and a fixed-size bucketed time series (mean/median/min/max per bucket, so
// short bursts survive downsampling even on a very long trace). Judging
// what counts as "bad" is left entirely to the caller -- this tool's job is
// correct numbers, not verdicts. An optional --baseline compares two traces
// (e.g. before/after a config change) at the summary-statistic level.
//
// Usage: windowed_history_analyze --csv <path> [--baseline <path>] [--out <path>]
// (writes to stdout if --out is omitted)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "../src/core/json.hpp"

namespace {

constexpr size_t kTargetBuckets = 150;
constexpr size_t kExtremesK = 10;

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

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

// v must already be sorted ascending. Linear-interpolated percentile, p in [0,100].
double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    if (sorted.size() == 1) return sorted[0];
    double idx = p / 100.0 * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

// Whole-series distribution: mean, stddev, and a fixed percentile ladder.
// Pure statistics -- no thresholds, no notion of "good"/"bad".
ddrtiming::json::Value metric_summary_stats(const std::vector<double>& values) {
    ddrtiming::json::Value v = ddrtiming::json::Value::make_object();
    if (values.empty()) return v;
    double m = mean(values);
    double var = 0.0;
    for (double x : values) var += (x - m) * (x - m);
    var /= static_cast<double>(values.size());
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    v.set("mean", m);
    v.set("stddev", std::sqrt(var));
    for (double p : {0.0, 1.0, 5.0, 25.0, 50.0, 75.0, 95.0, 99.0, 100.0}) {
        std::ostringstream key;
        key << "p" << static_cast<int>(p);
        v.set(key.str(), percentile(sorted, p));
    }
    return v;
}

// The K lowest and K highest windows for one metric, by raw value, with
// enough context (window_index/start_ns/end_ns) to go read that exact row
// out of the original CSV. A global sort survives even when every bucket in
// the time series happens to have wide min/max spread for unrelated reasons.
ddrtiming::json::Value metric_extremes(const std::vector<Row>& rows, const std::vector<double>& values,
                                        double window_ns, size_t k) {
    size_t n = values.size();
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return values[a] < values[b]; });

    auto make_list = [&](bool lowest) {
        ddrtiming::json::Array arr;
        size_t take = std::min(k, n);
        for (size_t i = 0; i < take; ++i) {
            size_t idx = lowest ? order[i] : order[n - 1 - i];
            ddrtiming::json::Value e = ddrtiming::json::Value::make_object();
            e.set("window_index", static_cast<int64_t>(rows[idx].window_index));
            e.set("start_ns", rows[idx].start_ns);
            e.set("end_ns", rows[idx].start_ns + window_ns);
            e.set("value", values[idx]);
            arr.push_back(std::move(e));
        }
        return ddrtiming::json::Value(std::move(arr));
    };

    ddrtiming::json::Value v = ddrtiming::json::Value::make_object();
    v.set("lowest", make_list(true));
    v.set("highest", make_list(false));
    return v;
}

// One bucket's stats for one metric: mean and median (so a caller can tell
// "a couple of outliers" (mean != median) from "the whole bucket moved"),
// plus the exact min/max window within the bucket so a short burst inside
// an otherwise-flat bucket is never silently averaged away.
ddrtiming::json::Value bucket_metric_stats(const std::vector<Row>& rows, const std::vector<double>& values,
                                            size_t w0, size_t w1) {
    double mn = values[w0], mx = values[w0];
    size_t mn_i = w0, mx_i = w0;
    std::vector<double> slice(values.begin() + static_cast<long>(w0), values.begin() + static_cast<long>(w1) + 1);
    for (size_t i = w0; i <= w1; ++i) {
        if (values[i] < mn) { mn = values[i]; mn_i = i; }
        if (values[i] > mx) { mx = values[i]; mx_i = i; }
    }
    std::vector<double> sorted = slice;
    std::sort(sorted.begin(), sorted.end());

    ddrtiming::json::Value v = ddrtiming::json::Value::make_object();
    v.set("mean", mean(slice));
    v.set("p50", percentile(sorted, 50.0));
    ddrtiming::json::Value minv = ddrtiming::json::Value::make_object();
    minv.set("value", mn);
    minv.set("window_index", static_cast<int64_t>(rows[mn_i].window_index));
    ddrtiming::json::Value maxv = ddrtiming::json::Value::make_object();
    maxv.set("value", mx);
    maxv.set("window_index", static_cast<int64_t>(rows[mx_i].window_index));
    v.set("min", std::move(minv));
    v.set("max", std::move(maxv));
    return v;
}

// One loaded trace plus everything derived from it that summary/extremes/
// series all need, computed once.
struct TraceData {
    std::vector<Row> rows;
    int num_channels = 0;
    bool has_outstanding = false;
    bool has_bank = false;
    double window_ns = 0.0; // 0 if undeterminable (fewer than 2 windows)
    uint64_t total_bytes = 0;
    double hit_rate_pct = 0.0, conflict_rate_pct = 0.0;
    std::vector<std::pair<std::string, std::vector<double>>> base_metrics; // name -> per-window values
    std::vector<std::vector<double>> channel_metrics; // per channel -> per-window avg_bandwidth_gbps
};

TraceData load_trace(const std::string& path) {
    TraceData t;
    t.rows = parse_csv(path);
    if (t.rows.empty()) throw std::runtime_error("no data rows in " + path);
    size_t n = t.rows.size();

    t.num_channels = static_cast<int>(t.rows[0].channel_bandwidth_gbps.size());
    t.has_outstanding = t.rows[0].outstanding_occupancy_pct >= 0.0;
    t.has_bank = t.rows[0].bank_utilization_pct >= 0.0;
    // Real window duration, not the *start* of the last window -- a bucket/
    // extremes entry's end_ns needs this to name the window's actual end.
    t.window_ns = (n >= 2) ? (t.rows[1].start_ns - t.rows[0].start_ns) : 0.0;

    std::vector<double> bw, occ, bank;
    bw.reserve(n);
    uint64_t total_hits = 0, total_conf = 0, total_emp = 0;
    for (const auto& r : t.rows) {
        bw.push_back(r.avg_bandwidth_gbps);
        if (t.has_outstanding) occ.push_back(std::max(0.0, r.outstanding_occupancy_pct));
        if (t.has_bank) bank.push_back(std::max(0.0, r.bank_utilization_pct));
        t.total_bytes += r.bytes_read + r.bytes_written;
        total_hits += r.hits; total_conf += r.conflicts; total_emp += r.empties;
    }
    uint64_t total_cmds = total_hits + total_conf + total_emp;
    if (total_cmds > 0) {
        t.hit_rate_pct = static_cast<double>(total_hits) / static_cast<double>(total_cmds) * 100.0;
        t.conflict_rate_pct = static_cast<double>(total_conf) / static_cast<double>(total_cmds) * 100.0;
    }

    t.base_metrics.emplace_back("avg_bandwidth_gbps", std::move(bw));
    if (t.has_outstanding) t.base_metrics.emplace_back("outstanding_occupancy_pct", std::move(occ));
    if (t.has_bank) t.base_metrics.emplace_back("bank_utilization_pct", std::move(bank));

    t.channel_metrics.resize(static_cast<size_t>(t.num_channels));
    for (int c = 0; c < t.num_channels; ++c) {
        t.channel_metrics[static_cast<size_t>(c)].reserve(n);
        for (const auto& r : t.rows) t.channel_metrics[static_cast<size_t>(c)].push_back(r.channel_bandwidth_gbps[static_cast<size_t>(c)]);
    }
    return t;
}

ddrtiming::json::Value build_summary(const TraceData& t) {
    ddrtiming::json::Value summary = ddrtiming::json::Value::make_object();
    summary.set("num_windows", static_cast<int64_t>(t.rows.size()));
    summary.set("num_channels", static_cast<int64_t>(t.num_channels));
    summary.set("window_ns", t.window_ns);
    summary.set("total_bytes", static_cast<int64_t>(t.total_bytes));
    summary.set("hit_rate_pct", t.hit_rate_pct);
    summary.set("conflict_rate_pct", t.conflict_rate_pct);

    ddrtiming::json::Value metrics = ddrtiming::json::Value::make_object();
    for (const auto& [name, values] : t.base_metrics) metrics.set(name, metric_summary_stats(values));
    summary.set("metrics", std::move(metrics));

    ddrtiming::json::Array channels;
    for (int c = 0; c < t.num_channels; ++c) {
        ddrtiming::json::Value ch = ddrtiming::json::Value::make_object();
        ch.set("index", static_cast<int64_t>(c));
        ch.set("avg_bandwidth_gbps", metric_summary_stats(t.channel_metrics[static_cast<size_t>(c)]));
        channels.push_back(std::move(ch));
    }
    summary.set("channels", ddrtiming::json::Value(std::move(channels)));
    return summary;
}

ddrtiming::json::Value build_extremes(const TraceData& t, size_t k) {
    ddrtiming::json::Value extremes = ddrtiming::json::Value::make_object();
    for (const auto& [name, values] : t.base_metrics) extremes.set(name, metric_extremes(t.rows, values, t.window_ns, k));

    ddrtiming::json::Array channels;
    for (int c = 0; c < t.num_channels; ++c) {
        ddrtiming::json::Value ch = ddrtiming::json::Value::make_object();
        ch.set("index", static_cast<int64_t>(c));
        ch.set("avg_bandwidth_gbps", metric_extremes(t.rows, t.channel_metrics[static_cast<size_t>(c)], t.window_ns, k));
        channels.push_back(std::move(ch));
    }
    extremes.set("channels", ddrtiming::json::Value(std::move(channels)));
    return extremes;
}

ddrtiming::json::Value build_series(const TraceData& t, size_t target_buckets) {
    size_t n = t.rows.size();
    size_t bucket_windows = std::max<size_t>(1, (n + target_buckets - 1) / target_buckets);

    ddrtiming::json::Value series = ddrtiming::json::Value::make_object();
    series.set("bucket_windows", static_cast<int64_t>(bucket_windows));

    ddrtiming::json::Array buckets;
    for (size_t w0 = 0; w0 < n; w0 += bucket_windows) {
        size_t w1 = std::min(n - 1, w0 + bucket_windows - 1);
        ddrtiming::json::Value b = ddrtiming::json::Value::make_object();
        b.set("window_start", static_cast<int64_t>(t.rows[w0].window_index));
        b.set("window_end", static_cast<int64_t>(t.rows[w1].window_index));
        b.set("start_ns", t.rows[w0].start_ns);
        b.set("end_ns", t.rows[w1].start_ns + t.window_ns);

        ddrtiming::json::Value metrics = ddrtiming::json::Value::make_object();
        for (const auto& [name, values] : t.base_metrics) metrics.set(name, bucket_metric_stats(t.rows, values, w0, w1));
        b.set("metrics", std::move(metrics));

        ddrtiming::json::Array channels;
        for (int c = 0; c < t.num_channels; ++c) {
            ddrtiming::json::Value ch = ddrtiming::json::Value::make_object();
            ch.set("index", static_cast<int64_t>(c));
            ch.set("avg_bandwidth_gbps", bucket_metric_stats(t.rows, t.channel_metrics[static_cast<size_t>(c)], w0, w1));
            channels.push_back(std::move(ch));
        }
        b.set("channels", ddrtiming::json::Value(std::move(channels)));

        buckets.push_back(std::move(b));
    }
    series.set("buckets", ddrtiming::json::Value(std::move(buckets)));
    return series;
}

// Summary-level A/B comparison against a second trace (e.g. the same
// workload before/after a config change). Deliberately summary-only, not
// per-bucket: the two traces can have different lengths/bucket_windows, and
// aligning buckets across that mismatch is a separate problem this doesn't
// try to solve -- the aggregate distributions are the decisive evidence for
// "did this change help" either way.
ddrtiming::json::Value build_baseline_delta(const ddrtiming::json::Value& target_summary,
                                             const ddrtiming::json::Value& baseline_summary) {
    ddrtiming::json::Value delta = ddrtiming::json::Value::make_object();
    delta.set("hit_rate_pct", target_summary["hit_rate_pct"].as_double() - baseline_summary["hit_rate_pct"].as_double());
    delta.set("conflict_rate_pct", target_summary["conflict_rate_pct"].as_double() - baseline_summary["conflict_rate_pct"].as_double());

    const auto& tm = target_summary["metrics"];
    const auto& bm = baseline_summary["metrics"];
    ddrtiming::json::Value metrics_delta = ddrtiming::json::Value::make_object();
    for (const auto& [name, tval] : tm.object_items()) {
        if (!bm.contains(name)) continue; // metric only present in one trace (e.g. differing config) -- skip rather than guess
        double tmean = tval["mean"].as_double();
        double bmean = bm[name]["mean"].as_double();
        ddrtiming::json::Value d = ddrtiming::json::Value::make_object();
        d.set("target_mean", tmean);
        d.set("baseline_mean", bmean);
        d.set("delta", tmean - bmean);
        d.set("delta_pct", bmean != 0.0 ? (tmean - bmean) / bmean * 100.0 : 0.0);
        metrics_delta.set(name, std::move(d));
    }
    delta.set("metrics", std::move(metrics_delta));
    return delta;
}

} // namespace

int main(int argc, char** argv) {
    std::string csv_path, out_path, baseline_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << flag << "\n"; std::exit(1); }
            return argv[++i];
        };
        if (arg == "--csv") csv_path = need("--csv");
        else if (arg == "--baseline") baseline_path = need("--baseline");
        else if (arg == "--out") out_path = need("--out");
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: windowed_history_analyze --csv <windowed_history.csv> "
                         "[--baseline <other_windowed_history.csv>] [--out <findings.json>]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 1;
        }
    }
    if (csv_path.empty()) {
        std::cerr << "Usage: windowed_history_analyze --csv <windowed_history.csv> "
                     "[--baseline <other_windowed_history.csv>] [--out <findings.json>]\n";
        return 1;
    }

    TraceData target;
    try {
        target = load_trace(csv_path);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    ddrtiming::json::Value summary = build_summary(target);
    ddrtiming::json::Value root = ddrtiming::json::Value::make_object();
    root.set("summary", summary); // copy -- `summary` is still needed below if --baseline is given
    root.set("extremes", build_extremes(target, kExtremesK));
    root.set("series", build_series(target, kTargetBuckets));

    if (!baseline_path.empty()) {
        TraceData baseline;
        try {
            baseline = load_trace(baseline_path);
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 1;
        }
        root.set("baseline_delta", build_baseline_delta(summary, build_summary(baseline)));
    }

    std::string out_text = root.dump(2);
    if (out_path.empty()) {
        std::cout << out_text << "\n";
    } else {
        std::ofstream f(out_path, std::ios::binary);
        if (!f) { std::cerr << "error: cannot open output path: " << out_path << "\n"; return 1; }
        f << out_text;
        std::cout << "wrote " << out_path << "\n";
    }
    return 0;
}
