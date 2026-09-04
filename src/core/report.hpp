#pragma once
#include <string>

#include "engine.hpp"

namespace ddrtiming {

// Writes summary + per-transaction results + windowed history (if enabled)
// as JSON to out_path.
void write_report_json(const Engine& engine, const std::string& out_path);

// Human-readable summary for CLI/stdout use.
std::string format_summary_text(const Engine& engine);

// Writes the windowed bandwidth/byte-access history (Engine::windows()) as
// CSV to out_path -- requires "reporting": {"history_window_ns": N} in the
// config; throws if there are no windows (i.e. that wasn't set).
void write_windowed_csv(const Engine& engine, const std::string& out_path);

} // namespace ddrtiming
