// Bus-time attribution budget: every cycle of a channel's timeline is
// charged to exactly one cause, so data + the seven bubble categories must
// add up -- to the channel's own last data burst at the ChannelScheduler
// level, and to 100% of channels*total_cycles once the engine adds each
// channel's trailing idle. A budget that does not close is the failure mode
// this file exists to catch: it would mean a real bubble is being attributed
// twice, or to nothing at all.
#include "testing.hpp"
#include "core/command_queue.hpp"
#include "core/config.hpp"
#include "core/engine.hpp"
#include "core/types.hpp"

#include <cmath>

using namespace ddrtiming;

namespace {

// Single bank, 1 ns/cycle, every recovery constraint zeroed except the row
// cycle itself -- so the only thing that can put a bubble on the bus is a
// row miss, and its size is exactly hand-computable.
DdrcConfig single_bank_config() {
    DdrcConfig cfg;
    cfg.channels = 1;
    cfg.ranks_per_channel = 1;
    cfg.bankgroups = 1;
    cfg.banks_per_group = 1;
    cfg.rows = 1 << 16;
    cfg.data_bus_bytes = 8;
    cfg.clock_mhz = 1000.0; // 1 ns/cycle, exact
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 0; cfg.tRC = 5;
    cfg.tCL = 0; cfg.tCWL = 0;
    cfg.tCCD_S = 1; cfg.tCCD_L = 1;
    cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 0;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    cfg.command_queue_depth = 1000;
    cfg.max_outstanding_per_id = 1000;
    return cfg;
}

DramCommand read_cmd(uint32_t row) {
    DramCommand c;
    c.type = TxnType::Read;
    c.addr.row = row;
    c.bytes = 8; // one beat on an 8-byte bus -> transfer_cycles == 1
    return c;
}

uint64_t attributed_total(const ChannelStats& s) {
    return s.busy_cycles + s.attr_refresh_cycles + s.attr_row_miss_cycles + s.attr_twtr_cycles +
           s.attr_turnaround_cycles + s.attr_tccd_l_excess_cycles + s.attr_frontend_idle_cycles +
           s.attr_other_cycles;
}

} // namespace

DDRTEST(attribution_charges_a_row_miss_bubble_exactly) {
    DdrcConfig cfg = single_bank_config();
    ChannelScheduler sched(cfg, 0);

    // Command 1 (row 0, bank never opened -> Empty). ACT takes command-bus
    // slot [0,2); column at ACT + tRCD = 5, data [5,6). The bus was idle
    // [0,5) waiting for that row to open -> 5 cycles of exposed row miss.
    sched.try_admit(read_cmd(0), 0);
    DramCommand a = sched.drain_one();
    DDR_CHECK_EQ(a.start_cycle, 5ull);
    DDR_CHECK_EQ(sched.stats().attr_row_miss_cycles, 5ull);
    DDR_CHECK_EQ(sched.stats().busy_cycles, 1ull);
    DDR_CHECK_EQ(attributed_total(sched.stats()), sched.stats().last_data_end_cycle);

    // Command 2 (row 1, same bank -> Conflict). PRE's own floor is bank
    // recovery (col_start 5 + tRTP 0 = 5), but the command bus is busy with
    // command 1's column slot [5,7), so PRE lands at 7; ACT at 7 + tRP(5)
    // = 12; column at 12 + tRCD(5) = 17, data [17,18). Bus was idle
    // [6,17) = 11 more cycles, all of it this row miss.
    sched.try_admit(read_cmd(1), 0);
    DramCommand b = sched.drain_one();
    DDR_CHECK_EQ(b.start_cycle, 17ull);
    DDR_CHECK(b.row_status == RowStatus::Conflict);
    DDR_CHECK_EQ(sched.stats().attr_row_miss_cycles, 16ull); // 5 + 11
    DDR_CHECK_EQ(sched.stats().busy_cycles, 2ull);

    // The budget closes exactly: 16 + 2 == 18 == end of the last burst.
    DDR_CHECK_EQ(sched.stats().last_data_end_cycle, 18ull);
    DDR_CHECK_EQ(attributed_total(sched.stats()), 18ull);
    DDR_CHECK_EQ(sched.stats().attr_refresh_cycles, 0ull);
    DDR_CHECK_EQ(sched.stats().attr_turnaround_cycles, 0ull);
    DDR_CHECK_EQ(sched.stats().attr_frontend_idle_cycles, 0ull);
}

DDRTEST(attribution_charges_waiting_for_a_late_arrival_to_the_front_end) {
    DdrcConfig cfg = single_bank_config();
    ChannelScheduler sched(cfg, 0);

    sched.try_admit(read_cmd(0), 0);
    (void)sched.drain_one(); // data [5,6), 5 cycles of row miss

    // A page hit that does not arrive until cycle 50: the bus sits idle
    // [6,50) waiting for the front end, then transfers [50,51).
    sched.try_admit(read_cmd(0), 50);
    DramCommand b = sched.drain_one();
    DDR_CHECK_EQ(b.start_cycle, 50ull);
    DDR_CHECK(b.row_status == RowStatus::Hit);
    DDR_CHECK_EQ(sched.stats().attr_frontend_idle_cycles, 44ull); // 50 - 6
    DDR_CHECK_EQ(sched.stats().attr_row_miss_cycles, 5ull);
    DDR_CHECK_EQ(attributed_total(sched.stats()), 51ull);
    DDR_CHECK_EQ(sched.stats().last_data_end_cycle, 51ull);
}

DDRTEST(attribution_budget_closes_to_100_percent_on_a_mixed_workload) {
    // Multi-channel, multi-bank, read/write mix, long enough for refresh to
    // fire -- so several categories are non-zero at once -- then assert the
    // engine-level percentages still partition the run exactly.
    DdrcConfig cfg;
    cfg.channels = 2;
    cfg.ranks_per_channel = 1;
    cfg.bankgroups = 2;
    cfg.banks_per_group = 2;
    cfg.rows = 1 << 16;
    cfg.columns = 1024;
    cfg.data_bus_bytes = 8;
    cfg.burst_beats = 8;
    cfg.clock_mhz = 1600.0;
    cfg.map_bankgroup = AddressField::contiguous(6, 1);
    cfg.map_bank = AddressField::contiguous(7, 1);
    cfg.map_channel = AddressField::contiguous(8, 1);
    cfg.map_row = AddressField::contiguous(15, 16);
    cfg.command_queue_depth = 16;
    cfg.max_outstanding_per_id = 8;

    Engine engine(cfg);
    for (int i = 0; i < 400; ++i) {
        AxiTxn t;
        t.core_id = i % 2;
        t.type = (i % 5 == 0) ? TxnType::Write : TxnType::Read; // forces turnaround + tWTR
        t.axi_id = static_cast<uint32_t>(i % 3);
        // Mostly sequential, with a large jump every 16th transaction so
        // rows actually close and reopen.
        t.addr = static_cast<uint64_t>(i) * 64 + static_cast<uint64_t>(i / 16) * 0x40000;
        t.size_bytes = 64;
        t.len_beats = 1;
        engine.push_txn(t);
    }
    engine.run();

    const SummaryStats& s = engine.summary();
    double sum = s.attr_data_pct + s.attr_row_miss_exposed_pct + s.attr_refresh_pct +
                 s.attr_turnaround_pct + s.attr_twtr_pct + s.attr_tccd_l_excess_pct +
                 s.attr_frontend_idle_pct + s.attr_other_pct;
    DDR_CHECK(std::fabs(sum - 100.0) < 0.01);

    // Sanity: the run actually moved data and actually spent time on more
    // than one cause, so the closure above is not trivially 100% = data.
    DDR_CHECK(s.attr_data_pct > 0.0);
    DDR_CHECK(s.attr_data_pct < 100.0);
    // Bandwidth utilization is the same quantity as the data share, since
    // both are physical bytes over the same channel-time budget.
    DDR_CHECK(std::fabs(s.attr_data_pct - s.bandwidth_utilization_pct) < 0.5);
}
