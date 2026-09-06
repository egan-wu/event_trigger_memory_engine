#include "command_queue.hpp"

#include <algorithm>

namespace ddrtiming {

ChannelScheduler::ChannelScheduler(const DdrcConfig& cfg, int channel_id)
    : cfg_(cfg), channel_id_(channel_id) {
    int ranks = std::max(1, cfg_.ranks_per_channel);
    int bgs = std::max(1, cfg_.bankgroups);
    int banks = std::max(1, cfg_.banks_per_group);
    banks_.assign(ranks, std::vector<std::vector<BankState>>(bgs, std::vector<BankState>(banks)));
    ranks_.assign(ranks, RankState{});
}

bool ChannelScheduler::has_room() const {
    return pending_.size() < static_cast<size_t>(std::max(1, cfg_.command_queue_depth));
}

bool ChannelScheduler::try_admit(const DramCommand& cmd, uint64_t ready_cycle) {
    if (!has_room()) return false;
    pending_.push_back(PendingCmd{cmd, ready_cycle, next_seq_++});
    return true;
}

// Would this command be a page-hit, or land on a never-opened ("idle") bank,
// or conflict *right now*, without committing to anything -- used only to
// rank pending_ candidates, never to decide timing. 0=hit, 1=idle bank
// (cheap to prefer over a conflict -- gets bank-group interleaving started),
// 2=conflict (needs precharge+activate against an already-open wrong row).
int ChannelScheduler::peek_priority(const DramCommand& cmd) const {
    uint32_t rank_idx = cmd.addr.rank % static_cast<uint32_t>(ranks_.size());
    uint32_t bg_idx = cmd.addr.bankgroup % static_cast<uint32_t>(banks_[rank_idx].size());
    uint32_t bank_idx = cmd.addr.bank % static_cast<uint32_t>(banks_[rank_idx][bg_idx].size());
    const BankState& bank = banks_[rank_idx][bg_idx][bank_idx];
    if (bank.row_open && bank.open_row == cmd.addr.row) return 0;
    if (!bank.row_open) return 1;
    return 2;
}

uint64_t ChannelScheduler::bank_key_of(const DramCommand& cmd) const {
    uint32_t rank_idx = cmd.addr.rank % static_cast<uint32_t>(ranks_.size());
    uint32_t bg_idx = cmd.addr.bankgroup % static_cast<uint32_t>(banks_[rank_idx].size());
    uint32_t bank_idx = cmd.addr.bank % static_cast<uint32_t>(banks_[rank_idx][bg_idx].size());
    return (static_cast<uint64_t>(rank_idx) * banks_[rank_idx].size() + bg_idx) *
               banks_[rank_idx][bg_idx].size() +
           bank_idx;
}

// Selects the index to service next: a starved candidate (skipped too many
// times) always wins outright; otherwise the best by (priority, arrival
// order), except a hit on a bank that's already at the pagematch streak
// limit is skipped in favor of any candidate on a *different* bank, if one
// is available.
size_t ChannelScheduler::pick_best_index() const {
    long starved = -1;
    for (size_t i = 0; i < pending_.size(); ++i) {
        if (pending_[i].skip_count >= kStarvationLimit &&
            (starved < 0 || pending_[i].seq < pending_[static_cast<size_t>(starved)].seq)) {
            starved = static_cast<long>(i);
        }
    }
    if (starved >= 0) return static_cast<size_t>(starved);

    bool streak_at_limit = consecutive_hit_count_ >= kPagematchLimit;
    bool other_bank_available = false;
    if (streak_at_limit) {
        for (const auto& p : pending_) {
            if (bank_key_of(p.cmd) != last_hit_bank_key_) { other_bank_available = true; break; }
        }
    }

    long best = -1;
    int best_pr = 3;
    uint64_t best_seq = 0;
    for (size_t i = 0; i < pending_.size(); ++i) {
        int pr = peek_priority(pending_[i].cmd);
        if (streak_at_limit && other_bank_available && pr == 0 && bank_key_of(pending_[i].cmd) == last_hit_bank_key_) {
            continue; // this bank's hit streak must yield to another bank this round
        }
        if (best < 0 || pr < best_pr || (pr == best_pr && pending_[i].seq < best_seq)) {
            best = static_cast<long>(i);
            best_pr = pr;
            best_seq = pending_[i].seq;
        }
    }
    return static_cast<size_t>(best);
}

uint64_t ChannelScheduler::apply_refresh_if_due(uint32_t rank_idx, uint64_t earliest_cycle) {
    RankState& rk = ranks_[rank_idx];
    if (!rk.refresh_initialized) {
        rk.next_refresh_due_cycle = cyc(cfg_.tREFI);
        rk.refresh_initialized = true;
    }
    while (earliest_cycle >= rk.next_refresh_due_cycle) {
        uint64_t refresh_end = rk.next_refresh_due_cycle + cyc(cfg_.tRFC);
        if (earliest_cycle < refresh_end) {
            stats_.refresh_cycles += (refresh_end - earliest_cycle);
            earliest_cycle = refresh_end;
        }
        // REFRESH requires every bank in the rank precharged first, and
        // leaves them that way -- a later-arriving command can find this
        // refresh long finished (so it isn't blocked by it above), but the
        // rows it closed stay closed regardless of whether anything was
        // actually waiting on it.
        for (auto& bg : banks_[rank_idx]) {
            for (auto& bank : bg) {
                bank.row_open = false;
                bank.col_ready_cycle = std::max(bank.col_ready_cycle, refresh_end);
                bank.precharge_ready_cycle = std::max(bank.precharge_ready_cycle, refresh_end);
            }
        }
        rk.next_refresh_due_cycle += cyc(cfg_.tREFI);
    }
    return earliest_cycle;
}

uint64_t ChannelScheduler::apply_activate_gating(RankState& rk, uint32_t bankgroup, uint64_t cycle) {
    if (!rk.recent_activates.empty()) {
        bool same_bg = (rk.last_activate_bankgroup == bankgroup);
        uint64_t min_gap = cyc(same_bg ? cfg_.tRRD_L : cfg_.tRRD_S);
        cycle = std::max(cycle, rk.recent_activates.back() + min_gap);
    }
    if (rk.recent_activates.size() >= 4) {
        uint64_t oldest = rk.recent_activates.front();
        uint64_t min_cycle = oldest + cyc(cfg_.tFAW);
        cycle = std::max(cycle, min_cycle);
    }
    while (!rk.recent_activates.empty() && rk.recent_activates.front() + cyc(cfg_.tFAW) <= cycle) {
        rk.recent_activates.pop_front();
    }
    rk.recent_activates.push_back(cycle);
    rk.last_activate_bankgroup = bankgroup;
    return cycle;
}

DramCommand ChannelScheduler::drain_one() {
    size_t best = pick_best_index();

    // Age bookkeeping (starvation): everyone not selected this round gets
    // one step older. Must happen before erasing `best` so indices still
    // line up with pending_.
    for (size_t i = 0; i < pending_.size(); ++i) {
        if (i != best) pending_[i].skip_count++;
    }

    // Pagematch bookkeeping: track consecutive hit-selections to the same
    // bank so pick_best_index() can cap the streak next time.
    uint64_t selected_bank_key = bank_key_of(pending_[best].cmd);
    if (peek_priority(pending_[best].cmd) == 0 && selected_bank_key == last_hit_bank_key_) {
        consecutive_hit_count_++;
    } else if (peek_priority(pending_[best].cmd) == 0) {
        last_hit_bank_key_ = selected_bank_key;
        consecutive_hit_count_ = 1;
    } else {
        last_hit_bank_key_ = ~0ull;
        consecutive_hit_count_ = 0;
    }

    DramCommand cmd = pending_[best].cmd;
    uint64_t arrival_cycle = pending_[best].ready_cycle;
    pending_.erase(pending_.begin() + static_cast<long>(best));

    uint32_t rank_idx = cmd.addr.rank % static_cast<uint32_t>(ranks_.size());
    uint32_t bg_idx = cmd.addr.bankgroup % static_cast<uint32_t>(banks_[rank_idx].size());
    uint32_t bank_idx = cmd.addr.bank % static_cast<uint32_t>(banks_[rank_idx][bg_idx].size());
    BankState& bank = banks_[rank_idx][bg_idx][bank_idx];
    RankState& rank = ranks_[rank_idx];

    // A command can't be serviced before it existed, nor (below) before the
    // shared bus/tCCD spacing from whatever this channel serviced
    // immediately prior allows. Bank readiness is checked per row-status
    // branch below, against whichever of col_ready_cycle/precharge_ready_cycle
    // is actually relevant to that branch.
    uint64_t earliest = arrival_cycle;

    // CAS latency (tCL/tCWL): pipeline delay from this command's own column
    // command to when its data actually appears -- needed below to translate
    // a data-bus-timeline constraint (bus_free_cycle_, which tracks when the
    // *data* bus frees up) back into a floor on col_start.
    double cas_ns = (cmd.type == TxnType::Read) ? cfg_.tCL : cfg_.tCWL;
    uint64_t cas_cycles = cyc(cas_ns);

    if (had_col_cmd_) {
        bool same_bg = (last_col_bankgroup_ == cmd.addr.bankgroup);
        cmd.bankgroup_reuse = same_bg;
        if (same_bg) stats_.bankgroup_reuse_count++;
        uint64_t min_gap = cyc(same_bg ? cfg_.tCCD_L : cfg_.tCCD_S);
        earliest = std::max(earliest, last_col_start_cycle_ + min_gap);
    }
    if (bus_used_) {
        // The data bus -- not the command bus -- is the real shared
        // resource here, and this command's own data doesn't start until
        // cas_cycles after its column command. So the floor this imposes on
        // col_start is bus_free_cycle_ backed off by this command's own CAS
        // latency, not bus_free_cycle_ itself: with tCL/tCWL pipelining,
        // two same-direction commands spaced by tCCD already produce
        // non-overlapping data windows (their CAS latencies are identical
        // and simply shift both windows by the same amount), so imposing
        // bus_free_cycle_ directly on col_start here would double-count the
        // latency as if it were a throughput cost (see FIX 2's trap).
        uint64_t min_col_start_for_bus = (bus_free_cycle_ > cas_cycles) ? bus_free_cycle_ - cas_cycles : 0;
        earliest = std::max(earliest, min_col_start_for_bus);
        if (last_bus_type_ != cmd.type) {
            uint64_t turn = cyc(last_bus_type_ == TxnType::Read ? cfg_.rd_wr_turnaround : cfg_.wr_rd_turnaround);
            uint64_t with_turn = bus_free_cycle_ + turn;
            uint64_t min_col_start_for_turn = (with_turn > cas_cycles) ? with_turn - cas_cycles : 0;
            if (min_col_start_for_turn > earliest) {
                stats_.turnaround_cycles += (min_col_start_for_turn - earliest);
                earliest = min_col_start_for_turn;
            }
        }
    }

    // tWTR (write-to-read, same rank): an additive DRAM-internal constraint
    // on top of the bus-direction turnaround above -- both apply. Measured
    // from the end of the write's data burst (its complete_cycle), not its
    // col_start, since that's genuinely when the write's data has cleared
    // the array/bus internally.
    if (cmd.type == TxnType::Read && had_write_cmd_) {
        bool same_bg = (last_write_bankgroup_ == cmd.addr.bankgroup);
        uint64_t twtr = cyc(same_bg ? cfg_.tWTR_L : cfg_.tWTR_S);
        earliest = std::max(earliest, last_write_complete_cycle_ + twtr);
    }

    earliest = apply_refresh_if_due(rank_idx, earliest);

    RowStatus status;
    uint64_t col_start;
    if (!bank.row_open) {
        status = RowStatus::Empty;
        // The bank is already precharged (that's what row_open==false means)
        // -- what remains to check is that whatever precharge-adjacent
        // recovery was still outstanding from its last occupant (tRTP/tWR/
        // tRAS, folded into precharge_ready_cycle below) has actually
        // elapsed. There is no column-side constraint to check here: this
        // bank has no open row for a stray column command to race against.
        uint64_t act_start = std::max(earliest, bank.precharge_ready_cycle);
        act_start = apply_activate_gating(rank, cmd.addr.bankgroup, act_start);
        col_start = act_start + cyc(cfg_.tRCD);
        bank.row_open = true;
        bank.open_row = cmd.addr.row;
        bank.row_opened_at_cycle = act_start;
    } else if (bank.open_row == cmd.addr.row) {
        status = RowStatus::Hit;
        // Next column command to an already-open row is governed purely by
        // tCCD spacing (already folded into `earliest` above via
        // last_col_start_cycle_) plus this bank's own pipeline being free
        // for another column op -- NOT by tRTP/tWR, which are
        // precharge-only constraints and must not leak in here (see FIX 1).
        col_start = std::max(earliest, bank.col_ready_cycle);
    } else {
        status = RowStatus::Conflict;
        // precharge_ready_cycle already folds in both the tRTP/tWR recovery
        // from the last column command against this row and tRAS-since-
        // activate (see the bottom of this function) -- tRAS need not be
        // re-derived here.
        uint64_t precharge_ready = std::max(earliest, bank.precharge_ready_cycle);
        uint64_t act_start = precharge_ready + cyc(cfg_.tRP);
        act_start = apply_activate_gating(rank, cmd.addr.bankgroup, act_start);
        col_start = act_start + cyc(cfg_.tRCD);
        bank.row_open = true;
        bank.open_row = cmd.addr.row;
        bank.row_opened_at_cycle = act_start;
    }

    uint32_t bus_bytes = static_cast<uint32_t>(std::max(1, cfg_.data_bus_bytes));
    uint64_t transfer_cycles = (cmd.bytes + bus_bytes - 1) / bus_bytes;
    if (transfer_cycles == 0) transfer_cycles = 1;

    // complete/data_start correctly include CAS latency -- it delays when
    // data actually appears on the bus, which is a genuine data-bus
    // quantity (bus_free_cycle_, stats_.busy_cycles, turnaround all
    // legitimately follow it). col_start itself, and everything derived
    // from col_start below (tCCD's base, tRTP/tWR's precharge base), must
    // NOT follow it -- tCL/tCWL is pipeline latency, not a throughput limit.
    uint64_t data_start = col_start + cas_cycles;
    uint64_t complete = data_start + transfer_cycles;

    // col_ready_cycle: this bank's own column pipeline is occupied for the
    // duration of the transfer -- measured from col_start, not data_start,
    // same as tCCD (see FIX 2's note on CAS latency being pipeline-only).
    bank.col_ready_cycle = col_start + transfer_cycles;

    // precharge_ready_cycle: tRTP (read) / tWR (write) measured from this
    // column command, maxed with tRAS since the activate that opened the
    // currently-open row -- both are genuine PRECHARGE-only constraints
    // (FIX 1), never column-to-column spacing.
    uint64_t recovery_ns = (cmd.type == TxnType::Read) ? cfg_.tRTP : cfg_.tWR;
    bank.precharge_ready_cycle = std::max(col_start + cyc(recovery_ns), bank.row_opened_at_cycle + cyc(cfg_.tRAS));

    bus_free_cycle_ = complete;
    last_bus_type_ = cmd.type;
    bus_used_ = true;
    stats_.busy_cycles += transfer_cycles;

    last_col_start_cycle_ = col_start;
    last_col_bankgroup_ = cmd.addr.bankgroup;
    had_col_cmd_ = true;

    if (cmd.type == TxnType::Write) {
        last_write_complete_cycle_ = complete;
        last_write_bankgroup_ = cmd.addr.bankgroup;
        had_write_cmd_ = true;
    }

    switch (status) {
        case RowStatus::Hit: stats_.hits++; break;
        case RowStatus::Conflict: stats_.conflicts++; break;
        case RowStatus::Empty: stats_.empties++; break;
    }

    cmd.arrival_cycle = arrival_cycle;
    cmd.start_cycle = col_start;
    cmd.complete_cycle = complete;
    cmd.row_status = status;
    return cmd;
}

} // namespace ddrtiming
