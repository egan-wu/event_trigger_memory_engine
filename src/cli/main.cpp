#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "../core/config.hpp"
#include "../core/config_presets.hpp"
#include "../core/engine.hpp"
#include "../core/json.hpp"
#include "../core/log_parser.hpp"
#include "../core/report.hpp"

namespace {
void print_usage() {
    std::cout << "Usage: ddrtiming_cli --config <ddrc_config.json> --log <core0_axi.csv> "
                 "[--log <core1_axi.csv> ...] [--out <report.json>] [--windowed-csv <history.csv>] "
                 "[--print-config]\n"
                 "       ddrtiming_cli --config <ddrc_config.json> --validate-only\n"
                 "Each --log is assigned core_id = its position (0, 1, 2, ...).\n"
                 "--windowed-csv requires \"reporting\": {\"history_window_ns\": N} in the config.\n"
                 "--validate-only loads and validates the config, then exits without --log or "
                 "running a simulation -- useful for quickly checking a config is well-formed.\n"
                 "--print-config expands any \"dram\"/\"address_mapping\" preset (see README S4.3) and "
                 "prints the fully effective config as JSON; combine with --validate-only for a "
                 "free check of what a preset actually expands to, or use alone to also run the "
                 "simulation afterward.\n";
}

// [C] Parses and preset-expands config_path, printing the result as JSON --
// shared by --print-config and --validate-only (which also prints it, so a
// preset's expansion is visible on the same free check that catches a
// malformed config). Exits the process on a parse/preset error, matching
// the other config-loading error paths in this file.
void print_expanded_config(const std::string& config_path) {
    try {
        ddrtiming::json::Value expanded = ddrtiming::expand_presets(ddrtiming::json::parse_file(config_path));
        std::cout << expanded.dump(2) << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        std::exit(1);
    }
}
} // namespace

int main(int argc, char** argv) {
    std::string config_path;
    std::vector<std::string> log_paths;
    std::string out_path;
    std::string windowed_csv_path;
    bool validate_only = false;
    bool print_config = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << flag << "\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "--config") config_path = need_value("--config");
        else if (arg == "--log") log_paths.push_back(need_value("--log"));
        else if (arg == "--out") out_path = need_value("--out");
        else if (arg == "--windowed-csv") windowed_csv_path = need_value("--windowed-csv");
        else if (arg == "--validate-only") validate_only = true;
        else if (arg == "--print-config") print_config = true;
        else if (arg == "--help" || arg == "-h") { print_usage(); return 0; }
        else { std::cerr << "unknown argument: " << arg << "\n"; print_usage(); return 1; }
    }

    if (config_path.empty() || (!validate_only && log_paths.empty())) {
        print_usage();
        return 1;
    }

    if (validate_only) {
        if (print_config) print_expanded_config(config_path);
        try {
            ddrtiming::DdrcConfig cfg = ddrtiming::DdrcConfig::load_from_file(config_path);
            (void)cfg;
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 1;
        }
        std::cout << "OK: " << config_path << " is a valid configuration\n";
        return 0;
    }

    if (print_config) print_expanded_config(config_path);

    try {
        ddrtiming::DdrcConfig cfg = ddrtiming::DdrcConfig::load_from_file(config_path);
        ddrtiming::Engine engine(std::move(cfg));

        for (size_t i = 0; i < log_paths.size(); ++i) {
            int core_id = static_cast<int>(i);
            auto entries = ddrtiming::parse_axi_log_file(log_paths[i], core_id);
            size_t txn_count = 0, barrier_count = 0;
            for (const auto& e : entries) {
                if (e.is_barrier) { engine.push_barrier(core_id); ++barrier_count; }
                else { engine.push_txn(e.txn); ++txn_count; }
            }
            std::cout << "Loaded " << txn_count << " transactions (" << barrier_count
                      << " barriers) from " << log_paths[i] << " as core " << core_id << "\n";
        }

        engine.run();

        std::cout << "\n" << ddrtiming::format_summary_text(engine);

        // Not a performance verdict -- an input-integrity problem: the
        // numbers above describe a different workload than the trace meant.
        const ddrtiming::SummaryStats& s = engine.summary();
        if (s.high_address_regions > 1) {
            std::cerr << "\nwarning: transactions span " << s.high_address_regions
                      << " distinct address regions above bit " << (s.mapped_address_bits - 1)
                      << ", the highest bit the address map decodes. Those regions alias onto the same "
                         "banks/rows/columns (the modeled DRAM covers 2^" << s.mapped_address_bits
                      << " bytes), so they share open rows and page-hit rate is overstated. Place the "
                         "trace's buffers inside the modeled capacity, or widen the row mapping.\n";
        }

        if (!out_path.empty()) {
            ddrtiming::ReportProvenance provenance;
            provenance.config_path = config_path;
            provenance.input_paths = log_paths;
            ddrtiming::write_report_json(engine, out_path, provenance);
            std::cout << "\nJSON report written to " << out_path << "\n";
        }
        if (!windowed_csv_path.empty()) {
            ddrtiming::write_windowed_csv(engine, windowed_csv_path);
            std::cout << "Windowed history CSV written to " << windowed_csv_path << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
