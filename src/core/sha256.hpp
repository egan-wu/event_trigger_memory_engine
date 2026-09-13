#pragma once
#include <string>

namespace ddrtiming {

// Minimal, dependency-free SHA-256 (FIPS 180-4). Used only for report
// provenance (README "provenance" -- config_sha256 / inputs[].sha256), a
// content-identity check for telling "same config/input" apart from
// "different config/input" across two reports, not for anything
// security-sensitive. Returns the 64-character lowercase hex digest.
std::string sha256_hex(const std::string& data);

} // namespace ddrtiming
