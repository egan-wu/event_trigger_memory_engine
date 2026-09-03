#pragma once
#include <string>

#include "engine.hpp"

namespace ddrtiming {

// Writes summary + per-transaction results as JSON to out_path.
void write_report_json(const Engine& engine, const std::string& out_path);

// Human-readable summary for CLI/stdout use.
std::string format_summary_text(const Engine& engine);

} // namespace ddrtiming
