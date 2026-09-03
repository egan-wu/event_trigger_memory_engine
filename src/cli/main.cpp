#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "../core/config.hpp"
#include "../core/engine.hpp"
#include "../core/log_parser.hpp"
#include "../core/report.hpp"

namespace {
void print_usage() {
    std::cout << "Usage: ddrtiming_cli --config <ddrc_config.json> --log <core0_axi.csv> "
                 "[--log <core1_axi.csv> ...] [--out <report.json>]\n"
                 "Each --log is assigned core_id = its position (0, 1, 2, ...).\n";
}
} // namespace

int main(int argc, char** argv) {
    std::string config_path;
    std::vector<std::string> log_paths;
    std::string out_path;

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
        else if (arg == "--help" || arg == "-h") { print_usage(); return 0; }
        else { std::cerr << "unknown argument: " << arg << "\n"; print_usage(); return 1; }
    }

    if (config_path.empty() || log_paths.empty()) {
        print_usage();
        return 1;
    }

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

        if (!out_path.empty()) {
            ddrtiming::write_report_json(engine, out_path);
            std::cout << "\nJSON report written to " << out_path << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
