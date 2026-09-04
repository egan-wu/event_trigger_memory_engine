// White-box tests against ChannelScheduler directly (not through Engine),
// covering the bank-group-aware timing paths that every existing test
// sidesteps by using a single bank: tCCD_S/L, tRRD_S/L, the tFAW 4-activate
// rolling window, refresh insertion/periodicity, and R/W bus turnaround.
// Every expected number here is hand-computed and traced in comments so a
// failure points at exactly which mechanism broke.
#include "testing.hpp"
#include "core/command_queue.hpp"
#include "core/config.hpp"
#include "core/types.hpp"

using namespace ddrtiming;

namespace {
DdrcConfig base_config() {
    DdrcConfig cfg;
    cfg.channels = 1;
    cfg.ranks_per_channel = 1;
    cfg.bankgroups = 2;
    cfg.banks_per_group = 2;
    cfg.rows = 1 << 20;
    cfg.data_bus_bytes = 8;
    cfg.clock_mhz = 1000.0; // 1 ns/cycle, exact
    cfg.command_queue_depth = 1000;
    cfg.max_outstanding_per_id = 1000;
    return cfg;
}

DramCommand make_cmd(TxnType type, uint32_t rank, uint32_t bg, uint32_t bank, uint32_t row, uint32_t bytes = 8) {
    DramCommand c;
    c.type = type;
    c.addr.rank = rank;
    c.addr.bankgroup = bg;
    c.addr.bank = bank;
    c.addr.row = row;
    c.bytes = bytes;
    return c;
}
} // namespace

DDRTEST(tccd_l_vs_tccd_s_column_spacing) {
    // tCCD_L (same bank group) must be strictly larger-gap-enforcing than
    // tCCD_S (different bank group) when it's the binding constraint.
    DdrcConfig cfg = base_config();
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 2; cfg.tCCD_L = 4;
    cfg.tRRD_S = 2; cfg.tRRD_L = 6; // smaller than what tCCD will demand below
    cfg.tFAW = 1000; // won't bind
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // P: (rank0, bg0, bank0), fresh -> Empty. act=0, start=0+tRCD=5, complete=6.
    auto p = make_cmd(TxnType::Read, 0, 0, 0, 0);
    sched.schedule(p, 0);
    DDR_CHECK_EQ(p.start_cycle, 5ull);
    DDR_CHECK(p.row_status == RowStatus::Empty);

    // Q: (rank0, bg0, bank1) -- same bank group as P, different bank.
    // tCCD_L binds: act = last_col_start(5) + tCCD_L(4) = 9 (beats tRRD_L's
    // own term of 0+6=6 and bus_free's 6). start = 9 + tRCD(5) = 14.
    auto q = make_cmd(TxnType::Read, 0, 0, 1, 0);
    sched.schedule(q, 0);
    DDR_CHECK_EQ(q.start_cycle, 14ull);
    DDR_CHECK(q.row_status == RowStatus::Empty);

    // R: (rank0, bg1, bank0) -- different bank group from Q's last column cmd.
    // tCCD_S binds: act = last_col_start(14) + tCCD_S(2) = 16 (beats
    // tRRD_S's own term of 9+2=11 and bus_free's 15). start = 16+5 = 21.
    auto r = make_cmd(TxnType::Read, 0, 1, 0, 0);
    sched.schedule(r, 0);
    DDR_CHECK_EQ(r.start_cycle, 21ull);
    DDR_CHECK(r.row_status == RowStatus::Empty);
}

DDRTEST(trrd_l_vs_trrd_s_activate_spacing) {
    // Isolate tRRD by making tCCD negligible (0) so it never wins the max().
    DdrcConfig cfg = base_config();
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 0; cfg.tCCD_L = 0;
    cfg.tRRD_S = 10; cfg.tRRD_L = 15;
    cfg.tFAW = 1000; // won't bind
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // P: (rank0, bg0, bank0) fresh. act=0, start=5, complete=6.
    auto p = make_cmd(TxnType::Read, 0, 0, 0, 0);
    sched.schedule(p, 0);
    DDR_CHECK_EQ(p.start_cycle, 5ull);

    // R: (rank0, bg1, bank0) -- different bank group from P's last activate.
    // tRRD_S binds: act = P's act(0) + tRRD_S(10) = 10 (beats bus_free's 6
    // and tCCD's 5). start = 10 + 5 = 15.
    auto r = make_cmd(TxnType::Read, 0, 1, 0, 0);
    sched.schedule(r, 0);
    DDR_CHECK_EQ(r.start_cycle, 15ull);

    // S: (rank0, bg1, bank1) -- SAME bank group as R's last activate.
    // tRRD_L binds: act = R's act(10) + tRRD_L(15) = 25 (beats bus_free's 16
    // and tCCD's 15). start = 25 + 5 = 30.
    auto s = make_cmd(TxnType::Read, 0, 1, 1, 0);
    sched.schedule(s, 0);
    DDR_CHECK_EQ(s.start_cycle, 30ull);
}

DDRTEST(tfaw_limits_to_four_activates_per_rolling_window) {
    // Five back-to-back Empty (conflict-free, always-fresh-row) accesses to
    // five different banks in the same rank. Natural per-activate spacing
    // (via tRCD+transfer, tRRD/tCCD negligible here) is ~6 cycles, so four
    // activates land at 0, 6, 12, 18 -- within tFAW(40) of each other. The
    // fifth must wait until activate #1 + tFAW = 40, not the natural 24.
    DdrcConfig cfg = base_config();
    cfg.banks_per_group = 5;
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 0; cfg.tCCD_L = 0;
    cfg.tRRD_S = 1; cfg.tRRD_L = 1; // small, must not be the binding constraint
    cfg.tFAW = 40;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    uint64_t expected_natural[4] = {0, 6, 12, 18};
    for (int i = 0; i < 4; ++i) {
        auto c = make_cmd(TxnType::Read, 0, 0, static_cast<uint32_t>(i), 0);
        sched.schedule(c, 0);
        DDR_CHECK_EQ(c.start_cycle, expected_natural[i] + 5); // act + tRCD
    }

    // 5th activate: natural would be 24 (18+6), but tFAW forces it to
    // activate#1(0) + tFAW(40) = 40. start = 40 + tRCD(5) = 45.
    auto c5 = make_cmd(TxnType::Read, 0, 0, 4, 0);
    sched.schedule(c5, 0);
    DDR_CHECK_EQ(c5.start_cycle, 45ull);
}

DDRTEST(refresh_inserted_periodically_and_blocks_for_trfc) {
    // Single bank kept open (first access opens it, rest are hits), tRCD/tRP
    // /tRTP/tWR/tCCD/tRRD/tFAW all zeroed so the only thing perturbing the
    // otherwise-1-cycle-per-command cadence is refresh. tREFI=20, tRFC=8.
    DdrcConfig cfg = base_config();
    cfg.tRCD = 0; cfg.tRP = 0; cfg.tRAS = 0; cfg.tRC = 0;
    cfg.tCCD_S = 0; cfg.tCCD_L = 0; cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 0;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 20; cfg.tRFC = 8;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // Command k (0-indexed) naturally lands at start=complete=k+1 with no
    // refresh. The first refresh boundary is at cycle 20: command index 20's
    // earliest-before-refresh is 20 (>= next_refresh_due=20), pushing it to
    // 20+tRFC(8)=28.
    DramCommand last;
    for (int k = 0; k <= 20; ++k) {
        auto c = make_cmd(TxnType::Read, 0, 0, 0, 0);
        sched.schedule(c, 0);
        last = c;
    }
    DDR_CHECK_EQ(last.start_cycle, 28ull);
    DDR_CHECK_EQ(sched.stats().refresh_cycles, 8ull);

    // Continuing at +1/command from complete=29, the next refresh boundary
    // (next_refresh_due=40) is crossed by command index 32 (see comment
    // derivation in the design discussion: complete(cmd_(20+j))=29+j, so
    // earliest for cmd_(20+12)=cmd32 is 29+11=40).
    for (int k = 21; k <= 32; ++k) {
        auto c = make_cmd(TxnType::Read, 0, 0, 0, 0);
        sched.schedule(c, 0);
        last = c;
    }
    DDR_CHECK_EQ(last.start_cycle, 48ull); // 40 + tRFC(8)
    DDR_CHECK_EQ(sched.stats().refresh_cycles, 16ull); // two refreshes, 8 each
}

DDRTEST(rw_turnaround_only_applied_on_direction_change) {
    DdrcConfig cfg = base_config();
    cfg.tRCD = 0; cfg.tRP = 0; cfg.tRAS = 0; cfg.tRC = 0;
    cfg.tCCD_S = 0; cfg.tCCD_L = 0; cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 0;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 3;
    cfg.wr_rd_turnaround = 5;
    ChannelScheduler sched(cfg, 0);

    // cmd0: Read, opens the row. start=0, complete=1.
    auto c0 = make_cmd(TxnType::Read, 0, 0, 0, 0);
    sched.schedule(c0, 0);
    DDR_CHECK_EQ(c0.start_cycle, 0ull);

    // cmd1: Write, same row (hit) -- direction change R->W: start =
    // bus_free(1) + rd_wr_turnaround(3) = 4.
    auto c1 = make_cmd(TxnType::Write, 0, 0, 0, 0);
    sched.schedule(c1, 0);
    DDR_CHECK_EQ(c1.start_cycle, 4ull);

    // cmd2: Read, same row -- direction change W->R: start =
    // bus_free(5) + wr_rd_turnaround(5) = 10.
    auto c2 = make_cmd(TxnType::Read, 0, 0, 0, 0);
    sched.schedule(c2, 0);
    DDR_CHECK_EQ(c2.start_cycle, 10ull);

    // cmd3: Read, same row -- SAME direction as cmd2, no turnaround: start =
    // bus_free(11) exactly.
    auto c3 = make_cmd(TxnType::Read, 0, 0, 0, 0);
    sched.schedule(c3, 0);
    DDR_CHECK_EQ(c3.start_cycle, 11ull);

    DDR_CHECK_EQ(sched.stats().turnaround_cycles, 8ull); // 3 + 5, cmd3 added none
}
