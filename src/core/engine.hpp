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
    // Fraction of column commands that paid tCCD_L (same bank group as the
    // immediately preceding command on that channel) instead of tCCD_S
    // (different bank group, typically zero extra bubble beyond the raw
    // transfer time -- the entire reason bank groups exist). A sequential
    // stream whose address mapping puts bank-group bits anywhere but the
    // fastest-changing position will show this near 100%; see README
    // "Bank-group ordering".
    double bankgroup_reuse_rate_pct = 0.0;
    // Input-integrity check, not a performance metric. The address map only
    // decodes the low mapped_address_bits bits of an address (see
    // AddressDecoder::mapped_address_bits); everything above is silently
    // ignored. high_address_regions counts the distinct values of those
    // ignored high bits across every pushed transaction (saturating at
    // kMaxTrackedHighRegions). 1 is normal -- a trace that lives entirely
    // above some DRAM base offset (e.g. 0x8000_0000) is fine, the offset
    // just drops out. >1 means distinct regions of the trace are being
    // folded onto the SAME banks/rows/columns: e.g. per-core buffers placed
    // further apart than the configured DRAM capacity, which then share open
    // rows and report an unrealistically high page-hit rate. 0 when nothing
    // has been pushed or nothing is mapped.
    int mapped_address_bits = 0;
    uint64_t high_address_regions = 0;
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
    // Of the commands counted in hits+conflicts+empties above, how many paid
    // tCCD_L (same bank group as the previous command on their channel)
    // instead of tCCD_S -- see SummaryStats::bankgroup_reuse_rate_pct.
    uint64_t bankgroup_reuse_count = 0;
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
    // Physical (DRAM-side, full-burst) bytes dispatched to each channel in
    // this window, indexed by channel number and grown lazily -- so imbalance
    // across channels is visible even when it's invisible in the aggregate:
    // "50% overall utilization" is a completely different situation if it's
    // 2 channels at 25% each versus one at 100% and one idle, and dram_bytes
    // above (summed across channels) can't tell those apart.
    std::vector<uint64_t> dram_bytes_per_channel;
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

    // Bookkeeping for whichever txn is currently being chunked/dispatched
    // for this id (only one at a time, matching `queued`'s one-event-per-id
    // invariant). A txn's chunks are now admitted into their channels one
    // per heap turn (see Engine::run()), interleaved with other cores'
    // chunks -- that interleaving is what lets the FR-FCFS channel scheduler
    // (command_queue.hpp) actually have competing candidates to choose
    // between, instead of always seeing one stream's commands in isolation.
    // Dispatch (draining) can then lag admission by an arbitrary number of
    // other chunks, so this struct accumulates results across however many
    // separate dispatch events it takes until every one of this txn's own
    // chunks has actually been serviced.
    struct InProgress {
        uint64_t txn_id = 0;
        int core_id = 0;
        TxnType type = TxnType::Read;
        uint64_t addr = 0;
        uint64_t issue_cycle = 0;
        uint64_t bytes = 0;        // logical AXI bytes requested (whole txn)
        uint64_t chunk_bytes = 0;
        uint64_t first_window_addr = 0;
        uint32_t num_chunks = 0;
        uint32_t next_chunk_idx = 0;
        uint32_t chunks_dispatched = 0;
        uint64_t max_complete = 0;
        uint32_t hits = 0, conflicts = 0, empties = 0;
        RowStatus dominant_row_status = RowStatus::Empty;
        bool has_window = false;
        size_t window_index = 0;
    };
    InProgress in_progress;
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
        // 0 = start this id's next pending txn from scratch; >0 = resume an
        // already-started txn's chunk admission at this chunk index (see
        // IdCursor::InProgress). Never affects heap ordering -- only one
        // event per id is ever in the heap at a time (IdCursor::queued).
        uint32_t chunk_resume_idx = 0;
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

    // See SummaryStats::high_address_regions.
    static constexpr size_t kMaxTrackedHighRegions = 1024;
    int mapped_address_bits_ = 0;
    std::set<uint64_t> high_address_regions_;
    void note_high_address_region(uint64_t addr);

    void enqueue_if_ready(int core_id, int segment_idx, uint32_t axi_id);
    void compute_summary();

    // Routes a just-dispatched chunk back to its owning IdCursor::InProgress
    // (identified by cmd.core_id/segment_idx/axi_id), folding in its
    // hit/conflict/empty and window stats; finalizes the owning txn once
    // every one of its chunks has been dispatched.
    void route_completed_chunk(const DramCommand& done);
    void finalize_in_progress_txn(int core_id, int segment_idx, uint32_t axi_id);
    bool any_channel_has_pending() const;
};

} // namespace ddrtiming
