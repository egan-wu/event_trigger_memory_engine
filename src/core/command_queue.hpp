#pragma once
#include <cstdint>
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
    // Count of column commands that paid tCCD_L (same bank group as the
    // immediately preceding command on this channel) rather than tCCD_S
    // (different bank group). See DramCommand::bankgroup_reuse.
    uint64_t bankgroup_reuse_count = 0;
};

// Per-channel DDRC scheduler: bounded command queue (backpressure), genuine
// FR-FCFS bank scheduling with row-buffer tracking, tRRD/tFAW/tCCD/turnaround
// spacing, and periodic refresh insertion. One instance models one physical
// DDR channel; channels are otherwise independent resources.
//
// FR-FCFS ("first-ready, first-come-first-served", Rixner et al.): commands
// accumulate in a bounded pending queue (depth = command_queue_depth) as they
// arrive from possibly-different cores/streams. Whenever a slot is needed --
// either to admit a new arrival once the queue is full, or to flush at the
// end of the run -- the scheduler picks the *best* currently-queued command,
// not simply the oldest: a command that is a page-hit against its target
// bank's already-open row is serviced ahead of an older command that would
// require a fresh activate, so one stream's cheap hit doesn't sit blocked
// behind another stream's expensive conflict. Ties (same hit/non-hit status)
// break by arrival order (oldest first), matching the classic definition.
//
// Plain FR-FCFS is known to be able to hurt throughput under concurrent
// multi-stream contention: greedily draining whichever bank currently has a
// hit backlog starves other banks' activate opportunity (their precharge/
// activate latency never gets to overlap with anything), and a command left
// waiting behind a hit streak can have another stream close its target row
// in the meantime, turning a would-have-been hit into a conflict once it's
// finally serviced -- a real, observed pathology here, not a hypothetical
// one (see README). Three bounded, deliberately simple mitigations are
// layered on top, matching what real shipped memory-controller IP (e.g.
// Synopsys DesignWare uMCTL2's starvation timers and pagematch_limit
// register) actually does, rather than a full academic scheduler redesign:
//   1. Per-command starvation age: a command skipped too many times is
//      force-selected regardless of hit status (kStarvationLimit).
//   2. A cap on consecutive same-bank hit selections while another bank has
//      a candidate waiting (kPagematchLimit) -- directly modeled on
//      pagematch_limit.
//   3. A never-opened ("idle") bank is preferred over a conflict (though
//      still behind an outright hit) -- a cheap way to get bank-group
//      interleaving started instead of always resolving whichever bank
//      already has backlog.
class ChannelScheduler {
public:
    ChannelScheduler(const DdrcConfig& cfg, int channel_id);

    // Admits `cmd` into the pending queue if there is room (queue size <
    // command_queue_depth); returns false (does nothing) if the queue is
    // already full -- the caller must drain_one() first to free a slot.
    // `ready_cycle` is the earliest cycle this command is known to exist
    // (front-end issue time); it is used only for FCFS tie-breaking, never
    // as a hard "not before this cycle" gate (real controllers don't get to
    // wait for a hypothetical better command that hasn't arrived yet, and
    // neither does this: whatever is in the queue when a decision is made
    // is all that decision can see).
    bool try_admit(const DramCommand& cmd, uint64_t ready_cycle);

    bool has_room() const;
    bool has_pending() const { return !pending_.empty(); }

    // Picks the best currently-pending command (FR-FCFS: hit-status first,
    // then oldest by arrival), computes its final timing exactly as the
    // single-command scheduler used to, updates channel/bank state, and
    // returns it (with start_cycle/complete_cycle/row_status filled in).
    // Precondition: has_pending() is true.
    DramCommand drain_one();

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

    ChannelStats stats_;

    struct PendingCmd {
        DramCommand cmd;
        uint64_t ready_cycle;
        uint64_t seq;         // admission order, for FCFS tie-breaking
        uint32_t skip_count = 0; // times passed over -- see kStarvationLimit
    };
    std::vector<PendingCmd> pending_;
    uint64_t next_seq_ = 0;

    static constexpr uint32_t kStarvationLimit = 16; // selection rounds before a hard force-promote
    static constexpr uint32_t kPagematchLimit = 4;   // consecutive same-bank hits before yielding to another bank

    uint64_t last_hit_bank_key_ = ~0ull; // sentinel: no streak yet
    uint32_t consecutive_hit_count_ = 0;

    int peek_priority(const DramCommand& cmd) const; // 0=hit, 1=idle/never-opened bank, 2=conflict
    uint64_t bank_key_of(const DramCommand& cmd) const;
    size_t pick_best_index() const;
    uint64_t apply_refresh_if_due(RankState& rk, uint64_t earliest_cycle);
    uint64_t apply_activate_gating(RankState& rk, uint32_t bankgroup, uint64_t cycle);
};

} // namespace ddrtiming
