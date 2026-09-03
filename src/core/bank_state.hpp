#pragma once
#include <cstdint>
#include <deque>

namespace ddrtiming {

struct BankState {
    bool row_open = false;
    uint32_t open_row = 0;
    uint64_t row_opened_at_cycle = 0; // for tRAS enforcement before precharge
    uint64_t bank_ready_cycle = 0;    // earliest cycle this bank can accept its next command
};

struct RankState {
    std::deque<uint64_t> recent_activates; // rolling window (<=4) for tFAW
    uint32_t last_activate_bankgroup = 0;
    bool refresh_initialized = false;
    uint64_t next_refresh_due_cycle = 0;
};

} // namespace ddrtiming
