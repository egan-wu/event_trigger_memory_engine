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

uint64_t ChannelScheduler::queue_admit_cycle(uint64_t arrival_cycle) {
    uint64_t admit = arrival_cycle;
    while (!inflight_completions_.empty() && *inflight_completions_.begin() <= admit) {
        inflight_completions_.erase(inflight_completions_.begin());
    }
    while (static_cast<int>(inflight_completions_.size()) >= cfg_.command_queue_depth) {
        admit = std::max(admit, *inflight_completions_.begin());
        inflight_completions_.erase(inflight_completions_.begin());
        while (!inflight_completions_.empty() && *inflight_completions_.begin() <= admit) {
            inflight_completions_.erase(inflight_completions_.begin());
        }
    }
    return admit;
}

uint64_t ChannelScheduler::apply_refresh_if_due(RankState& rk, uint64_t earliest_cycle) {
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

uint64_t ChannelScheduler::schedule(DramCommand& cmd, uint64_t arrival_cycle) {
    uint64_t admit = queue_admit_cycle(arrival_cycle);

    uint32_t rank_idx = cmd.addr.rank % static_cast<uint32_t>(ranks_.size());
    uint32_t bg_idx = cmd.addr.bankgroup % static_cast<uint32_t>(banks_[rank_idx].size());
    uint32_t bank_idx = cmd.addr.bank % static_cast<uint32_t>(banks_[rank_idx][bg_idx].size());
    BankState& bank = banks_[rank_idx][bg_idx][bank_idx];
    RankState& rank = ranks_[rank_idx];

    uint64_t earliest = std::max(admit, bank.bank_ready_cycle);

    // Data-bus / tCCD spacing against the previous column command on this channel.
    if (had_col_cmd_) {
        bool same_bg = (last_col_bankgroup_ == cmd.addr.bankgroup);
        uint64_t min_gap = cyc(same_bg ? cfg_.tCCD_L : cfg_.tCCD_S);
        earliest = std::max(earliest, last_col_start_cycle_ + min_gap);
    }
    if (bus_used_) {
        earliest = std::max(earliest, bus_free_cycle_);
        if (last_bus_type_ != cmd.type) {
            uint64_t turn = cyc(last_bus_type_ == TxnType::Read ? cfg_.rd_wr_turnaround : cfg_.wr_rd_turnaround);
            uint64_t with_turn = bus_free_cycle_ + turn;
            if (with_turn > earliest) {
                stats_.turnaround_cycles += (with_turn - earliest);
                earliest = with_turn;
            }
        }
    }

    earliest = apply_refresh_if_due(rank, earliest);

    RowStatus status;
    uint64_t col_start;
    if (!bank.row_open) {
        status = RowStatus::Empty;
        uint64_t act_start = std::max(earliest, bank.bank_ready_cycle);
        act_start = apply_activate_gating(rank, cmd.addr.bankgroup, act_start);
        col_start = act_start + cyc(cfg_.tRCD);
        bank.row_open = true;
        bank.open_row = cmd.addr.row;
        bank.row_opened_at_cycle = act_start;
    } else if (bank.open_row == cmd.addr.row) {
        status = RowStatus::Hit;
        col_start = std::max(earliest, bank.bank_ready_cycle);
    } else {
        status = RowStatus::Conflict;
        uint64_t precharge_ready = std::max({earliest, bank.row_opened_at_cycle + cyc(cfg_.tRAS), bank.bank_ready_cycle});
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

    uint64_t complete = col_start + transfer_cycles;

    // Bank recovery: earliest this bank can start another command after this one.
    uint64_t recovery_ns = (cmd.type == TxnType::Read) ? cfg_.tRTP : cfg_.tWR;
    bank.bank_ready_cycle = std::max(complete, col_start + cyc(recovery_ns));

    bus_free_cycle_ = complete;
    last_bus_type_ = cmd.type;
    bus_used_ = true;
    stats_.busy_cycles += transfer_cycles;

    last_col_start_cycle_ = col_start;
    last_col_bankgroup_ = cmd.addr.bankgroup;
    had_col_cmd_ = true;

    inflight_completions_.insert(complete);

    switch (status) {
        case RowStatus::Hit: stats_.hits++; break;
        case RowStatus::Conflict: stats_.conflicts++; break;
        case RowStatus::Empty: stats_.empties++; break;
    }

    cmd.arrival_cycle = arrival_cycle;
    cmd.start_cycle = col_start;
    cmd.complete_cycle = complete;
    cmd.row_status = status;
    return complete;
}

} // namespace ddrtiming
