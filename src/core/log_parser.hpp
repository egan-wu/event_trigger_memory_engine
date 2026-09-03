#pragma once
#include <string>
#include <vector>

#include "types.hpp"

namespace ddrtiming {

// One parsed row: either a transaction or a barrier marker (see push_barrier
// in engine.hpp for barrier semantics).
struct LogEntry {
    bool is_barrier = false;
    AxiTxn txn; // valid only if !is_barrier
};

// Parses a CSV AXI transaction log into a sequence of LogEntry tagged with
// core_id. Expected columns (header row required): type,id,addr,size,len,wstrb
//   type  - "AR", "AW", or "BARRIER" (barrier rows ignore all other columns)
//   id    - AXI transaction ID (decimal or 0x-hex), optional/defaults to 0
//   addr  - byte address (decimal or 0x-hex)
//   size  - literal bytes transferred per beat (e.g. 64), NOT the raw 3-bit
//           AxSIZE protocol encoding
//   len   - literal beat count (i.e. AxLEN + 1 already applied), not raw AxLEN
//   wstrb - optional hex byte-string, AW rows only; blank = full-strobe assumed
// Throws std::runtime_error on malformed input.
std::vector<LogEntry> parse_axi_log_file(const std::string& path, int core_id);

} // namespace ddrtiming
