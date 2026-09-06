// Regression harness for bench/: runs each bench/<case>/{config.json,
// core*.csv} through the engine directly (not by shelling out to the CLI --
// faster, and sidesteps path/quoting headaches on Windows) and compares the
// resulting SummaryStats plus a handful of aggregate windowed-history
// statistics against a committed bench/<case>/golden.json.
//
// This exists to make timing-model changes *visible and attributable*: two
// other efforts are fixing known-wrong behavior in the scheduler/timing
// model concurrently (see bench/README.md's known-wrong-behavior list), and
// the goldens checked in right now deliberately capture that wrong behavior.
// A FAIL here after a fix lands is not "the fix broke something" by default
// -- it's "the fix changed a number", and bench/README.md's prediction table
// says which direction each number *should* move. Confirm the direction
// matches, then re-run with --update-golden to adopt the new numbers as the
// baseline for the next change. See bench/README.md for the full policy on
// when --update-golden is legitimate versus papering over a regression.
//
// Usage:
//   golden_check --bench-root <dir> [--case NAME ...] [--tolerance PCT] [--update-golden]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "../src/core/config.hpp"
#include "../src/core/engine.hpp"
#include "../src/core/json.hpp"
#include "../src/core/log_parser.hpp"

namespace fs = std::filesystem;
using ddrtiming::AddressField;
using ddrtiming::DdrcConfig;
using ddrtiming::Engine;
using ddrtiming::SummaryStats;
using ddrtiming::WindowStats;

namespace {

constexpr double kDefaultTolerancePct = 0.5;

// One field of the snapshot we compare goldens against. `is_exact` fields
// (see the classification note below) must match bit-for-bit; everything
// else is compared with a relative tolerance.
struct Field {
    std::string name;
    double value;
    bool is_exact;
};

// A flattened, comparable snapshot of one bench case's run: the full
// SummaryStats plus a few aggregate windowed-history statistics. Per-window
// arrays are deliberately NOT captured here -- they're both too large to
// keep as a committed golden and too brittle (any change to windowing,
// dispatch order, or chunk granularity shifts individual window boundaries
// without the aggregate behavior actually being wrong) to compare
// window-by-window in a regression gate. The count of windows plus the
// mean/min/max of per-window bandwidth is enough to catch a real regression
// in the windowed-history *shape* (e.g. one channel starving) without that
// brittleness.
struct Snapshot {
    std::vector<Field> fields;

    void add(const std::string& name, double value, bool is_exact) {
        fields.push_back({name, value, is_exact});
    }

    const Field* find(const std::string& name) const {
        for (const auto& f : fields) if (f.name == name) return &f;
        return nullptr;
    }
};

// Classification rationale (the task calls this out explicitly, so it gets
// a real comment, not just a bool):
//
// total_txns / total_bytes / total_dram_bytes are *structural* -- they fall
// out of parsing the AXI log against the config's burst/alignment geometry
// alone, before any scheduling or timing decision is made. A timing-model
// fix (the entire reason this harness exists) has no mechanism to change
// them; if one of them moves, either the log/config changed (expected, and
// --update-golden is correct) or something upstream of the scheduler broke
// (a real bug, and it is not correct). There is no meaningful "close enough"
// for a byte count, so these compare exactly.
//
// Everything else -- total_cycles, sim_time_ns, every rate/bandwidth/latency
// field, and the windowed-history aggregates -- is a direct function of the
// scheduling/timing model and is *expected* to move whenever that model
// changes, by design. These get the relative-tolerance comparison so routine
// floating-point noise (summation order, etc.) doesn't cause spurious FAILs
// while a real shift still trips the tolerance.

double window_bandwidth_gbps(const WindowStats& w, double window_ns) {
    if (window_ns <= 0.0) return 0.0;
    return static_cast<double>(w.bytes_read + w.bytes_written) / window_ns;
}

Snapshot build_snapshot(const Engine& engine) {
    Snapshot snap;
    const SummaryStats& s = engine.summary();

    snap.add("total_txns", static_cast<double>(s.total_txns), true);
    snap.add("total_bytes", static_cast<double>(s.total_bytes), true);
    snap.add("total_dram_bytes", static_cast<double>(s.total_dram_bytes), true);

    snap.add("total_cycles", static_cast<double>(s.total_cycles), false);
    snap.add("sim_time_ns", s.sim_time_ns, false);
    snap.add("avg_bandwidth_gbps", s.avg_bandwidth_gbps, false);
    snap.add("avg_dram_bandwidth_gbps", s.avg_dram_bandwidth_gbps, false);
    snap.add("peak_bandwidth_gbps", s.peak_bandwidth_gbps, false);
    snap.add("bandwidth_utilization_pct", s.bandwidth_utilization_pct, false);
    snap.add("burst_efficiency_pct", s.burst_efficiency_pct, false);
    snap.add("avg_latency_ns", s.avg_latency_ns, false);
    snap.add("page_hit_rate_pct", s.page_hit_rate_pct, false);
    snap.add("row_conflict_rate_pct", s.row_conflict_rate_pct, false);
    snap.add("row_empty_rate_pct", s.row_empty_rate_pct, false);
    snap.add("refresh_overhead_pct", s.refresh_overhead_pct, false);
    snap.add("turnaround_overhead_pct", s.turnaround_overhead_pct, false);
    snap.add("bankgroup_reuse_rate_pct", s.bankgroup_reuse_rate_pct, false);

    const std::vector<WindowStats>& windows = engine.windows();
    double window_ns = engine.config().history_window_ns;
    snap.add("windowed.window_count", static_cast<double>(windows.size()), false);
    if (!windows.empty()) {
        double sum = 0.0, mn = std::numeric_limits<double>::infinity(), mx = -std::numeric_limits<double>::infinity();
        for (const auto& w : windows) {
            double bw = window_bandwidth_gbps(w, window_ns);
            sum += bw;
            mn = std::min(mn, bw);
            mx = std::max(mx, bw);
        }
        snap.add("windowed.avg_bandwidth_gbps_mean", sum / static_cast<double>(windows.size()), false);
        snap.add("windowed.avg_bandwidth_gbps_min", mn, false);
        snap.add("windowed.avg_bandwidth_gbps_max", mx, false);
    } else {
        snap.add("windowed.avg_bandwidth_gbps_mean", 0.0, false);
        snap.add("windowed.avg_bandwidth_gbps_min", 0.0, false);
        snap.add("windowed.avg_bandwidth_gbps_max", 0.0, false);
    }
    return snap;
}

ddrtiming::json::Value snapshot_to_json(const Snapshot& snap) {
    ddrtiming::json::Value root = ddrtiming::json::Value::make_object();
    for (const auto& f : snap.fields) root.set(f.name, f.value);
    return root;
}

Snapshot snapshot_from_json(const ddrtiming::json::Value& root, const Snapshot& shape_reference) {
    Snapshot snap;
    for (const auto& f : shape_reference.fields) {
        double v = root.contains(f.name) ? root[f.name].as_double(0.0) : 0.0;
        snap.add(f.name, v, f.is_exact);
    }
    return snap;
}

// Finds core0.csv, core1.csv, ... in `dir` (must be contiguous from 0, which
// every bench case satisfies by construction) and returns them in core_id
// order -- mirroring the CLI's "each --log is assigned core_id = its
// position" rule (src/cli/main.cpp) exactly, just discovered from the
// filesystem instead of the argv list.
std::vector<fs::path> discover_core_logs(const fs::path& dir) {
    std::vector<fs::path> logs;
    for (int core_id = 0;; ++core_id) {
        fs::path p = dir / ("core" + std::to_string(core_id) + ".csv");
        if (!fs::exists(p)) break;
        logs.push_back(p);
    }
    return logs;
}

struct CaseResult {
    std::string name;
    bool ran_ok = false;
    std::string error;
    Snapshot actual;
};

CaseResult run_case(const fs::path& case_dir) {
    CaseResult result;
    result.name = case_dir.filename().string();

    fs::path config_path = case_dir / "config.json";
    if (!fs::exists(config_path)) {
        result.error = "no config.json in " + case_dir.string();
        return result;
    }
    std::vector<fs::path> logs = discover_core_logs(case_dir);
    if (logs.empty()) {
        result.error = "no core*.csv logs found in " + case_dir.string();
        return result;
    }

    try {
        DdrcConfig cfg = DdrcConfig::load_from_file(config_path.string());
        Engine engine(std::move(cfg));
        for (size_t i = 0; i < logs.size(); ++i) {
            int core_id = static_cast<int>(i);
            auto entries = ddrtiming::parse_axi_log_file(logs[i].string(), core_id);
            for (const auto& e : entries) {
                if (e.is_barrier) engine.push_barrier(core_id);
                else engine.push_txn(e.txn);
            }
        }
        engine.run();
        result.actual = build_snapshot(engine);
        result.ran_ok = true;
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}

// Prints one violation line in the format the task's own worked example
// uses -- case, field, both values, signed relative delta -- since that
// line is the entire point: someone reading only it should know what
// happened without re-running anything.
void print_failure(const std::string& case_name, const Field& golden, const Field& actual) {
    if (golden.is_exact) {
        std::printf("FAIL %s.%s: golden %.0f  actual %.0f  (delta %+.0f)\n",
                    case_name.c_str(), golden.name.c_str(), golden.value, actual.value,
                    actual.value - golden.value);
        return;
    }
    if (golden.value == 0.0) {
        std::printf("FAIL %s.%s: golden %.4f  actual %.4f  (golden is 0, absolute delta %+.4f)\n",
                    case_name.c_str(), golden.name.c_str(), golden.value, actual.value,
                    actual.value - golden.value);
        return;
    }
    double rel_pct = (actual.value - golden.value) / golden.value * 100.0;
    std::printf("FAIL %s.%s: golden %.4f  actual %.4f  (%+.2f%%)\n",
                case_name.c_str(), golden.name.c_str(), golden.value, actual.value, rel_pct);
}

// Returns true if this case passed (no violations).
bool compare_case(const std::string& case_name, const Snapshot& golden, const Snapshot& actual, double tolerance_pct) {
    bool ok = true;
    for (const auto& gf : golden.fields) {
        const Field* af = actual.find(gf.name);
        if (!af) {
            std::printf("FAIL %s.%s: field present in golden but missing from actual run\n",
                        case_name.c_str(), gf.name.c_str());
            ok = false;
            continue;
        }
        if (gf.is_exact) {
            if (af->value != gf.value) {
                print_failure(case_name, gf, *af);
                ok = false;
            }
            continue;
        }
        if (gf.value == 0.0) {
            if (std::fabs(af->value) > 1e-9) {
                print_failure(case_name, gf, *af);
                ok = false;
            }
            continue;
        }
        double rel_pct = std::fabs((af->value - gf.value) / gf.value) * 100.0;
        if (rel_pct > tolerance_pct) {
            print_failure(case_name, gf, *af);
            ok = false;
        }
    }
    return ok;
}

std::vector<std::string> discover_all_case_names(const fs::path& bench_root) {
    std::vector<std::string> names;
    if (!fs::exists(bench_root)) return names;
    for (const auto& entry : fs::directory_iterator(bench_root)) {
        if (!entry.is_directory()) continue;
        if (fs::exists(entry.path() / "config.json")) names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

void print_usage() {
    std::cout <<
        "Usage: golden_check --bench-root <dir> [--case NAME ...] [--tolerance PCT] [--update-golden]\n"
        "  --bench-root DIR   directory containing one subdirectory per bench case (default: bench cases discovered here)\n"
        "  --case NAME        restrict to this case (repeatable); default: every subdirectory of --bench-root with a config.json\n"
        "  --tolerance PCT    relative tolerance percent for floating-point fields (default: " << kDefaultTolerancePct << ")\n"
        "  --update-golden    rewrite bench/<case>/golden.json from the current run instead of comparing against it\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string bench_root_arg;
    std::vector<std::string> case_names;
    double tolerance_pct = kDefaultTolerancePct;
    bool update_golden = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << flag << "\n"; std::exit(1); }
            return argv[++i];
        };
        if (arg == "--bench-root") bench_root_arg = need("--bench-root");
        else if (arg == "--case") case_names.push_back(need("--case"));
        else if (arg == "--tolerance") tolerance_pct = std::strtod(need("--tolerance").c_str(), nullptr);
        else if (arg == "--update-golden") update_golden = true;
        else if (arg == "--help" || arg == "-h") { print_usage(); return 0; }
        else { std::cerr << "unknown argument: " << arg << "\n"; print_usage(); return 1; }
    }

    if (bench_root_arg.empty()) {
        std::cerr << "error: --bench-root is required\n";
        print_usage();
        return 1;
    }
    fs::path bench_root(bench_root_arg);
    if (!fs::exists(bench_root)) {
        std::cerr << "error: bench root does not exist: " << bench_root.string() << "\n";
        return 1;
    }

    if (case_names.empty()) case_names = discover_all_case_names(bench_root);
    if (case_names.empty()) {
        std::cerr << "error: no bench cases found under " << bench_root.string() << "\n";
        return 1;
    }

    bool all_ok = true;
    int cases_run = 0;

    for (const auto& name : case_names) {
        fs::path case_dir = bench_root / name;
        CaseResult result = run_case(case_dir);
        if (!result.ran_ok) {
            std::cerr << "error running case '" << name << "': " << result.error << "\n";
            all_ok = false;
            continue;
        }
        ++cases_run;

        fs::path golden_path = case_dir / "golden.json";
        if (update_golden) {
            ddrtiming::json::Value out = snapshot_to_json(result.actual);
            std::ofstream f(golden_path, std::ios::binary);
            if (!f) {
                std::cerr << "error: cannot write " << golden_path.string() << "\n";
                all_ok = false;
                continue;
            }
            f << out.dump(2);
            std::cout << "updated " << golden_path.string() << "\n";
            continue;
        }

        if (!fs::exists(golden_path)) {
            std::cerr << "error: no golden.json for case '" << name
                      << "' -- run with --update-golden once to create it\n";
            all_ok = false;
            continue;
        }
        ddrtiming::json::Value golden_json;
        try {
            golden_json = ddrtiming::json::parse_file(golden_path.string());
        } catch (const std::exception& e) {
            std::cerr << "error: cannot parse " << golden_path.string() << ": " << e.what() << "\n";
            all_ok = false;
            continue;
        }
        Snapshot golden_snap = snapshot_from_json(golden_json, result.actual);
        bool case_ok = compare_case(name, golden_snap, result.actual, tolerance_pct);
        if (case_ok) {
            std::cout << "PASS " << name << " (" << golden_snap.fields.size() << " fields, tolerance "
                      << tolerance_pct << "%)\n";
        } else {
            all_ok = false;
        }
    }

    if (update_golden) {
        std::cout << "updated " << cases_run << " golden(s)\n";
        return all_ok ? 0 : 1;
    }
    std::cout << (all_ok ? "OK" : "FAILURES") << ": " << cases_run << " case(s) checked\n";
    return all_ok ? 0 : 1;
}
