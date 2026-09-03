#include "engine.hpp"

#include <algorithm>
#include <utility>

#include "address_decoder.hpp"
#include "command_queue.hpp"

namespace ddrtiming {

namespace {
constexpr uint64_t kMinIssueSpacingCycles = 1;
}

Engine::Engine(DdrcConfig cfg) : cfg_(std::move(cfg)) {
    int nchannels = std::max(1, cfg_.channels);
    channels_.reserve(static_cast<size_t>(nchannels));
    for (int c = 0; c < nchannels; ++c) {
        channels_.push_back(std::make_unique<ChannelScheduler>(cfg_, c));
    }
    max_out_ = static_cast<uint64_t>(std::max(1, cfg_.max_outstanding_per_id));
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

uint64_t Engine::push_txn(const AxiTxn& txn) {
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

void Engine::run() {
    AddressDecoder decoder(cfg_);

    while (!heap_.empty()) {
        Event ev = heap_.top();
        heap_.pop();

        Segment& seg = segments_[ev.core_id][static_cast<size_t>(ev.segment_idx)];
        IdCursor& idc = seg.by_id[ev.axi_id];
        if (idc.pending.empty()) continue;

        // Recompute the true issue cycle: gate on this id's outstanding cap,
        // the core's shared port being free, and the segment's barrier gate.
        uint64_t port_free = core_port_free_cycle_[ev.core_id];
        uint64_t id_gate = (idc.outstanding.size() >= max_out_) ? *idc.outstanding.begin() : 0;
        uint64_t issue_cycle = std::max({port_free, id_gate, seg.gate_cycle});

        if (issue_cycle != ev.ready_cycle) {
            // Stale entry -- the port advanced (another id from this core
            // dispatched) since this was queued. Reschedule with the fresh gate.
            heap_.push({issue_cycle, ev.core_id, ev.segment_idx, ev.axi_id});
            continue;
        }
        // Drain this id's outstanding entries that have completed by now.
        while (!idc.outstanding.empty() && *idc.outstanding.begin() <= issue_cycle) {
            idc.outstanding.erase(idc.outstanding.begin());
        }

        int core_id = ev.core_id;
        const AxiTxn& txn = idc.pending.front();
        uint64_t total_bytes = static_cast<uint64_t>(txn.size_bytes) * txn.len_beats;
        if (total_bytes == 0) total_bytes = txn.size_bytes;

        TxnResult res;
        res.txn_id = txn.txn_id;
        res.core_id = txn.core_id;
        res.type = txn.type;
        res.addr = txn.addr;
        res.issue_cycle = issue_cycle;
        res.bytes = static_cast<uint32_t>(total_bytes);

        // DRAM only ever transfers whole burst-aligned windows (chunk_bytes
        // each) -- never a partial burst -- so the windows touched are
        // determined by aligning [addr, addr+total_bytes) to that grid, not
        // by walking chunk_bytes forward from addr itself (addr need not be
        // aligned). Every window costs a full burst's worth of physical
        // transfer time and dram_bytes, even where it only partially
        // overlaps what was actually requested -- that gap is over-fetch.
        uint64_t chunk_bytes = static_cast<uint64_t>(std::max(1, cfg_.data_bus_bytes)) *
                                static_cast<uint64_t>(std::max(1, cfg_.burst_beats));
        uint64_t first_window = (txn.addr / chunk_bytes) * chunk_bytes;
        uint64_t last_window = ((txn.addr + total_bytes - 1) / chunk_bytes) * chunk_bytes;
        uint32_t num_chunks = static_cast<uint32_t>((last_window - first_window) / chunk_bytes) + 1;

        uint64_t max_complete = issue_cycle;
        for (uint32_t i = 0; i < num_chunks; ++i) {
            uint64_t window_addr = first_window + static_cast<uint64_t>(i) * chunk_bytes;

            DramCommand cmd;
            cmd.txn_id = txn.txn_id;
            cmd.core_id = txn.core_id;
            cmd.type = txn.type;
            cmd.addr = decoder.decode(window_addr);
            cmd.bytes = static_cast<uint32_t>(chunk_bytes); // physical: always a full burst
            cmd.seq_in_txn = i;
            cmd.total_in_txn = num_chunks;

            uint32_t ch = cmd.addr.channel % static_cast<uint32_t>(channels_.size());
            uint64_t complete = channels_[ch]->schedule(cmd, issue_cycle);
            max_complete = std::max(max_complete, complete);

            if (i == 0) res.dominant_row_status = cmd.row_status;
            switch (cmd.row_status) {
                case RowStatus::Hit: res.hits++; break;
                case RowStatus::Conflict: res.conflicts++; break;
                case RowStatus::Empty: res.empties++; break;
            }
        }

        res.complete_cycle = max_complete;
        res.latency_ns = static_cast<double>(max_complete - issue_cycle) * cfg_.clock_period_ns();
        res.dram_bytes = num_chunks * static_cast<uint32_t>(chunk_bytes);
        results_.push_back(res);

        cum_total_txns_++;
        cum_total_bytes_ += res.bytes;
        cum_total_dram_bytes_ += res.dram_bytes;
        cum_latency_sum_ns_ += res.latency_ns;
        cum_max_complete_cycle_ = std::max(cum_max_complete_cycle_, res.complete_cycle);

        idc.outstanding.insert(max_complete);
        idc.pending.pop_front();
        core_port_free_cycle_[core_id] = issue_cycle + kMinIssueSpacingCycles;
        seg.max_complete = std::max(seg.max_complete, max_complete);

        if (!idc.pending.empty()) {
            uint64_t next_port_free = core_port_free_cycle_[core_id];
            uint64_t next_id_gate = (idc.outstanding.size() >= max_out_) ? *idc.outstanding.begin() : 0;
            heap_.push({std::max({next_port_free, next_id_gate, seg.gate_cycle}), core_id, ev.segment_idx, ev.axi_id});
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
                    size_t next_idx = static_cast<size_t>(ev.segment_idx) + 1;
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

    compute_summary();
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
    for (const auto& ch : channels_) {
        const ChannelStats& cs = ch->stats();
        total_hits += cs.hits;
        total_conflicts += cs.conflicts;
        total_empties += cs.empties;
        total_refresh_cycles += cs.refresh_cycles;
        total_turnaround_cycles += cs.turnaround_cycles;
    }

    uint64_t total_cmds = total_hits + total_conflicts + total_empties;
    if (total_cmds > 0) {
        s.page_hit_rate_pct = static_cast<double>(total_hits) / total_cmds * 100.0;
        s.row_conflict_rate_pct = static_cast<double>(total_conflicts) / total_cmds * 100.0;
        s.row_empty_rate_pct = static_cast<double>(total_empties) / total_cmds * 100.0;
    }

    uint64_t channel_time_budget = static_cast<uint64_t>(channels_.size()) * cum_max_complete_cycle_;
    if (channel_time_budget > 0) {
        s.refresh_overhead_pct = static_cast<double>(total_refresh_cycles) / channel_time_budget * 100.0;
        s.turnaround_overhead_pct = static_cast<double>(total_turnaround_cycles) / channel_time_budget * 100.0;
    }

    summary_ = s;
}

} // namespace ddrtiming
