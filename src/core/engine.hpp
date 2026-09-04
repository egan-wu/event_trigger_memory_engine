#pragma once
#include <deque>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "config.hpp"
#include "types.hpp"

namespace ddrtiming {

class ChannelScheduler;

struct SummaryStats {
    uint64_t total_txns = 0;
    uint64_t total_bytes = 0;      // logical: bytes actually requested by AXI bursts
    uint64_t total_dram_bytes = 0; // physical: full burst-aligned bytes DRAM actually moved (>= total_bytes)
    uint64_t total_cycles = 0;
    double sim_time_ns = 0.0;
    double avg_bandwidth_gbps = 0.0;     // total_bytes / sim_time_ns -- useful throughput delivered
    double avg_dram_bandwidth_gbps = 0.0; // total_dram_bytes / sim_time_ns -- actual DRAM bus traffic
    double peak_bandwidth_gbps = 0.0;
    double bandwidth_utilization_pct = 0.0; // avg_dram_bandwidth_gbps / peak -- physical bus utilization
    double burst_efficiency_pct = 0.0;      // total_bytes / total_dram_bytes -- 100% = no over-fetch waste
    double avg_latency_ns = 0.0;
    double page_hit_rate_pct = 0.0;
    double row_conflict_rate_pct = 0.0;
    double row_empty_rate_pct = 0.0;
    double refresh_overhead_pct = 0.0;
    double turnaround_overhead_pct = 0.0;
};

// One fixed-size bucket of simulated time (history_window_ns in the config),
// indexed by a transaction's issue_cycle. Accumulated incrementally at
// dispatch time -- like SummaryStats, unaffected by prune_results_before()
// -- so a caller can build a bandwidth/byte-access history independent of
// how often it happens to call run() or drain/prune results(). See README
// "Windowed history".
// start_ns for window i is always i * history_window_ns (derive it from the
// index, not stored here -- an untouched/empty window still has a well-
// defined start, and deriving from the index keeps that correct for free).
struct WindowStats {
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
    uint64_t dram_bytes = 0;
    uint64_t txn_count = 0;
    uint64_t hits = 0, conflicts = 0, empties = 0;
    // High-water mark, across every (core, axi_id) stream active in this
    // window, of that stream's outstanding-request count immediately after a
    // dispatch (sampled at dispatch instants, which is exact: occupancy for a
    // given stream only ever changes at its own dispatch events, so nothing
    // is missed between samples). Compare against config max_outstanding_per_id
    // to see whether the cap is actually the thing limiting throughput.
    uint64_t max_outstanding_count = 0;
    // Distinct physical banks (channel/rank/bankgroup/bank, post-modulo --
    // i.e. exactly the banks_[] indices ChannelScheduler actually schedules
    // against) touched by at least one dispatched command in this window.
    // size() / total_banks (topology) is bank-level parallelism utilization:
    // separate from bus/outstanding saturation -- a workload can be far from
    // both of those limits and still serialize badly if it's only ever
    // hitting a handful of banks (an address-mapping spread problem, not a
    // timing one).
    std::set<uint64_t> active_banks;
};

// One independent dispatch stream per (core_id, segment, axi_id): AXI
// guarantees same-ID transactions complete in the order issued, but different
// IDs from the same core may be independently outstanding and complete out of
// order. `pending` holds this stream's not-yet-dispatched transactions in
// original relative (log) order; each is freed the moment it's dispatched
// (pop_front), so a long-running caller doesn't accumulate its whole history
// -- only whatever's genuinely still in flight or backlogged. `queued` tracks
// whether an event for this cursor currently sits in the run() heap, so
// push_txn() knows whether it needs to enqueue a fresh one (it must, if this
// cursor had drained and now has new work).
struct IdCursor {
    std::deque<AxiTxn> pending;
    std::multiset<uint64_t> outstanding; // completion cycles of this id's in-flight txns
    bool queued = false;
};

// A run of transactions between two barriers (or log start / "still open")
// for one core. `closed` becomes true once a barrier ends it -- an open
// segment may still receive more push_txn() calls indefinitely (the normal
// case for a long-running daemon with no "end of log"). A segment can only be
// gate_known before its predecessor is `closed` AND every id-stream in the
// predecessor has fully drained (pending empty for all of them).
struct Segment {
    std::map<uint32_t, IdCursor> by_id;
    uint64_t max_complete = 0;
    uint64_t gate_cycle = 0;
    bool gate_known = false;
    bool closed = false;
};

// Top-level orchestrator: owns per-core barrier-delimited segments (which own
// the not-yet-dispatched transactions themselves -- there's no separate raw
// log store) and per-channel DDRC schedulers, and runs the event-driven
// simulation. Designed for two usage patterns: (1) push everything then call
// run() once (batch/CLI use), or (2) push incrementally from a live/
// long-running source (e.g. a DMA model daemon with no natural "end of log")
// and call run() periodically as a "tick" -- each call only processes
// transactions pushed since the previous call, using persistent internal
// state, and results()/summary() always reflect everything processed so far.
// push_barrier() lets the caller mark a known synchronization point (no real
// timestamp needed) so the engine doesn't wrongly assume maximum eagerness
// across it -- see README.
//
// Memory over a long-running session: dispatched transactions are freed
// immediately (see IdCursor above), but results() accumulates forever unless
// you call prune_results_before() -- summary() is tracked with separate
// cumulative counters specifically so pruning results() never corrupts it.
class Engine {
public:
    explicit Engine(DdrcConfig cfg);
    ~Engine();

    uint64_t push_txn(const AxiTxn& txn);
    void push_barrier(int core_id);
    void run();

    const std::vector<TxnResult>& results() const { return results_; }
    const SummaryStats& summary() const { return summary_; }
    const DdrcConfig& config() const { return cfg_; }

    // Windowed bandwidth/byte-access history, one entry per history_window_ns
    // bucket of simulated time (config; 0 disables this, and the vector stays
    // empty). Grows as dispatch reaches later windows; never shrinks, and is
    // NOT affected by prune_results_before() -- see WindowStats above.
    const std::vector<WindowStats>& windows() const { return windows_; }

    // Removes every result with txn_id <= max_txn_id from results() (order-
    // independent -- results() is dispatch-ordered, not txn_id-ordered, since
    // independent AXI-ID streams can complete out of order relative to each
    // other). Call this only after you've actually consumed/reported
    // everything you're about to prune -- there's no re-fetching it after.
    // summary() is unaffected: it's tracked independently and stays correct
    // (cumulative since the engine was created) regardless of pruning.
    void prune_results_before(uint64_t max_txn_id);

private:
    struct Event {
        uint64_t ready_cycle;
        int core_id;
        int segment_idx;
        uint32_t axi_id;
        bool operator>(const Event& o) const {
            if (ready_cycle != o.ready_cycle) return ready_cycle > o.ready_cycle;
            if (core_id != o.core_id) return core_id > o.core_id;
            if (segment_idx != o.segment_idx) return segment_idx > o.segment_idx;
            return axi_id > o.axi_id;
        }
    };

    DdrcConfig cfg_;
    uint64_t next_txn_id_ = 1;

    std::vector<std::unique_ptr<ChannelScheduler>> channels_;
    std::vector<TxnResult> results_;
    SummaryStats summary_;

    // Cumulative counters behind summary(), updated once per dispatch and
    // never touched by prune_results_before() -- this is what keeps the
    // running summary correct regardless of how much of results() is pruned.
    uint64_t cum_total_txns_ = 0;
    uint64_t cum_total_bytes_ = 0;
    uint64_t cum_total_dram_bytes_ = 0;
    uint64_t cum_max_complete_cycle_ = 0;
    double cum_latency_sum_ns_ = 0.0;

    std::vector<WindowStats> windows_;
    uint64_t history_window_cycles_ = 0; // 0 = windowed accounting disabled

    // Persistent incremental scheduling state (survives across run() calls).
    std::map<int, std::vector<Segment>> segments_;
    std::map<int, uint64_t> core_port_free_cycle_;
    std::priority_queue<Event, std::vector<Event>, std::greater<Event>> heap_;
    uint64_t max_out_ = 1;

    void enqueue_if_ready(int core_id, int segment_idx, uint32_t axi_id);
    void compute_summary();
};

} // namespace ddrtiming
