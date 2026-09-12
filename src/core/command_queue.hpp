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
    // Count of column commands that paid tCCD_L (same bank group as the
    // immediately preceding command on this channel) rather than tCCD_S
    // (different bank group). See DramCommand::bankgroup_reuse.
    uint64_t bankgroup_reuse_count = 0;

    // Row-miss (Empty/Conflict) overlap measurement: for each row miss,
    // compare the cycle its row ops (PRE/ACT, plus tRCD) would make the row
    // ready against `earliest` -- the data-bus/tCCD/turnaround/tWTR/refresh
    // floor the column command would have needed anyway even on a page hit.
    // "hidden" means the row ops finished at or before that floor, i.e. they
    // added zero exposed dead time (they were fully overlapped with other
    // commands' data-bus activity); "exposed" means they pushed the column
    // command later than the floor alone would have, by exactly
    // row_miss_exposed_cycles cycles (summed). This is the direct measure of
    // whether bank-level parallelism (the point of this whole rewrite) is
    // actually hiding row-op latency in a given run.
    uint64_t row_miss_hidden = 0;
    uint64_t row_miss_exposed = 0;
    uint64_t row_miss_exposed_cycles = 0;
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

    // col_start of the most recent command this channel actually drained (0
    // if none yet). Numerically this tracks the same quantity as
    // last_col_start_cycle_ above, but the two are kept as separate members
    // because they answer different questions: last_col_start_cycle_ is the
    // tCCD floor (a column-to-column spacing constraint), while this one is
    // the *visibility* floor for PRE/ACT (see PendingCmd::visible_cycle and
    // try_admit) -- a queue slot only frees, and so a new command only
    // becomes knowable to this controller, once this cycle. They happen to
    // be updated together and equal today; keeping them distinct avoids
    // silently coupling two unrelated constraints if either one's role
    // changes later.
    uint64_t last_drain_col_start_ = 0;

    // Monotone non-decreasing bound on ready_cycle across every try_admit()
    // call so far. Safe because Engine::run() only ever calls try_admit()
    // from inside its global min-heap dispatch loop (heap_.top()/pop()),
    // which pops strictly non-decreasing `ready_cycle` (== Event::ready_cycle,
    // the primary sort key -- see Engine::Event::operator> in engine.hpp) --
    // so every command any channel is ever asked to admit has a ready_cycle
    // at least as large as the one before it, whichever channel it lands on.
    // Used by prune_cmd_bus_slots() as a lower bound on any *future*
    // admission's floors.
    uint64_t last_admitted_ready_cycle_ = 0;

    // Command bus: PRE, ACT, and RD/WR each occupy the channel's command bus
    // for kCmdSlotCycles cycles from their own issue cycle, and (item 3 of
    // the redesign) no two may overlap -- modeled as an ordered set of
    // occupied slot *start* cycles (each implicitly spanning
    // [start, start+kCmdSlotCycles)). PRE/ACT for a row miss are placed
    // *earlier* in cycle-time than already-placed column commands (that's
    // the whole point of lookahead), so this can't be a single monotone
    // "bus free" pointer the way the data bus (bus_free_cycle_) is -- a
    // later call can need to insert a slot before ones already recorded.
    std::set<uint64_t> cmd_bus_slots_;
    // Model cycles here are data beats: clock_mhz is the data rate in MT/s,
    // i.e. two transfers per DRAM clock (see config.hpp's comment on
    // clock_mhz), so one command-clock period (during which the command bus
    // carries exactly one command) spans 2 of these cycles.
    static constexpr uint32_t kCmdSlotCycles = 2;

    // tWTR (write-to-read, same rank) tracking: the last WRITE's data-burst
    // completion and bank group, channel-wide -- same granularity as
    // last_col_bankgroup_/last_bus_type_ above.
    uint64_t last_write_complete_cycle_ = 0;
    uint32_t last_write_bankgroup_ = 0;
    bool had_write_cmd_ = false;

    ChannelStats stats_;

    struct PendingCmd {
        DramCommand cmd;
        uint64_t ready_cycle;
        // Earliest cycle this command's PRE/ACT may be scheduled: a
        // controller can only prepare a row for a command actually sitting
        // in its queue, and this command wasn't in the queue until
        // whichever earlier drain vacated the slot it was admitted into
        // (see try_admit). Always >= ready_cycle. Column-command timing is
        // unaffected -- it still floors on ready_cycle via `earliest` in
        // drain_one(), unchanged from before this field existed.
        uint64_t visible_cycle;
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
    uint64_t apply_refresh_if_due(uint32_t rank_idx, uint64_t earliest_cycle);
    uint64_t apply_activate_gating(RankState& rk, uint32_t bankgroup, uint64_t cycle);

    // Places a PRECHARGE no earlier than `floor_cycle`: refresh due-ness
    // (a REFRESH forces this bank precharged anyway, so PRE can't usefully
    // precede one that's already due) then a command-bus slot. No
    // tRRD/tFAW-style gating applies to PRE -- that's an ACT-to-ACT
    // constraint only.
    uint64_t place_precharge(uint32_t rank_idx, uint64_t floor_cycle);

    // Places an ACTIVATE no earlier than `floor_cycle`, in order: refresh
    // due-ness, tRRD/tFAW spacing (apply_activate_gating -- this is the
    // single authoritative call that records the activate into
    // rk.recent_activates and, via its use of recent_activates.back()/
    // .front(), also keeps ACT issue order monotone per rank: a later-
    // admitted command's ACT can never be placed before an earlier one's,
    // matching how a real controller drains its activate queue roughly in
    // order), a second refresh check in case tRRD/tFAW's push crossed into a
    // newly-due refresh window (with the just-recorded activate timestamp
    // corrected in place so a later tRRD/tFAW check isn't under-constrained
    // by the stale, pre-push value), and finally a command-bus slot. Returns
    // the final ACT cycle.
    uint64_t place_activate(uint32_t rank_idx, uint32_t bankgroup, uint64_t floor_cycle);

    // Finds the smallest cycle >= `desired` whose [t, t+kCmdSlotCycles)
    // window doesn't overlap any already-reserved command-bus slot, reserves
    // it, and returns it.
    uint64_t reserve_cmd_slot(uint64_t desired);

    // Drops command-bus slot records that no future call to
    // reserve_cmd_slot() could ever need to check against -- see the
    // comment at cmd_bus_slots_'s definition and the method body for the
    // pruning-horizon argument.
    void prune_cmd_bus_slots();
};

} // namespace ddrtiming
