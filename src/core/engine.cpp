#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include "address_decoder.hpp"
#include "command_queue.hpp"

namespace ddrtiming {

namespace {
constexpr uint64_t kMinIssueSpacingCycles = 1;

// Nearest-rank percentile over a value->observation-count histogram (a
// std::map iterates in ascending key order, so this walks values smallest
// to largest without a separate sort). Returns an observed value, never an
// interpolated one -- see CoreBurstStats's doc comment for why that's the
// right choice for a naturally discrete quantity like burst size.
uint64_t nearest_rank_percentile(const std::map<uint64_t, uint64_t>& hist, uint64_t total, double pct) {
    if (total == 0) return 0;
    uint64_t rank = static_cast<uint64_t>(std::ceil(pct / 100.0 * static_cast<double>(total)));
    rank = std::min(std::max<uint64_t>(rank, 1), total);
    uint64_t cumulative = 0;
    for (const auto& [value, count] : hist) {
        cumulative += count;
        if (cumulative >= rank) return value;
    }
    return hist.empty() ? 0 : hist.rbegin()->first; // unreachable: rank <= total guarantees an early return
}
}

Engine::Engine(DdrcConfig cfg) : cfg_(std::move(cfg)) {
    int nchannels = std::max(1, cfg_.channels);
    channels_.reserve(static_cast<size_t>(nchannels));
    for (int c = 0; c < nchannels; ++c) {
        channels_.push_back(std::make_unique<ChannelScheduler>(cfg_, c));
    }
    max_out_ = static_cast<uint64_t>(std::max(1, cfg_.max_outstanding_per_id));
    mapped_address_bits_ = AddressDecoder(cfg_).mapped_address_bits();

    if (cfg_.history_window_ns > 0.0) {
        history_window_cycles_ = cfg_.ns_to_cycles(cfg_.history_window_ns);
        if (history_window_cycles_ == 0) history_window_cycles_ = 1; // avoid a degenerate 0-cycle window
    }
}

Engine::~Engine() = default;

void Engine::enqueue_if_ready(int core_id, int segment_idx, uint32_t axi_id) {
    Segment& seg = segments_[core_id][static_cast<size_t>(segment_idx)];
    if (!seg.gate_known) return;
    IdCursor& idc = seg.by_id[axi_id];
    if (idc.queued || idc.pending.empty()) return;

    uint64_t port_free = core_port_free_cycle_[core_id];
    uint64_t id_gate = (idc.outstanding.size() >= max_out_) ? *idc.outstanding.begin() : 0;
    uint64_t ready = std::max({port_free, id_gate, seg.gate_cycle});
    heap_.push({ready, core_id, segment_idx, axi_id});
    idc.queued = true;
}

void Engine::note_high_address_region(uint64_t addr) {
    // Nothing mapped (single-bank config) or every bit mapped: aliasing is
    // either total-by-design or impossible, so there's nothing to report.
    if (mapped_address_bits_ <= 0 || mapped_address_bits_ >= 64) return;
    if (high_address_regions_.size() >= kMaxTrackedHighRegions) return;
    high_address_regions_.insert(addr >> mapped_address_bits_);
}

uint64_t Engine::push_txn(const AxiTxn& txn) {
    {
        uint64_t bytes = static_cast<uint64_t>(txn.size_bytes) * txn.len_beats;
        if (bytes == 0) bytes = txn.size_bytes;
        note_high_address_region(txn.addr);
        if (bytes > 0) note_high_address_region(txn.addr + bytes - 1);
        core_burst_histogram_[txn.core_id][bytes]++;
    }
    AxiTxn t = txn;
    t.txn_id = next_txn_id_++;
    int core_id = t.core_id;
    uint32_t axi_id = t.axi_id;
    uint64_t txn_id = t.txn_id;

    std::vector<Segment>& segs = segments_[core_id];
    if (segs.empty()) {
        Segment first;
        first.gate_known = true;
        first.gate_cycle = 0;
        segs.push_back(std::move(first));
    }
    int seg_idx = static_cast<int>(segs.size() - 1);
    segs[static_cast<size_t>(seg_idx)].by_id[axi_id].pending.push_back(std::move(t));

    enqueue_if_ready(core_id, seg_idx, axi_id);
    return txn_id;
}

void Engine::push_barrier(int core_id) {
    std::vector<Segment>& segs = segments_[core_id];
    if (segs.empty()) return; // nothing pushed for this core yet; no-op
    Segment& cur = segs.back();
    if (cur.by_id.empty()) return; // collapse a barrier with nothing before it

    cur.closed = true;

    bool drained = true;
    for (auto& [id, idc] : cur.by_id) {
        if (!idc.pending.empty()) { drained = false; break; }
    }

    Segment next_seg;
    if (drained) {
        // A previous run() call already fully processed `cur` before this
        // barrier arrived -- the gate is already known, so the next segment
        // can accept transactions immediately.
        next_seg.gate_known = true;
        next_seg.gate_cycle = cur.max_complete;
    }
    segs.push_back(std::move(next_seg));
}

void Engine::prune_results_before(uint64_t max_txn_id) {
    results_.erase(
        std::remove_if(results_.begin(), results_.end(),
                        [max_txn_id](const TxnResult& r) { return r.txn_id <= max_txn_id; }),
        results_.end());
}

bool Engine::any_channel_has_pending() const {
    for (const auto& ch : channels_) {
        if (ch->has_pending()) return true;
    }
    return false;
}

// Folds a just-dispatched chunk's outcome into its owning txn's in-progress
// accumulator, and finalizes that txn once every one of its chunks has been
// dispatched (chunks can dispatch across many separate drain_one() calls,
// interleaved with other streams' chunks -- see Engine::run()).
void Engine::route_completed_chunk(const DramCommand& done) {
    Segment& seg = segments_[done.core_id][static_cast<size_t>(done.segment_idx)];
    IdCursor& idc = seg.by_id[done.axi_id];
    IdCursor::InProgress& ip = idc.in_progress;

    ip.max_complete = std::max(ip.max_complete, done.complete_cycle);
    if (done.seq_in_txn == 0) ip.dominant_row_status = done.row_status;
    switch (done.row_status) {
        case RowStatus::Hit: ip.hits++; break;
        case RowStatus::Conflict: ip.conflicts++; break;
        case RowStatus::Empty: ip.empties++; break;
    }

    if (ip.has_window) {
        WindowStats& w = windows_[ip.window_index];
        if (done.bankgroup_reuse) w.bankgroup_reuse_count++;
        uint32_t ch = done.addr.channel % static_cast<uint32_t>(channels_.size());
        // Same modulo reduction ChannelScheduler itself uses to index
        // banks_[], so this counts actual physical banks, not raw (possibly
        // out-of-range) decoded field values.
        uint32_t rank_idx = done.addr.rank % static_cast<uint32_t>(std::max(1, cfg_.ranks_per_channel));
        uint32_t bg_idx = done.addr.bankgroup % static_cast<uint32_t>(std::max(1, cfg_.bankgroups));
        uint32_t bank_idx = done.addr.bank % static_cast<uint32_t>(std::max(1, cfg_.banks_per_group));
        uint64_t bank_key = ((static_cast<uint64_t>(ch) * static_cast<uint64_t>(std::max(1, cfg_.ranks_per_channel)) + rank_idx) *
                                  static_cast<uint64_t>(std::max(1, cfg_.bankgroups)) + bg_idx) *
                                 static_cast<uint64_t>(std::max(1, cfg_.banks_per_group)) + bank_idx;
        w.active_banks.insert(bank_key);

        if (w.dram_bytes_per_channel.size() <= ch) w.dram_bytes_per_channel.resize(ch + 1, 0);
        w.dram_bytes_per_channel[ch] += done.bytes;
    }

    ip.chunks_dispatched++;
    if (ip.chunks_dispatched == ip.num_chunks) {
        finalize_in_progress_txn(done.core_id, done.segment_idx, done.axi_id);
    }
}

// Ports the old per-txn "after the chunk loop" bookkeeping verbatim, just
// reading from IdCursor::InProgress instead of loop-local variables, since
// it can now run at a different program point than when the txn's chunks
// were generated (see Engine::run()).
void Engine::finalize_in_progress_txn(int core_id, int segment_idx, uint32_t axi_id) {
    Segment& seg = segments_[core_id][static_cast<size_t>(segment_idx)];
    IdCursor& idc = seg.by_id[axi_id];
    IdCursor::InProgress& ip = idc.in_progress;

    TxnResult res;
    res.txn_id = ip.txn_id;
    res.core_id = ip.core_id;
    res.type = ip.type;
    res.addr = ip.addr;
    res.issue_cycle = ip.issue_cycle;
    res.bytes = static_cast<uint32_t>(ip.bytes);
    res.complete_cycle = ip.max_complete;
    res.latency_ns = static_cast<double>(ip.max_complete - ip.issue_cycle) * cfg_.clock_period_ns();
    res.dram_bytes = ip.num_chunks * static_cast<uint32_t>(ip.chunk_bytes);
    res.dominant_row_status = ip.dominant_row_status;
    res.hits = ip.hits;
    res.conflicts = ip.conflicts;
    res.empties = ip.empties;
    results_.push_back(res);

    cum_total_txns_++;
    cum_total_bytes_ += res.bytes;
    cum_total_dram_bytes_ += res.dram_bytes;
    cum_latency_sum_ns_ += res.latency_ns;
    cum_max_complete_cycle_ = std::max(cum_max_complete_cycle_, res.complete_cycle);

    idc.outstanding.insert(ip.max_complete);

    if (ip.has_window) {
        WindowStats& w = windows_[ip.window_index];
        if (res.type == TxnType::Read) w.bytes_read += res.bytes;
        else w.bytes_written += res.bytes;
        w.dram_bytes += res.dram_bytes;
        w.txn_count++;
        w.hits += res.hits;
        w.conflicts += res.conflicts;
        w.empties += res.empties;
        w.max_outstanding_count = std::max(w.max_outstanding_count, static_cast<uint64_t>(idc.outstanding.size()));
    }

    idc.pending.pop_front();
    seg.max_complete = std::max(seg.max_complete, ip.max_complete);

    if (!idc.pending.empty()) {
        uint64_t next_port_free = core_port_free_cycle_[core_id];
        uint64_t next_id_gate = (idc.outstanding.size() >= max_out_) ? *idc.outstanding.begin() : 0;
        heap_.push({std::max({next_port_free, next_id_gate, seg.gate_cycle}), core_id, segment_idx, axi_id, 0});
        // idc.queued stays true -- this cursor still has an event pending.
    } else {
        idc.queued = false;
        if (seg.closed) {
            bool drained = true;
            for (auto& [id, c] : seg.by_id) {
                if (!c.pending.empty()) { drained = false; break; }
            }
            if (drained) {
                std::vector<Segment>& segs = segments_[core_id];
                size_t next_idx = static_cast<size_t>(segment_idx) + 1;
                if (next_idx < segs.size() && !segs[next_idx].gate_known) {
                    segs[next_idx].gate_known = true;
                    segs[next_idx].gate_cycle = seg.max_complete;
                    for (auto& [nid, ncur] : segs[next_idx].by_id) {
                        (void)ncur;
                        enqueue_if_ready(core_id, static_cast<int>(next_idx), nid);
                    }
                }
            }
        }
    }
}

void Engine::run() {
    AddressDecoder decoder(cfg_);

    // Loop while there is either front-end work to consider (the heap) or
    // dispatched-but-still-queued commands sitting in some channel (which
    // can only be true once the heap has momentarily run dry -- see the
    // "else" branch below). Whichever a channel's FR-FCFS selection defers,
    // it defers only until the *next* time that channel needs to make room
    // or until this final drain -- never indefinitely.
    while (!heap_.empty() || any_channel_has_pending()) {
        if (heap_.empty()) {
            // Nothing left to admit right now -- force one dispatch so
            // finalization can (maybe) push more front-end work and revive
            // the loop above. Picks the first channel with anything queued;
            // which one is arbitrary since channels are independent.
            for (auto& channel : channels_) {
                if (channel->has_pending()) {
                    DramCommand done = channel->drain_one();
                    route_completed_chunk(done);
                    break;
                }
            }
            continue;
        }

        Event ev = heap_.top();
        heap_.pop();

        Segment& seg = segments_[ev.core_id][static_cast<size_t>(ev.segment_idx)];
        IdCursor& idc = seg.by_id[ev.axi_id];

        if (ev.chunk_resume_idx == 0) {
            if (idc.pending.empty()) continue;

            // Recompute the true issue cycle: gate on this id's outstanding
            // cap, the core's shared port being free, and the segment's
            // barrier gate.
            uint64_t port_free = core_port_free_cycle_[ev.core_id];
            uint64_t id_gate = (idc.outstanding.size() >= max_out_) ? *idc.outstanding.begin() : 0;
            uint64_t issue_cycle = std::max({port_free, id_gate, seg.gate_cycle});

            if (issue_cycle != ev.ready_cycle) {
                // Stale entry -- the port advanced (another id from this core
                // dispatched) since this was queued. Reschedule with the fresh gate.
                heap_.push({issue_cycle, ev.core_id, ev.segment_idx, ev.axi_id, 0});
                continue;
            }
            // Drain this id's outstanding entries that have completed by now.
            while (!idc.outstanding.empty() && *idc.outstanding.begin() <= issue_cycle) {
                idc.outstanding.erase(idc.outstanding.begin());
            }

            const AxiTxn& txn = idc.pending.front();
            uint64_t total_bytes = static_cast<uint64_t>(txn.size_bytes) * txn.len_beats;
            if (total_bytes == 0) total_bytes = txn.size_bytes;
            // A genuinely zero-sized transaction (size_bytes==0 and
            // len_beats==0) would otherwise underflow the window math below
            // (txn.addr + 0 - 1); treat it as the smallest possible access
            // rather than corrupting num_chunks into a huge/wrapped value.
            if (total_bytes == 0) total_bytes = 1;

            // DRAM only ever transfers whole burst-aligned windows
            // (chunk_bytes each) -- never a partial burst -- so the windows
            // touched are determined by aligning [addr, addr+total_bytes) to
            // that grid, not by walking chunk_bytes forward from addr itself
            // (addr need not be aligned). Every window costs a full burst's
            // worth of physical transfer time and dram_bytes, even where it
            // only partially overlaps what was actually requested -- that
            // gap is over-fetch.
            uint64_t chunk_bytes = static_cast<uint64_t>(std::max(1, cfg_.data_bus_bytes)) *
                                    static_cast<uint64_t>(std::max(1, cfg_.burst_beats));
            uint64_t first_window = (txn.addr / chunk_bytes) * chunk_bytes;
            uint64_t last_window = ((txn.addr + total_bytes - 1) / chunk_bytes) * chunk_bytes;
            uint32_t num_chunks = static_cast<uint32_t>((last_window - first_window) / chunk_bytes) + 1;

            IdCursor::InProgress& ip = idc.in_progress;
            ip = IdCursor::InProgress{};
            ip.txn_id = txn.txn_id;
            ip.core_id = txn.core_id;
            ip.type = txn.type;
            ip.addr = txn.addr;
            ip.issue_cycle = issue_cycle;
            ip.bytes = total_bytes;
            ip.chunk_bytes = chunk_bytes;
            ip.first_window_addr = first_window;
            ip.num_chunks = num_chunks;
            ip.next_chunk_idx = 0;
            ip.chunks_dispatched = 0;
            ip.max_complete = issue_cycle;

            if (history_window_cycles_ > 0) {
                size_t window_index = static_cast<size_t>(issue_cycle / history_window_cycles_);
                if (windows_.size() <= window_index) windows_.resize(window_index + 1);
                ip.has_window = true;
                ip.window_index = window_index;
            }

            // Front-end port advance happens at admission, decoupled from
            // this txn's (possibly much later) dispatch/completion --
            // exactly like today: a core may have several of its own txns
            // simultaneously in flight, up to the outstanding cap.
            core_port_free_cycle_[ev.core_id] = issue_cycle + kMinIssueSpacingCycles;
        }

        IdCursor::InProgress& ip = idc.in_progress;
        uint32_t i = ev.chunk_resume_idx; // == 0 also correct: freshly (re)initialized above

        uint64_t window_addr = ip.first_window_addr + static_cast<uint64_t>(i) * ip.chunk_bytes;
        DramCommand cmd;
        cmd.txn_id = ip.txn_id;
        cmd.core_id = ip.core_id;
        cmd.segment_idx = ev.segment_idx;
        cmd.axi_id = ev.axi_id;
        cmd.type = ip.type;
        cmd.addr = decoder.decode(window_addr);
        cmd.bytes = static_cast<uint32_t>(ip.chunk_bytes); // physical: always a full burst
        cmd.seq_in_txn = i;
        cmd.total_in_txn = ip.num_chunks;

        uint32_t ch = cmd.addr.channel % static_cast<uint32_t>(channels_.size());
        ChannelScheduler& channel = *channels_[ch];
        if (!channel.has_room()) {
            // Make exactly one slot's worth of room. The dispatched command
            // may belong to a completely different stream (possibly a
            // different core) than the one we're about to admit -- that's
            // the reordering this whole design exists for.
            DramCommand done = channel.drain_one();
            route_completed_chunk(done);
        }
        channel.try_admit(cmd, ip.issue_cycle); // room guaranteed by the check above

        // Admit exactly one chunk per heap turn, then yield: pushing a
        // continuation (rather than looping through all of this txn's
        // chunks synchronously) is what lets another core's chunks get a
        // turn to admit into the same channel queue in between -- without
        // that, this txn's own chunks would always be the only candidates
        // FR-FCFS ever sees.
        if (i + 1 < ip.num_chunks) {
            heap_.push({ip.issue_cycle, ev.core_id, ev.segment_idx, ev.axi_id, i + 1});
        }
        // else: every chunk is now admitted (not necessarily dispatched --
        // some may still be queued). Finalization fires from
        // route_completed_chunk() once the last of them actually drains,
        // whenever that happens to be.
    }

    compute_summary();
}

CoreBurstStats Engine::core_burst_stats_at(size_t index) const {
    CoreBurstStats out;
    if (index >= core_burst_histogram_.size()) return out;
    auto it = core_burst_histogram_.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(index));
    out.core_id = it->first;

    const std::map<uint64_t, uint64_t>& hist = it->second;
    uint64_t total = 0;
    uint64_t sum = 0;
    for (const auto& [bytes, count] : hist) {
        total += count;
        sum += bytes * count;
    }
    out.txn_count = total;
    out.total_bytes = sum;
    out.mean_bytes = total > 0 ? static_cast<double>(sum) / static_cast<double>(total) : 0.0;
    out.min_bytes = hist.empty() ? 0 : hist.begin()->first;
    out.max_bytes = hist.empty() ? 0 : hist.rbegin()->first;
    out.p25_bytes = nearest_rank_percentile(hist, total, 25.0);
    out.p50_bytes = nearest_rank_percentile(hist, total, 50.0);
    out.p75_bytes = nearest_rank_percentile(hist, total, 75.0);
    return out;
}

void Engine::compute_summary() {
    SummaryStats s;
    s.total_txns = cum_total_txns_;
    s.total_bytes = cum_total_bytes_;
    s.total_dram_bytes = cum_total_dram_bytes_;
    s.total_cycles = cum_max_complete_cycle_;
    s.sim_time_ns = static_cast<double>(cum_max_complete_cycle_) * cfg_.clock_period_ns();
    s.peak_bandwidth_gbps = cfg_.peak_bandwidth_gbps();

    if (s.sim_time_ns > 0.0) {
        s.avg_bandwidth_gbps = static_cast<double>(s.total_bytes) / s.sim_time_ns;
        s.avg_dram_bandwidth_gbps = static_cast<double>(s.total_dram_bytes) / s.sim_time_ns;
    }
    if (s.peak_bandwidth_gbps > 0.0) {
        s.bandwidth_utilization_pct = s.avg_dram_bandwidth_gbps / s.peak_bandwidth_gbps * 100.0;
    }
    if (s.total_dram_bytes > 0) {
        s.burst_efficiency_pct = static_cast<double>(s.total_bytes) / static_cast<double>(s.total_dram_bytes) * 100.0;
    }
    if (s.total_txns > 0) {
        s.avg_latency_ns = cum_latency_sum_ns_ / static_cast<double>(s.total_txns);
    }

    uint64_t total_hits = 0, total_conflicts = 0, total_empties = 0;
    uint64_t total_refresh_cycles = 0, total_turnaround_cycles = 0;
    uint64_t total_bankgroup_reuse = 0;
    for (const auto& ch : channels_) {
        const ChannelStats& cs = ch->stats();
        total_hits += cs.hits;
        total_conflicts += cs.conflicts;
        total_empties += cs.empties;
        total_refresh_cycles += cs.refresh_cycles;
        total_turnaround_cycles += cs.turnaround_cycles;
        total_bankgroup_reuse += cs.bankgroup_reuse_count;
    }

    uint64_t total_cmds = total_hits + total_conflicts + total_empties;
    if (total_cmds > 0) {
        s.page_hit_rate_pct = static_cast<double>(total_hits) / total_cmds * 100.0;
        s.row_conflict_rate_pct = static_cast<double>(total_conflicts) / total_cmds * 100.0;
        s.row_empty_rate_pct = static_cast<double>(total_empties) / total_cmds * 100.0;
        s.bankgroup_reuse_rate_pct = static_cast<double>(total_bankgroup_reuse) / total_cmds * 100.0;
    }

    uint64_t channel_time_budget = static_cast<uint64_t>(channels_.size()) * cum_max_complete_cycle_;
    if (channel_time_budget > 0) {
        s.refresh_overhead_pct = static_cast<double>(total_refresh_cycles) / channel_time_budget * 100.0;
        s.turnaround_overhead_pct = static_cast<double>(total_turnaround_cycles) / channel_time_budget * 100.0;
    }

    s.mapped_address_bits = mapped_address_bits_;
    s.high_address_regions = high_address_regions_.size();
    summary_ = s;
}

} // namespace ddrtiming
