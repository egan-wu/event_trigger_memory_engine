#pragma once
#include <array>
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

    // [S] bus-time attribution: where the run's channel-time went, as
    // percentages of (channels * total_cycles). These eight partition that
    // budget -- every cycle is charged to exactly one cause -- so they sum
    // to 100% and can be read directly as "the largest non-data entry is
    // what cost the bandwidth". Distinct from refresh_overhead_pct /
    // turnaround_overhead_pct above, which measure the delay each of those
    // constraints added to individual commands and can therefore overlap
    // each other and exceed the bubble that actually appeared on the bus.
    double attr_data_pct = 0.0;              // data bursts on the bus
    double attr_row_miss_exposed_pct = 0.0;  // PRE/ACT/tRCD not hidden behind other banks
    double attr_refresh_pct = 0.0;
    double attr_turnaround_pct = 0.0;        // R<->W bus direction changes
    double attr_twtr_pct = 0.0;              // write-to-read DRAM recovery
    double attr_tccd_l_excess_pct = 0.0;     // tCCD_L paid where tCCD_S would have done
    double attr_frontend_idle_pct = 0.0;     // bus waited for a command to arrive
    double attr_other_pct = 0.0;             // command-bus slots, same-bank column pipeline
    // [S] Data-bus direction changes across all channels. Each costs a
    // turnaround, and each write->read also a tWTR, so this is the count
    // behind attr_turnaround_pct/attr_twtr_pct -- and what
    // ddrc_resources.write_policy "batch" is meant to reduce.
    uint64_t rw_direction_switches = 0;

    // [F] Analytical ceilings, computed from the config alone (not measured)
    // -- for telling "this run is close to what the config can ever do" apart
    // from "this run has real headroom left". Deliberately narrow: each is a
    // ceiling under one specific constraint in isolation, not a combined
    // achievable maximum, so a run can legitimately sit below more than one
    // of them at once without that being a contradiction.
    double ceiling_refresh_pct = 0.0;   // 100*(1 - tRFC/tREFI): the refresh-only ceiling
    double ceiling_tccd_l_gbps = 0.0;   // if every column command paid tCCD_L
    double ceiling_tfaw_gbps = 0.0;     // if every access were a fresh activate, tFAW-rate-bound
    double headroom_pct = 0.0;          // ceiling_refresh_pct - bandwidth_utilization_pct
    // [F] max/min of physical bytes moved per channel, over channels that
    // carried any traffic (1.0 for a single channel or if none did) -- a
    // multi-channel config's utilization can look fine in aggregate while
    // one channel does all the work; see Engine::channel_dram_bytes().
    double channel_imbalance_ratio = 1.0;

    // [F] Latency distribution, whole run. avg_latency_ns above is the mean;
    // these are approximate percentiles from a fixed log-scale histogram
    // (see kLatencyBuckets in engine.cpp) -- each is the *upper edge* of the
    // bucket containing the nearest-rank observation, coarsened to the
    // bucket's resolution (~41% relative width) rather than interpolated.
    // latency_max_ns is exact (tracked independently of the histogram) and
    // every percentile is clamped to it, so p50 <= p95 <= p99 <= max always
    // holds even when the true value and its bucket edge fall either side
    // of the exact maximum.
    double latency_p50_ns = 0.0;
    double latency_p95_ns = 0.0;
    double latency_p99_ns = 0.0;
    double latency_max_ns = 0.0;
};

// Distribution of AXI burst sizes (logical bytes requested per transaction,
// i.e. size_bytes * len_beats -- the same quantity as TxnResult::bytes) one
// core has pushed, so a caller can see whether one core's requests are
// systematically smaller/larger than another's -- a small average burst
// size concentrates DDRC/timing overhead (tRCD/tRP/CAS) over less useful
// data per command, which can bottleneck a core even when aggregate
// bandwidth utilization looks fine. Percentiles use the nearest-rank method
// (the value at the N-th smallest observation, N = ceil(pct/100 * count))
// rather than interpolating between two observed sizes: burst sizes are
// naturally few and discrete (they fall out of a handful of size_bytes/
// len_beats combinations), so every reported value is one that was actually
// observed, not a synthetic in-between number. See Engine::core_burst_stats_at().
struct CoreBurstStats {
    int core_id = 0;
    uint64_t txn_count = 0;
    uint64_t total_bytes = 0;
    double mean_bytes = 0.0;
    uint64_t min_bytes = 0;
    uint64_t p25_bytes = 0;
    uint64_t p50_bytes = 0; // median
    uint64_t p75_bytes = 0;
    uint64_t max_bytes = 0;
};

// A second, independent per-core breakdown alongside CoreBurstStats -- kept
// as its own struct/array rather than folded into CoreBurstStats because
// it's recorded at a different point in a transaction's life: burst size is
// known at push_txn() (an input-stream property), everything here is only
// known once a transaction actually completes (a scheduling outcome). See
// Engine::core_runtime_stats_at(). Percentiles use the same log-scale
// histogram and "bucket upper edge" convention as SummaryStats's
// latency_p50_ns/etc. -- see that struct's comment.
struct CoreRuntimeStats {
    int core_id = 0;
    uint64_t txn_count = 0;
    uint64_t read_bytes = 0;
    uint64_t write_bytes = 0;
    uint64_t hits = 0, conflicts = 0, empties = 0;
    double avg_latency_ns = 0.0;
    double latency_p50_ns = 0.0;
    double latency_p95_ns = 0.0;
    // Mean, per completed transaction on this core, of the cycles (as ns)
    // it sat ready at the front end but held back specifically because its
    // (core_id, axi_id) stream was at max_outstanding_per_id -- i.e. the
    // portion of its issue delay attributable to that cap rather than to
    // the core's own port or a barrier gate. 0 for a core that was never
    // actually gated by the cap, even if the cap is configured low; see
    // IdCursor::InProgress::outstanding_wait_cycles for exactly which term
    // this measures.
    double outstanding_wait_avg_ns = 0.0;
};

// One fixed-size bucket of simulated time (history_window_ns in the config).
// Accumulated incrementally at completion time -- like SummaryStats,
// unaffected by prune_results_before() -- so a caller can build a
// bandwidth/byte-access history independent of how often it happens to call
// run() or drain/prune results(). See README "Windowed history".
//
// [F] bytes_read/bytes_written/dram_bytes/txn_count/hits/conflicts/empties/
// bankgroup_reuse_count/active_banks/dram_bytes_per_channel are indexed by
// each transaction's (completion-clamped) complete_cycle -- what this
// window's bus actually delivered -- NOT issue_cycle. They used to be
// issue-indexed, which front-loaded the whole series under a deep
// outstanding queue badly enough that a window's bandwidth could exceed
// peak_bandwidth_gbps, a physical impossibility (see bench/README.md's
// known-limitations history for where this was first flagged). A
// transaction longer than one window is not split -- the whole transfer
// lands in the window containing its completion, same simplification the
// old code made for issue_cycle. offered_bytes/offered_txn_count below are
// the (still issue-indexed) counterpart for when the front-end request
// rate itself is what's being examined; max_outstanding_count also stays
// issue-indexed, since occupancy is a front-end-queue quantity by
// definition, not something a transaction "delivers" on completion.
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

    // [F] Issue-time ("offered") counterpart to the completion-time
    // ("delivered") fields above -- see the class comment for why both
    // exist. Bucketed by issue_cycle, same as every field here was before
    // this fix; a deep outstanding queue front-loads this series the same
    // way it used to front-load the whole struct, which is exactly the
    // failure mode delivered bytes no longer have.
    uint64_t offered_bytes = 0;
    uint64_t offered_txn_count = 0;
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

    // [F] front-end: AXI requires same-ID responses to return in the order
    // they were issued. The channel-level completion this id's Nth txn
    // actually computes (IdCursor::InProgress::max_complete) is a raw
    // per-channel timing result and can, in a multi-channel config, come out
    // SMALLER than the (N-1)th txn's own completion -- e.g. txn N-1 lands on
    // a heavily-loaded channel and txn N (same id, admitted only after N-1
    // fully finalizes) lands on an idle one. A real controller holds N's
    // response back until N-1's has gone out, so the reported completion
    // can never regress id-to-id. last_complete_cycle is the previous txn's
    // (already-clamped) completion on this id; finalize_in_progress_txn()
    // clamps every new completion to at least this value -- see its use
    // there for the exact five places that must read the clamped number
    // instead of InProgress::max_complete directly. Scoped identically to
    // `outstanding` above (per-segment, like the rest of IdCursor): a new
    // segment only ever becomes reachable once its predecessor's barrier
    // gate (Segment::gate_cycle, itself the prior segment's max_complete
    // across every id) has already forced every one of its own txns to
    // issue no earlier than that -- so a fresh IdCursor starting this at 0
    // for a new segment can never under-clamp.
    uint64_t last_complete_cycle = 0;

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
        // [F] Windowed-history accumulators for this one transaction's
        // chunks, filled in by route_completed_chunk() as they complete and
        // folded into windows_[] at finalize_in_progress_txn() time, once
        // the transaction's own completion window is known -- see
        // WindowStats's class comment for why this can no longer be
        // committed straight into windows_[] per chunk the way it used to
        // be (that used the txn's issue window, computed before any chunk
        // had actually finished).
        uint32_t win_bankgroup_reuse = 0;
        std::set<uint64_t> win_active_banks;
        std::vector<uint64_t> win_dram_bytes_per_channel;
        // [F] Set once, when issue_cycle is finalized in Engine::run(): the
        // portion of that cycle contributed by this id's outstanding cap
        // being the strictly-binding term over the core's own port and the
        // segment's barrier gate (0 if it wasn't -- including when the cap
        // never bound at all). Carried through to finalize_in_progress_txn()
        // for CoreRuntimeStats::outstanding_wait_avg_ns.
        uint64_t outstanding_wait_cycles = 0;
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

    // Per-core AXI burst-size distribution -- see CoreBurstStats. Cumulative
    // since the engine was created and unaffected by prune_results_before(),
    // like summary()/windows() (it's recorded at push_txn() time, an
    // input-stream property, not a scheduling outcome). Cores are discovered
    // dynamically from whatever core_id values are pushed -- there's no
    // config-level core count -- so read this by index (ascending core_id
    // order), not by core_id directly; re-check num_cores_with_burst_stats()
    // before iterating if a transaction with a not-yet-seen core_id might
    // have been pushed since your last call.
    size_t num_cores_with_burst_stats() const { return core_burst_histogram_.size(); }
    CoreBurstStats core_burst_stats_at(size_t index) const;

    // [F] Per-core completion-time stats -- see CoreRuntimeStats. Same
    // indexing convention as core_burst_stats_at() (ascending core_id
    // order, re-check the count before iterating), but populated from a
    // different, independent accumulator: a core only appears here once one
    // of its transactions has actually completed, not merely been pushed.
    size_t num_core_runtime_stats() const { return core_runtime_.size(); }
    CoreRuntimeStats core_runtime_stats_at(size_t index) const;

    // [F] Physical (DRAM-side, full-burst) bytes moved by each channel over
    // the whole run so far -- the non-windowed counterpart to
    // WindowStats::dram_bytes_per_channel, for the same reason: aggregate
    // bandwidth_utilization_pct can't distinguish "every channel at 50%"
    // from "one channel maxed, the rest idle". One entry per channel,
    // channel index order.
    std::vector<uint64_t> channel_dram_bytes() const;

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
    double cum_latency_max_ns_ = 0.0; // [F] exact, independent of the histogram's bucketing

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

    // See CoreBurstStats / num_cores_with_burst_stats() / core_burst_stats_at().
    // core_id -> (burst_size_bytes -> observation count). A histogram rather
    // than a stored list of every burst size: real workloads use a handful
    // of distinct AXI burst sizes, so this stays small regardless of how
    // many transactions are pushed, while still giving exact (not sampled or
    // approximated) quantiles.
    std::map<int, std::map<uint64_t, uint64_t>> core_burst_histogram_;

    // [F] Fixed log-scale latency histogram: 2 buckets per power-of-two
    // octave (~41% relative width, i.e. edges at 1, 2^0.5, 2, 2^1.5, 4, ...
    // ns), 64 buckets reaching up to 2^32 ns (~4.3s) -- see
    // SummaryStats::latency_p50_ns's comment for why percentiles report a
    // bucket's upper edge rather than an interpolated value. Kept as a
    // small fixed array (not a map) since it's touched once per completed
    // transaction and every core needs its own.
    static constexpr size_t kLatencyBuckets = 64;
    static size_t latency_bucket_index(double latency_ns);
    static double latency_bucket_upper_edge_ns(size_t index);
    static double percentile_from_latency_hist(const std::array<uint64_t, kLatencyBuckets>& hist,
                                                uint64_t total, double pct);

    // [F] Per-core completion-time accumulator behind CoreRuntimeStats --
    // updated once per finalized transaction (finalize_in_progress_txn()),
    // cumulative since the engine was created like core_burst_histogram_
    // above, just keyed on a different lifecycle event.
    struct CoreRuntimeAccum {
        uint64_t txn_count = 0;
        uint64_t read_bytes = 0, write_bytes = 0;
        uint64_t hits = 0, conflicts = 0, empties = 0;
        double latency_sum_ns = 0.0;
        double latency_max_ns = 0.0; // exact; clamps this core's bucketed percentiles below
        std::array<uint64_t, kLatencyBuckets> latency_hist{};
        double outstanding_wait_sum_ns = 0.0;
    };
    std::map<int, CoreRuntimeAccum> core_runtime_;

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
