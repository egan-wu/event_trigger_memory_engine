#pragma once
#include <cstdint>
#include <deque>

namespace ddrtiming {

struct BankState {
    bool row_open = false;
    uint32_t open_row = 0;
    uint64_t row_opened_at_cycle = 0; // for tRAS enforcement before precharge
    // Next COLUMN command to this bank (read or write against whatever row is
    // currently open) may start at or after this cycle. Advanced only by the
    // transfer itself -- tCCD_S/tCCD_L spacing is enforced separately, at the
    // channel level, via last_col_start_cycle_.
    uint64_t col_ready_cycle = 0;
    // This bank may be PRECHARGED at or after this cycle. Advanced by tRTP
    // (read) / tWR (write) from the column command, and by tRAS from the
    // activate that opened the row -- both are genuine precharge-only
    // constraints, never column-to-column spacing.
    uint64_t precharge_ready_cycle = 0;
    // [S] Earliest cycle an ACTIVATE may open a row in this bank while it is
    // closed. For a bank that was never opened this is 0; a refresh leaves
    // the rank precharged, so it becomes the refresh's end; an
    // auto-precharge (page_policy "closed"/"timer") already paid its own
    // tRP in the shadow of other traffic, so it becomes that precharge plus
    // tRP. Only meaningful when row_open is false.
    uint64_t act_ready_cycle = 0;
    // [S] col_start of this bank's most recent column command -- the
    // reference point for the "timer" page policy's idle countdown.
    uint64_t last_col_start_cycle = 0;
};

struct RankState {
    std::deque<uint64_t> recent_activates; // rolling window (<=4) for tFAW
    uint32_t last_activate_bankgroup = 0;
    bool refresh_initialized = false;
    uint64_t next_refresh_due_cycle = 0;
};

} // namespace ddrtiming
