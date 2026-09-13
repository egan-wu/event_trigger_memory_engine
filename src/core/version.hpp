#pragma once

namespace ddrtiming {

// Single source of truth for the library version string. Both
// ddrt_version() (src/api/ddrtiming_api.cpp, the public C API) and the JSON
// report's "provenance.tool_version" field (src/core/report.cpp) reference
// this constant rather than each hard-coding their own copy, so the two can
// never silently drift apart.
inline constexpr const char* kVersion = "0.4.0";

} // namespace ddrtiming
