#pragma once
#include <string>
#include <vector>

#include "engine.hpp"

namespace ddrtiming {

// [C] Optional provenance to embed in the JSON report's top-level
// "provenance" object -- lets a caller (an AI agent especially) check
// whether two reports came from the same config/inputs before comparing
// them, rather than silently assuming so. config_path/input_paths are the
// exact paths passed in; write_report_json() hashes their contents (and
// counts input_paths' lines) itself, so the caller only needs to name them.
// Default-constructed (config_path empty) means "no provenance requested" --
// write_report_json() omits the whole "provenance" object in that case, so
// existing callers that don't pass one see no change in their output.
struct ReportProvenance {
    std::string config_path;
    std::vector<std::string> input_paths;
};

// Writes summary + per-transaction results + windowed history (if enabled)
// as JSON to out_path. `provenance`, if given (see ReportProvenance above),
// adds a top-level "provenance" object naming the tool version and hashing
// the config/input files named in it.
void write_report_json(const Engine& engine, const std::string& out_path,
                        const ReportProvenance& provenance = ReportProvenance{});

// Human-readable summary for CLI/stdout use.
std::string format_summary_text(const Engine& engine);

// Writes the windowed bandwidth/byte-access history (Engine::windows()) as
// CSV to out_path -- requires "reporting": {"history_window_ns": N} in the
// config; throws if there are no windows (i.e. that wasn't set).
void write_windowed_csv(const Engine& engine, const std::string& out_path);

} // namespace ddrtiming
