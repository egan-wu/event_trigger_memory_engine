#pragma once
#include <cstdint>
#include <set>
#include <vector>

#include "bank_state.hpp"
#include "config.hpp"
#include "types.hpp"

namespace ddrtiming {

struct ChannelStats {
    uint64_t hits = 0, conflicts = 0, empties = 0;
    uint64_t refresh_cycles = 0;
    uint64_t turnaround_cycles = 0;
    uint64_t busy_cycles = 0; // cycles the data bus was actually transferring
};

// Per-channel DDRC scheduler: bounded command queue (backpressure), FR-FCFS-lite
// bank scheduling with row-buffer tracking, tRRD/tFAW/tCCD/turnaround spacing,
// and periodic refresh insertion. One instance models one physical DDR channel;
// channels are otherwise independent resources.
class ChannelScheduler {
public:
    ChannelScheduler(const DdrcConfig& cfg, int channel_id);

    // Schedules one DRAM column command given the earliest cycle it can arrive
    // at the channel (already gated by core-level outstanding/backpressure
    // upstream). Mutates cmd.start_cycle / cmd.complete_cycle / cmd.row_status
    // and updates internal bank/rank/refresh/queue state. Returns complete_cycle.
    uint64_t schedule(DramCommand& cmd, uint64_t arrival_cycle);

    const ChannelStats& stats() const { return stats_; }

private:
    const DdrcConfig& cfg_;
    int channel_id_;

    uint64_t cyc(double ns) const { return cfg_.ns_to_cycles(ns); }

    // indexed [rank][bankgroup][bank]
    std::vector<std::vector<std::vector<BankState>>> banks_;
    std::vector<RankState> ranks_;

    uint64_t bus_free_cycle_ = 0;
    TxnType last_bus_type_ = TxnType::Read;
    bool bus_used_ = false;

    uint64_t last_col_start_cycle_ = 0;
    uint32_t last_col_bankgroup_ = 0;
    bool had_col_cmd_ = false;

    std::multiset<uint64_t> inflight_completions_;

    ChannelStats stats_;

    uint64_t queue_admit_cycle(uint64_t arrival_cycle);
    uint64_t apply_refresh_if_due(RankState& rk, uint64_t earliest_cycle);
    uint64_t apply_activate_gating(RankState& rk, uint32_t bankgroup, uint64_t cycle);
};

} // namespace ddrtiming
