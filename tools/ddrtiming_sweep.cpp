// Parameter-sweep tool: runs the engine over the Cartesian product of a set
// of config-parameter "axes" (dot-separated paths into a base config JSON)
// against a fixed set of AXI logs, and reports the requested SummaryStats
// metrics per point -- turning "hand-edit config.json, re-run the CLI, read
// the output by eye" into a single sweep table. See README S4.5 for the
// full spec format, CLI flags, and output columns.
//
// This project's own principle carries over here too: numbers only, never
// verdicts. A point whose overridden config fails DdrcConfig::validate() is
// not a crash -- it is itself sweep information (e.g. "this corner of the
// design space is not physically realizable"), so it is reported as a row
// with its `error` column filled in and its metrics left blank, and the
// sweep keeps going. Only a problem with the spec/IO itself (a malformed
// spec, a missing base_config/log file, an unwritable --out path) aborts
// the whole run.
//
// Usage:
//   ddrtiming_sweep --spec <sweep.json> --out <report.csv>
//                    [--jobs N] [--baseline-index N]
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../src/core/config.hpp"
#include "../src/core/engine.hpp"
#include "../src/core/json.hpp"
#include "../src/core/log_parser.hpp"

namespace fs = std::filesystem;
using ddrtiming::DdrcConfig;
using ddrtiming::Engine;
using ddrtiming::SummaryStats;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "error: " << msg << "\n";
    std::exit(1);
}

// ---------------------------------------------------------------------
// json::Value helpers.
//
// The hand-rolled json::Value (src/core/json.hpp) shares nested Array/
// Object storage across copies via shared_ptr -- a `Value` copy is
// shallow. That is fine for read-only access, but it means a naive "copy
// the base config, then mutate the copy" per sweep point would silently
// mutate the SAME underlying nested objects across every point -- a real
// hazard here since points run concurrently on a thread pool. deep_copy()
// produces a fully independent tree (fresh storage at every level) so each
// point can safely own and mutate its own config.
// ---------------------------------------------------------------------

ddrtiming::json::Value deep_copy(const ddrtiming::json::Value& v) {
    using ddrtiming::json::Type;
    using ddrtiming::json::Value;
    switch (v.type()) {
        case Type::Array: {
            ddrtiming::json::Array out;
            out.reserve(v.array_items().size());
            for (const auto& item : v.array_items()) out.push_back(deep_copy(item));
            return Value(std::move(out));
        }
        case Type::Object: {
            ddrtiming::json::Object out;
            out.reserve(v.object_items().size());
            for (const auto& kv : v.object_items()) out.emplace_back(kv.first, deep_copy(kv.second));
            return Value(std::move(out));
        }
        case Type::Bool: return Value(v.as_bool());
        case Type::Number: return Value(v.as_double());
        case Type::String: return Value(v.as_string());
        case Type::Null:
        default: return Value();
    }
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t dot = path.find('.', start);
        if (dot == std::string::npos) { parts.push_back(path.substr(start)); break; }
        parts.push_back(path.substr(start, dot - start));
        start = dot + 1;
    }
    return parts;
}

// Sets `node` at the dot-path named by parts[idx..] to `leaf`, creating any
// missing intermediate objects along the way (README S4.5: "setting a path
// that doesn't exist in the base JSON should create it"). Always rebuilds
// bottom-up and re-assigns at every level via Value::set() -- correct
// regardless of whether an intermediate key already existed (and so already
// aliases shared storage with `node`, per the deep_copy() note above) or had
// to be freshly created here. Callers must apply this to an already-
// deep-copied tree, never to a tree still shared with another point/thread.
void set_json_path(ddrtiming::json::Value& node, const std::vector<std::string>& parts, size_t idx,
                    const ddrtiming::json::Value& leaf) {
    const std::string& key = parts[idx];
    if (idx + 1 == parts.size()) {
        node.set(key, leaf);
        return;
    }
    ddrtiming::json::Value child = node.contains(key) ? node[key] : ddrtiming::json::Value::make_object();
    set_json_path(child, parts, idx + 1, leaf);
    node.set(key, std::move(child));
}

// Resolves a spec-relative path: relative to the spec file's own directory
// first, falling back to the current working directory (README S4.5).
fs::path resolve_spec_path(const fs::path& spec_dir, const std::string& raw) {
    fs::path from_spec_dir = spec_dir / raw;
    if (fs::exists(from_spec_dir)) return from_spec_dir;
    fs::path from_cwd(raw);
    if (fs::exists(from_cwd)) return from_cwd;
    // Neither exists -- return the spec-dir-relative candidate so the
    // resulting "not found" error names the path actually tried first.
    return from_spec_dir;
}

std::string csv_escape(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

// "Numbers printed with enough precision (%.6g is fine)" -- README S4.5.
// Used for CSV/table cells; the JSON writer instead stores real json::Value
// numbers and lets Value::dump() format them (which already special-cases
// whole numbers, e.g. byte counts, to print exactly rather than in %g form).
std::string format_g6(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

// Renders one axis value (number/string/bool/array -- README S4.5 allows
// any of these for an axis's candidate list) as a single CSV/table cell.
std::string cell_string(const ddrtiming::json::Value& v) {
    using ddrtiming::json::Type;
    switch (v.type()) {
        case Type::Number: return format_g6(v.as_double());
        case Type::String: return v.as_string();
        case Type::Bool: return v.as_bool() ? "true" : "false";
        case Type::Array: {
            std::string s = "[";
            bool first = true;
            for (const auto& item : v.array_items()) {
                if (!first) s += ";";
                first = false;
                s += cell_string(item);
            }
            s += "]";
            return s;
        }
        case Type::Object: return v.dump(0);
        case Type::Null:
        default: return "";
    }
}

// ---------------------------------------------------------------------
// SummaryStats field access by name -- every scalar numeric field, in the
// order declared in src/core/engine.hpp, so "report" can default to "all
// of them" per README S4.5.
// ---------------------------------------------------------------------

const std::vector<std::string>& all_summary_fields() {
    static const std::vector<std::string> fields = {
        "total_txns", "total_bytes", "total_dram_bytes", "total_cycles", "sim_time_ns",
        "avg_bandwidth_gbps", "avg_dram_bandwidth_gbps", "peak_bandwidth_gbps",
        "bandwidth_utilization_pct", "burst_efficiency_pct", "avg_latency_ns",
        "page_hit_rate_pct", "row_conflict_rate_pct", "row_empty_rate_pct",
        "refresh_overhead_pct", "turnaround_overhead_pct", "bankgroup_reuse_rate_pct",
        "mapped_address_bits", "high_address_regions",
        // Bus-time attribution: these are what make a sweep readable as
        // "this parameter moved N% of bus time from X to Y" rather than
        // just "bandwidth went up".
        "attr_data_pct", "attr_row_miss_exposed_pct", "attr_refresh_pct",
        "attr_turnaround_pct", "attr_twtr_pct", "attr_tccd_l_excess_pct",
        "attr_frontend_idle_pct", "attr_other_pct", "rw_direction_switches",
        "ceiling_refresh_pct", "ceiling_tccd_l_gbps", "ceiling_tfaw_gbps",
        "headroom_pct", "channel_imbalance_ratio",
    };
    return fields;
}

bool get_summary_field(const SummaryStats& s, const std::string& name, double& out) {
    if (name == "total_txns") { out = static_cast<double>(s.total_txns); return true; }
    if (name == "total_bytes") { out = static_cast<double>(s.total_bytes); return true; }
    if (name == "total_dram_bytes") { out = static_cast<double>(s.total_dram_bytes); return true; }
    if (name == "total_cycles") { out = static_cast<double>(s.total_cycles); return true; }
    if (name == "sim_time_ns") { out = s.sim_time_ns; return true; }
    if (name == "avg_bandwidth_gbps") { out = s.avg_bandwidth_gbps; return true; }
    if (name == "avg_dram_bandwidth_gbps") { out = s.avg_dram_bandwidth_gbps; return true; }
    if (name == "peak_bandwidth_gbps") { out = s.peak_bandwidth_gbps; return true; }
    if (name == "bandwidth_utilization_pct") { out = s.bandwidth_utilization_pct; return true; }
    if (name == "burst_efficiency_pct") { out = s.burst_efficiency_pct; return true; }
    if (name == "avg_latency_ns") { out = s.avg_latency_ns; return true; }
    if (name == "page_hit_rate_pct") { out = s.page_hit_rate_pct; return true; }
    if (name == "row_conflict_rate_pct") { out = s.row_conflict_rate_pct; return true; }
    if (name == "row_empty_rate_pct") { out = s.row_empty_rate_pct; return true; }
    if (name == "refresh_overhead_pct") { out = s.refresh_overhead_pct; return true; }
    if (name == "turnaround_overhead_pct") { out = s.turnaround_overhead_pct; return true; }
    if (name == "bankgroup_reuse_rate_pct") { out = s.bankgroup_reuse_rate_pct; return true; }
    if (name == "mapped_address_bits") { out = static_cast<double>(s.mapped_address_bits); return true; }
    if (name == "high_address_regions") { out = static_cast<double>(s.high_address_regions); return true; }
    if (name == "attr_data_pct") { out = s.attr_data_pct; return true; }
    if (name == "attr_row_miss_exposed_pct") { out = s.attr_row_miss_exposed_pct; return true; }
    if (name == "attr_refresh_pct") { out = s.attr_refresh_pct; return true; }
    if (name == "attr_turnaround_pct") { out = s.attr_turnaround_pct; return true; }
    if (name == "attr_twtr_pct") { out = s.attr_twtr_pct; return true; }
    if (name == "attr_tccd_l_excess_pct") { out = s.attr_tccd_l_excess_pct; return true; }
    if (name == "attr_frontend_idle_pct") { out = s.attr_frontend_idle_pct; return true; }
    if (name == "attr_other_pct") { out = s.attr_other_pct; return true; }
    if (name == "rw_direction_switches") { out = static_cast<double>(s.rw_direction_switches); return true; }
    if (name == "ceiling_refresh_pct") { out = s.ceiling_refresh_pct; return true; }
    if (name == "ceiling_tccd_l_gbps") { out = s.ceiling_tccd_l_gbps; return true; }
    if (name == "ceiling_tfaw_gbps") { out = s.ceiling_tfaw_gbps; return true; }
    if (name == "headroom_pct") { out = s.headroom_pct; return true; }
    if (name == "channel_imbalance_ratio") { out = s.channel_imbalance_ratio; return true; }
    return false;
}

// ---------------------------------------------------------------------
// Sweep spec + point model
// ---------------------------------------------------------------------

struct AxisSpec {
    std::string path;
    ddrtiming::json::Array values;
};

struct SweepSpec {
    ddrtiming::json::Value raw; // the parsed spec file, echoed verbatim into the JSON report
    fs::path base_config_path;
    std::vector<fs::path> log_paths;
    std::vector<AxisSpec> axes;
    std::vector<std::string> report_fields;
};

SweepSpec load_spec(const std::string& spec_path) {
    SweepSpec spec;
    try {
        spec.raw = ddrtiming::json::parse_file(spec_path);
    } catch (const std::exception& e) {
        fail("cannot parse spec '" + spec_path + "': " + e.what());
    }

    if (!spec.raw.contains("base_config") || spec.raw["base_config"].as_string().empty()) {
        fail("spec is missing required string field \"base_config\"");
    }
    if (!spec.raw.contains("logs") || !spec.raw["logs"].is_array() || spec.raw["logs"].size() == 0) {
        fail("spec is missing required non-empty array field \"logs\"");
    }
    if (!spec.raw.contains("axes") || !spec.raw["axes"].is_object()) {
        fail("spec is missing required object field \"axes\" (use {} for a single no-override point)");
    }

    fs::path spec_dir = fs::absolute(fs::path(spec_path)).parent_path();

    spec.base_config_path = resolve_spec_path(spec_dir, spec.raw["base_config"].as_string());
    if (!fs::exists(spec.base_config_path)) {
        fail("base_config not found in spec dir or cwd: " + spec.raw["base_config"].as_string());
    }

    for (const auto& item : spec.raw["logs"].array_items()) {
        std::string raw_log = item.as_string();
        fs::path resolved = resolve_spec_path(spec_dir, raw_log);
        if (!fs::exists(resolved)) {
            fail("log file not found in spec dir or cwd: " + raw_log);
        }
        spec.log_paths.push_back(resolved);
    }

    // Axis iteration order must match the spec's own key order ("first axis
    // slowest-varying", README S4.5) -- json::Object is insertion-ordered
    // (see json.hpp), so a plain walk of object_items() preserves it.
    for (const auto& kv : spec.raw["axes"].object_items()) {
        if (!kv.second.is_array() || kv.second.size() == 0) {
            fail("axes[\"" + kv.first + "\"] must be a non-empty array of candidate values");
        }
        AxisSpec axis;
        axis.path = kv.first;
        axis.values = kv.second.array_items();
        spec.axes.push_back(std::move(axis));
    }

    if (spec.raw.contains("report")) {
        for (const auto& item : spec.raw["report"].array_items()) {
            std::string name = item.as_string();
            double dummy = 0.0;
            if (!get_summary_field(SummaryStats{}, name, dummy)) {
                std::ostringstream known;
                for (const auto& f : all_summary_fields()) known << f << " ";
                fail("report metric \"" + name + "\" is not a SummaryStats field. Known fields: " + known.str());
            }
            spec.report_fields.push_back(name);
        }
    } else {
        spec.report_fields = all_summary_fields();
    }

    // Fail fast on a broken/missing log *format* now (a spec/IO problem,
    // identical for every point since all points share the same logs)
    // rather than letting every point in the sweep independently rediscover
    // the same parse error. Each point still reparses its own logs itself
    // at run time (see run_point) for thread isolation -- this is purely an
    // early sanity check.
    for (size_t i = 0; i < spec.log_paths.size(); ++i) {
        try {
            (void)ddrtiming::parse_axi_log_file(spec.log_paths[i].string(), static_cast<int>(i));
        } catch (const std::exception& e) {
            fail("cannot parse log '" + spec.log_paths[i].string() + "': " + e.what());
        }
    }

    return spec;
}

// Axis sizes/strides for the Cartesian product, computed once. Axis 0 is
// slowest-varying (README S4.5), i.e. standard row-major nested-loop order
// with axis 0 outermost: stride[k] = product of axis_sizes[k+1..end].
struct SweepPlan {
    std::vector<size_t> axis_sizes;
    std::vector<size_t> strides;
    size_t total_points = 1;
};

SweepPlan build_plan(const std::vector<AxisSpec>& axes) {
    SweepPlan plan;
    plan.axis_sizes.resize(axes.size());
    for (size_t i = 0; i < axes.size(); ++i) plan.axis_sizes[i] = axes[i].values.size();
    plan.strides.assign(axes.size(), 1);
    for (size_t i = axes.size(); i-- > 1;) {
        plan.strides[i - 1] = plan.strides[i] * plan.axis_sizes[i];
    }
    plan.total_points = 1;
    for (size_t n : plan.axis_sizes) plan.total_points *= n;
    return plan;
}

std::vector<size_t> point_axis_indices(const SweepPlan& plan, size_t point_index) {
    std::vector<size_t> idxs(plan.axis_sizes.size());
    for (size_t k = 0; k < idxs.size(); ++k) {
        idxs[k] = (point_index / plan.strides[k]) % plan.axis_sizes[k];
    }
    return idxs;
}

struct PointResult {
    std::vector<ddrtiming::json::Value> axis_values; // one per axis, the value used at this point
    bool ok = false;
    std::string error;
    std::vector<double> metrics; // valid only when ok, one per spec.report_fields entry
};

// Runs exactly one sweep point: deep-copies the base config, applies this
// point's axis overrides, writes it to a temp file under tmp_dir, then
// loads it through DdrcConfig::load_from_file() so validation runs exactly
// as it does for the CLI (README S4.5 "per point"). Any failure anywhere in
// this process -- validation, log parsing, engine execution -- is caught
// here and reported as this point's `error`; it never propagates out, so
// one bad point can never abort the rest of the sweep.
PointResult run_point(size_t point_index, const ddrtiming::json::Value& base_root, const SweepSpec& spec,
                       const SweepPlan& plan, const fs::path& tmp_dir) {
    PointResult r;
    std::vector<size_t> idxs = point_axis_indices(plan, point_index);
    for (size_t a = 0; a < spec.axes.size(); ++a) {
        r.axis_values.push_back(spec.axes[a].values[idxs[a]]);
    }

    try {
        ddrtiming::json::Value point_cfg = deep_copy(base_root);
        for (size_t a = 0; a < spec.axes.size(); ++a) {
            std::vector<std::string> parts = split_path(spec.axes[a].path);
            set_json_path(point_cfg, parts, 0, r.axis_values[a]);
        }

        fs::path tmp_file = tmp_dir / ("point_" + std::to_string(point_index) + ".json");
        {
            std::ofstream f(tmp_file, std::ios::binary);
            if (!f) throw std::runtime_error("cannot write temp config file: " + tmp_file.string());
            f << point_cfg.dump(2);
        }

        DdrcConfig cfg = DdrcConfig::load_from_file(tmp_file.string()); // runs validate()
        Engine engine(std::move(cfg));
        for (size_t i = 0; i < spec.log_paths.size(); ++i) {
            int core_id = static_cast<int>(i);
            auto entries = ddrtiming::parse_axi_log_file(spec.log_paths[i].string(), core_id);
            for (const auto& e : entries) {
                if (e.is_barrier) engine.push_barrier(core_id);
                else engine.push_txn(e.txn);
            }
        }
        engine.run();

        const SummaryStats& s = engine.summary();
        r.metrics.reserve(spec.report_fields.size());
        for (const auto& name : spec.report_fields) {
            double v = 0.0;
            get_summary_field(s, name, v); // name already validated against all_summary_fields() at spec load
            r.metrics.push_back(v);
        }
        r.ok = true;
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
    }
    return r;
}

// Runs every point across a simple thread pool: each thread repeatedly
// claims the next unclaimed point index via an atomic counter and writes
// only to that index of a pre-sized results vector, so no locking is
// needed. Safe because Engine carries no global state and json::parse_file/
// parse_axi_log_file touch no shared mutable state (each point deep-copies
// its own config and reparses its own logs) -- see README S4.5.
std::vector<PointResult> run_all_points(const ddrtiming::json::Value& base_root, const SweepSpec& spec,
                                         const SweepPlan& plan, const fs::path& tmp_dir, unsigned jobs) {
    std::vector<PointResult> results(plan.total_points);
    std::atomic<size_t> next{0};
    unsigned num_threads = std::max(1u, jobs);
    num_threads = static_cast<unsigned>(std::min<size_t>(num_threads, std::max<size_t>(1, plan.total_points)));

    auto worker = [&]() {
        while (true) {
            size_t p = next.fetch_add(1);
            if (p >= plan.total_points) break;
            results[p] = run_point(p, base_root, spec, plan, tmp_dir);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (unsigned t = 0; t < num_threads; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    return results;
}

bool baseline_available(const std::vector<PointResult>& results, int baseline_index) {
    return baseline_index >= 0 && static_cast<size_t>(baseline_index) < results.size() &&
           results[static_cast<size_t>(baseline_index)].ok;
}

void write_csv(const std::string& out_path, const SweepSpec& spec, const std::vector<PointResult>& results,
               int baseline_index) {
    std::ofstream f(out_path, std::ios::binary);
    if (!f) fail("cannot open --out path for writing: " + out_path);

    bool has_baseline = baseline_available(results, baseline_index);

    std::vector<std::string> header;
    for (const auto& ax : spec.axes) header.push_back(ax.path);
    for (const auto& m : spec.report_fields) header.push_back(m);
    if (has_baseline) {
        for (const auto& m : spec.report_fields) header.push_back(m + "_delta_pct");
    }
    header.push_back("error");
    for (size_t i = 0; i < header.size(); ++i) {
        if (i) f << ",";
        f << csv_escape(header[i]);
    }
    f << "\n";

    for (const auto& r : results) {
        std::vector<std::string> cells;
        for (const auto& v : r.axis_values) cells.push_back(cell_string(v));
        for (size_t m = 0; m < spec.report_fields.size(); ++m) {
            cells.push_back(r.ok ? format_g6(r.metrics[m]) : std::string());
        }
        if (has_baseline) {
            const PointResult& base = results[static_cast<size_t>(baseline_index)];
            for (size_t m = 0; m < spec.report_fields.size(); ++m) {
                std::string cell;
                if (r.ok && base.metrics[m] != 0.0) {
                    cell = format_g6((r.metrics[m] - base.metrics[m]) / base.metrics[m] * 100.0);
                }
                cells.push_back(cell);
            }
        }
        cells.push_back(r.ok ? std::string() : r.error);

        for (size_t i = 0; i < cells.size(); ++i) {
            if (i) f << ",";
            f << csv_escape(cells[i]);
        }
        f << "\n";
    }
}

void write_json_report(const std::string& csv_out_path, const SweepSpec& spec,
                        const std::vector<PointResult>& results, int baseline_index) {
    using ddrtiming::json::Value;
    bool has_baseline = baseline_available(results, baseline_index);

    Value root = Value::make_object();
    root.set("spec", spec.raw);
    root.set("points", static_cast<int64_t>(results.size()));

    ddrtiming::json::Array rows;
    for (const auto& r : results) {
        Value row = Value::make_object();
        for (size_t a = 0; a < spec.axes.size(); ++a) row.set(spec.axes[a].path, r.axis_values[a]);
        for (size_t m = 0; m < spec.report_fields.size(); ++m) {
            row.set(spec.report_fields[m], r.ok ? Value(r.metrics[m]) : Value(nullptr));
        }
        if (has_baseline) {
            const PointResult& base = results[static_cast<size_t>(baseline_index)];
            for (size_t m = 0; m < spec.report_fields.size(); ++m) {
                Value delta_v(nullptr);
                if (r.ok && base.metrics[m] != 0.0) {
                    delta_v = Value((r.metrics[m] - base.metrics[m]) / base.metrics[m] * 100.0);
                }
                row.set(spec.report_fields[m] + "_delta_pct", delta_v);
            }
        }
        row.set("error", r.ok ? std::string() : r.error);
        rows.push_back(std::move(row));
    }
    root.set("rows", Value(std::move(rows)));

    fs::path json_path(csv_out_path);
    json_path.replace_extension(".json"); // "report.csv" -> "report.json", next to the CSV
    std::ofstream f(json_path, std::ios::binary);
    if (!f) fail("cannot open JSON output path for writing: " + json_path.string());
    f << root.dump(2);
}

// Compact aligned text table to stdout (README S4.5: "axes + metrics"). The
// trailing `error` column is this tool's own addition beyond that -- a
// failed point's metric cells are blank exactly like the CSV, and without
// some indication a human scanning the table would have no way to tell
// "zero" apart from "this point never ran"; printing the (non-judgmental,
// purely factual) validation message is still just reporting a number... or
// rather, a fact, never a verdict.
void print_table(const SweepSpec& spec, const std::vector<PointResult>& results) {
    std::vector<std::string> header;
    for (const auto& ax : spec.axes) header.push_back(ax.path);
    for (const auto& m : spec.report_fields) header.push_back(m);
    header.push_back("error");

    std::vector<std::vector<std::string>> rows;
    for (const auto& r : results) {
        std::vector<std::string> row;
        for (const auto& v : r.axis_values) row.push_back(cell_string(v));
        for (size_t m = 0; m < spec.report_fields.size(); ++m) {
            row.push_back(r.ok ? format_g6(r.metrics[m]) : std::string("-"));
        }
        row.push_back(r.ok ? std::string() : r.error);
        rows.push_back(std::move(row));
    }

    std::vector<size_t> width(header.size());
    for (size_t c = 0; c < header.size(); ++c) width[c] = header[c].size();
    for (const auto& row : rows) {
        for (size_t c = 0; c + 1 < row.size(); ++c) width[c] = std::max(width[c], row[c].size());
    }

    auto print_row = [&](const std::vector<std::string>& cells) {
        for (size_t c = 0; c < cells.size(); ++c) {
            if (c) std::cout << "  ";
            if (c + 1 == cells.size()) std::cout << cells[c]; // error: left-justified, unpadded (can be long/empty)
            else std::cout << std::string(width[c] - cells[c].size(), ' ') << cells[c];
        }
        std::cout << "\n";
    };

    print_row(header);
    for (const auto& row : rows) print_row(row);
}

void print_usage() {
    std::cout <<
        "Usage: ddrtiming_sweep --spec <sweep.json> --out <report.csv> [--jobs N] [--baseline-index N]\n"
        "Runs the Cartesian product of a sweep spec's \"axes\" against its \"base_config\"/\"logs\",\n"
        "writing <report.csv>, the equivalent <report.json>, and a compact table to stdout.\n"
        "  --spec PATH            sweep spec JSON (required) -- see README S4.5\n"
        "  --out PATH.csv         output CSV path (required); a sibling .json is written alongside it\n"
        "  --jobs N               worker threads (default: std::thread::hardware_concurrency())\n"
        "  --baseline-index N     also emit <metric>_delta_pct columns relative to point N\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string spec_path, out_path;
    int jobs = 0;
    bool have_baseline = false;
    int baseline_index = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << flag << "\n"; std::exit(1); }
            return argv[++i];
        };
        if (arg == "--spec") spec_path = need("--spec");
        else if (arg == "--out") out_path = need("--out");
        else if (arg == "--jobs") jobs = std::atoi(need("--jobs").c_str());
        else if (arg == "--baseline-index") { baseline_index = std::atoi(need("--baseline-index").c_str()); have_baseline = true; }
        else if (arg == "--help" || arg == "-h") { print_usage(); return 0; }
        else { std::cerr << "unknown argument: " << arg << "\n"; print_usage(); return 1; }
    }

    if (spec_path.empty() || out_path.empty()) {
        print_usage();
        return 1;
    }

    SweepSpec spec = load_spec(spec_path);
    SweepPlan plan = build_plan(spec.axes);

    if (have_baseline && (baseline_index < 0 || static_cast<size_t>(baseline_index) >= plan.total_points)) {
        fail("--baseline-index " + std::to_string(baseline_index) + " is out of range [0, " +
             std::to_string(plan.total_points) + ")");
    }
    int effective_baseline_index = have_baseline ? baseline_index : -1;

    ddrtiming::json::Value base_root;
    try {
        base_root = ddrtiming::json::parse_file(spec.base_config_path.string());
    } catch (const std::exception& e) {
        fail("cannot parse base_config '" + spec.base_config_path.string() + "': " + e.what());
    }

    fs::path out_dir = fs::path(out_path).parent_path();
    if (out_dir.empty()) out_dir = fs::path(".");
    fs::path tmp_dir = out_dir / ".sweep_tmp";
    std::error_code ec;
    fs::create_directories(tmp_dir, ec);
    if (ec) fail("cannot create temp directory '" + tmp_dir.string() + "': " + ec.message());

    unsigned hw = std::thread::hardware_concurrency();
    unsigned jobs_to_use = jobs > 0 ? static_cast<unsigned>(jobs) : (hw > 0 ? hw : 1u);

    std::vector<PointResult> results = run_all_points(base_root, spec, plan, tmp_dir, jobs_to_use);

    write_csv(out_path, spec, results, effective_baseline_index);
    write_json_report(out_path, spec, results, effective_baseline_index);
    print_table(spec, results);

    size_t failed = 0;
    for (const auto& r : results) if (!r.ok) ++failed;
    fs::path json_path(out_path);
    json_path.replace_extension(".json");
    std::cout << "\n" << results.size() << " point(s), " << (results.size() - failed) << " ok, "
              << failed << " failed validation/run\n";
    std::cout << "wrote " << out_path << " and " << json_path.string() << "\n";

    // README S4.5 / brief: every point running (even with some `error` rows)
    // is success -- only a spec/IO problem (handled above via fail(), which
    // exits 1 directly) should produce a nonzero exit code.
    return 0;
}
