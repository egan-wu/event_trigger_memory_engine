// White-box tests against ChannelScheduler directly (not through Engine),
// covering the bank-group-aware timing paths that every existing test
// sidesteps by using a single bank: tCCD_S/L, tRRD_S/L, the tFAW 4-activate
// rolling window, refresh insertion/periodicity, and R/W bus turnaround.
// Every expected number here is hand-computed and traced in comments so a
// failure points at exactly which mechanism broke.
//
// Each command below is admitted alone (queue otherwise empty) and drained
// immediately, so FR-FCFS has only one candidate to pick -- these tests
// exercise the underlying per-command timing math, not the queue's
// reordering behavior (that's covered separately).
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

DramCommand admit_and_drain(ChannelScheduler& sched, const DramCommand& cmd, uint64_t ready_cycle) {
    sched.try_admit(cmd, ready_cycle);
    return sched.drain_one();
}
} // namespace

DDRTEST(tccd_l_vs_tccd_s_column_spacing) {
    // tCCD_L (same bank group) must be strictly larger-gap-enforcing than
    // tCCD_S (different bank group) when it's the binding constraint.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0; // isolate this test from FIX 2's CAS latency
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 2; cfg.tCCD_L = 4;
    cfg.tRRD_S = 2; cfg.tRRD_L = 6; // smaller than what tCCD will demand below
    cfg.tFAW = 1000; // won't bind
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // P: (rank0, bg0, bank0), fresh -> Empty. act=0, start=0+tRCD=5, complete=6.
    auto p = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(p.start_cycle, 5ull);
    DDR_CHECK(p.row_status == RowStatus::Empty);

    // Q: (rank0, bg0, bank1) -- same bank group as P, different bank.
    // tCCD_L binds: act = last_col_start(5) + tCCD_L(4) = 9 (beats tRRD_L's
    // own term of 0+6=6 and bus_free's 6). start = 9 + tRCD(5) = 14.
    auto q = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 1, 0), 0);
    DDR_CHECK_EQ(q.start_cycle, 14ull);
    DDR_CHECK(q.row_status == RowStatus::Empty);

    // R: (rank0, bg1, bank0) -- different bank group from Q's last column cmd.
    // tCCD_S binds: act = last_col_start(14) + tCCD_S(2) = 16 (beats
    // tRRD_S's own term of 9+2=11 and bus_free's 15). start = 16+5 = 21.
    auto r = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 1, 0, 0), 0);
    DDR_CHECK_EQ(r.start_cycle, 21ull);
    DDR_CHECK(r.row_status == RowStatus::Empty);
}

DDRTEST(trrd_l_vs_trrd_s_activate_spacing) {
    // Isolate tRRD by making tCCD negligible (0) so it never wins the max().
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0; // isolate this test from FIX 2's CAS latency
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 0; cfg.tCCD_L = 0;
    cfg.tRRD_S = 10; cfg.tRRD_L = 15;
    cfg.tFAW = 1000; // won't bind
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // P: (rank0, bg0, bank0) fresh. act=0, start=5, complete=6.
    auto p = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(p.start_cycle, 5ull);

    // R: (rank0, bg1, bank0) -- different bank group from P's last activate.
    // tRRD_S binds: act = P's act(0) + tRRD_S(10) = 10 (beats bus_free's 6
    // and tCCD's 5). start = 10 + 5 = 15.
    auto r = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 1, 0, 0), 0);
    DDR_CHECK_EQ(r.start_cycle, 15ull);

    // S: (rank0, bg1, bank1) -- SAME bank group as R's last activate.
    // tRRD_L binds: act = R's act(10) + tRRD_L(15) = 25 (beats bus_free's 16
    // and tCCD's 15). start = 25 + 5 = 30.
    auto s = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 1, 1, 0), 0);
    DDR_CHECK_EQ(s.start_cycle, 30ull);
}

DDRTEST(tfaw_limits_to_four_activates_per_rolling_window) {
    // Five back-to-back Empty (conflict-free, always-fresh-row) accesses to
    // five different banks in the same rank. Natural per-activate spacing
    // (via tRCD+transfer, tRRD/tCCD negligible here) is ~6 cycles, so four
    // activates land at 0, 6, 12, 18 -- within tFAW(40) of each other. The
    // fifth must wait until activate #1 + tFAW = 40, not the natural 24.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0; // isolate this test from FIX 2's CAS latency
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
        auto c = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, static_cast<uint32_t>(i), 0), 0);
        DDR_CHECK_EQ(c.start_cycle, expected_natural[i] + 5); // act + tRCD
    }

    // 5th activate: natural would be 24 (18+6), but tFAW forces it to
    // activate#1(0) + tFAW(40) = 40. start = 40 + tRCD(5) = 45.
    auto c5 = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 4, 0), 0);
    DDR_CHECK_EQ(c5.start_cycle, 45ull);
}

DDRTEST(refresh_inserted_periodically_and_blocks_for_trfc) {
    // Single bank kept open (first access opens it, rest are hits), tRCD/tRP
    // /tRTP/tWR/tCCD/tRRD/tFAW all zeroed so the only thing perturbing the
    // otherwise-1-cycle-per-command cadence is refresh. tREFI=20, tRFC=8.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0; // isolate this test from FIX 2's CAS latency
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
        last = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    }
    DDR_CHECK_EQ(last.start_cycle, 28ull);
    DDR_CHECK_EQ(sched.stats().refresh_cycles, 8ull);

    // Continuing at +1/command from complete=29, the next refresh boundary
    // (next_refresh_due=40) is crossed by command index 32 (see comment
    // derivation in the design discussion: complete(cmd_(20+j))=29+j, so
    // earliest for cmd_(20+12)=cmd32 is 29+11=40).
    for (int k = 21; k <= 32; ++k) {
        last = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    }
    DDR_CHECK_EQ(last.start_cycle, 48ull); // 40 + tRFC(8)
    DDR_CHECK_EQ(sched.stats().refresh_cycles, 16ull); // two refreshes, 8 each
}

DDRTEST(rw_turnaround_only_applied_on_direction_change) {
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0; // isolate this test from FIX 2's CAS latency
    cfg.tRCD = 0; cfg.tRP = 0; cfg.tRAS = 0; cfg.tRC = 0;
    cfg.tCCD_S = 0; cfg.tCCD_L = 0; cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 0;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 3;
    cfg.wr_rd_turnaround = 5;
    ChannelScheduler sched(cfg, 0);

    // cmd0: Read, opens the row. start=0, complete=1.
    auto c0 = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(c0.start_cycle, 0ull);

    // cmd1: Write, same row (hit) -- direction change R->W: start =
    // bus_free(1) + rd_wr_turnaround(3) = 4.
    auto c1 = admit_and_drain(sched, make_cmd(TxnType::Write, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(c1.start_cycle, 4ull);

    // cmd2: Read, same row -- direction change W->R: start =
    // bus_free(5) + wr_rd_turnaround(5) = 10.
    auto c2 = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(c2.start_cycle, 10ull);

    // cmd3: Read, same row -- SAME direction as cmd2, no turnaround: start =
    // bus_free(11) exactly.
    auto c3 = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(c3.start_cycle, 11ull);

    DDR_CHECK_EQ(sched.stats().turnaround_cycles, 8ull); // 3 + 5, cmd3 added none
}

DDRTEST(fr_fcfs_prefers_a_ready_hit_over_an_older_conflict) {
    // The whole point of this rewrite: given two candidates sitting in the
    // queue together, an older command that would need a fresh
    // activate must NOT block a newer command that's a page-hit against an
    // already-open row -- the newer one should be serviced first.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0; // isolate this test from FIX 2's CAS latency
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 1; cfg.tCCD_L = 1; cfg.tRRD_S = 1; cfg.tRRD_L = 1; cfg.tFAW = 0;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // Open bank0's row 0 first (Empty), so it's the "already-open" row.
    admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);

    // Admit an OLDER command that targets bank0 but a DIFFERENT row (would be
    // a Conflict -- expensive: precharge+activate), then a NEWER command
    // that targets bank0's SAME open row (a Hit -- cheap), both before
    // draining either.
    DramCommand older_conflict = make_cmd(TxnType::Read, 0, 0, 0, /*row=*/1);
    DramCommand newer_hit = make_cmd(TxnType::Read, 0, 0, 0, /*row=*/0);
    sched.try_admit(older_conflict, 10);
    sched.try_admit(newer_hit, 20);

    // FR-FCFS must pick the hit first, even though it arrived later.
    DramCommand first = sched.drain_one();
    DDR_CHECK(first.row_status == RowStatus::Hit);
    DDR_CHECK_EQ(first.addr.row, 0u);

    DramCommand second = sched.drain_one();
    DDR_CHECK(second.row_status == RowStatus::Conflict);
    DDR_CHECK_EQ(second.addr.row, 1u);
}

DDRTEST(column_spacing_on_page_hits_is_tccd_not_trtp) {
    // tRTP/tWR are read/write-to-PRECHARGE constraints, not next-column
    // constraints -- a page hit's next column command is spaced by
    // tCCD_S/tCCD_L alone. tRTP(12)/tWR(20) are deliberately set much larger
    // than tCCD_L(4) here so a leftover coupling to them would be obvious.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0; // isolate this test from FIX 2's CAS latency
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 2; cfg.tCCD_L = 4;
    cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 1000; // won't bind
    cfg.tRTP = 12; cfg.tWR = 20;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // A: fresh bank -> Empty. act=0, start=0+tRCD(5)=5, complete=6.
    auto a = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(a.start_cycle, 5ull);
    DDR_CHECK(a.row_status == RowStatus::Empty);

    // B: same bank, same row -> Hit. tCCD_L binds: start = 5 + 4 = 9.
    auto b = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(b.start_cycle, 9ull);
    DDR_CHECK(b.row_status == RowStatus::Hit);

    // C: same -> Hit. start = 9 + 4 = 13.
    auto c = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(c.start_cycle, 13ull);
    DDR_CHECK(c.row_status == RowStatus::Hit);
}

DDRTEST(write_page_hits_are_not_gated_by_twr) {
    // Same as above but all WRITEs, to confirm tWR (write-recovery-before-
    // PRECHARGE) doesn't leak into next-column spacing either.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0; // isolate this test from FIX 2's CAS latency
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 2; cfg.tCCD_L = 4;
    cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 1000;
    cfg.tRTP = 12; cfg.tWR = 20;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    auto a = admit_and_drain(sched, make_cmd(TxnType::Write, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(a.start_cycle, 5ull);
    auto b = admit_and_drain(sched, make_cmd(TxnType::Write, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(b.start_cycle, 9ull);
    auto c = admit_and_drain(sched, make_cmd(TxnType::Write, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(c.start_cycle, 13ull);
}

DDRTEST(cas_latency_delays_data_but_not_command_spacing) {
    // tCL/tCWL are pipeline latency from column command to data, not a
    // throughput limit: they must delay complete_cycle but never leak into
    // command-issue spacing (tCCD), which stays anchored to col_start.
    DdrcConfig cfg = base_config();
    cfg.tRCD = 5; cfg.tCL = 10; cfg.tCWL = 8; cfg.tCCD_L = 4; cfg.tCCD_S = 2;
    cfg.tRP = 0; cfg.tRAS = 0; cfg.tRC = 0; cfg.tRRD_S = 0; cfg.tRRD_L = 0;
    cfg.tFAW = 0; cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // A: fresh bank -> Empty. start = 0 + tRCD(5) = 5.
    // complete = start(5) + tCL(10) + transfer(1) = 16.
    auto a = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(a.start_cycle, 5ull);
    DDR_CHECK_EQ(a.complete_cycle, 16ull);

    // B: same bank, same row -> Hit. tCCD_L binds: start = 5 + 4 = 9 -- NOT
    // gated by A's complete_cycle(16) in any way; that's the whole point of
    // this test (a naive implementation would leak data-return latency into
    // command spacing and turn a latency bug into a throughput bug).
    // complete = 9 + tCL(10) + 1 = 20.
    auto b = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(b.start_cycle, 9ull);
    DDR_CHECK_EQ(b.complete_cycle, 20ull);
}

DDRTEST(trtp_still_delays_precharge_on_row_conflict) {
    // Guard against over-correcting FIX 1: tRTP must still delay the
    // PRECHARGE that a row conflict requires. This test's expectation is
    // unchanged by the FIX 1 rework -- it must pass both before and after.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0; // isolate this test from FIX 2's CAS latency
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 2; cfg.tCCD_L = 4;
    cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 1000;
    cfg.tRTP = 12; cfg.tWR = 20;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // A: (bank0, row0) fresh -> Empty. start=5, complete=6.
    auto a = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(a.start_cycle, 5ull);

    // B: (bank0, row1) -- Conflict. Precharge can't start before
    // col_start(5) + tRTP(12) = 17 (beats row_opened_at(0) + tRAS(10) = 10).
    // act_start = 17 + tRP(5) = 22. start = 22 + tRCD(5) = 27.
    auto b = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 1), 0);
    DDR_CHECK_EQ(b.start_cycle, 27ull);
    DDR_CHECK(b.row_status == RowStatus::Conflict);
}

DDRTEST(twtr_delays_read_after_write_beyond_bus_turnaround) {
    // tWTR is a separate, additive DRAM-internal constraint on top of the
    // bus-direction turnaround: after a write's data burst ends, a
    // same-rank read must wait tWTR_L (same bank group) / tWTR_S (different
    // bank group), measured from the write's complete_cycle (end of its
    // data burst), not its col_start.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0;
    cfg.tRCD = 5; cfg.tCCD_L = 2; cfg.tCCD_S = 2;
    cfg.tWTR_L = 10; cfg.tWTR_S = 4;
    cfg.wr_rd_turnaround = 1; cfg.rd_wr_turnaround = 0;
    cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tRP = 0; cfg.tRAS = 0; cfg.tRC = 0;
    cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 1000;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    ChannelScheduler sched(cfg, 0);

    // A: WRITE (bg0, bank0, row0) -> Empty. start = 0 + tRCD(5) = 5,
    // complete = 5 + tCWL(0) + transfer(1) = 6.
    auto a = admit_and_drain(sched, make_cmd(TxnType::Write, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(a.start_cycle, 5ull);
    DDR_CHECK_EQ(a.complete_cycle, 6ull);

    // B: READ (bg0, bank0, row0) -> Hit, same bank group as the write.
    // Competing floors: tCCD_L -> 5+2=7; bus turnaround -> bus_free(6)+
    // wr_rd_turnaround(1)=7; tWTR_L -> write complete(6)+10=16. tWTR_L wins.
    auto b = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(b.start_cycle, 16ull);
}

DDRTEST(twtr_uses_the_short_bound_across_bank_groups) {
    // Same setup, but the read targets a DIFFERENT bank group than the
    // write -- tWTR_S(4) applies instead of tWTR_L(10).
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0;
    cfg.tRCD = 5; cfg.tCCD_L = 2; cfg.tCCD_S = 2;
    cfg.tWTR_L = 10; cfg.tWTR_S = 4;
    cfg.wr_rd_turnaround = 1; cfg.rd_wr_turnaround = 0;
    cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tRP = 0; cfg.tRAS = 0; cfg.tRC = 0;
    cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 1000;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    ChannelScheduler sched(cfg, 0);

    // A: WRITE (bg0, bank0, row0) -> Empty. start=5, complete=6.
    auto a = admit_and_drain(sched, make_cmd(TxnType::Write, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(a.start_cycle, 5ull);

    // B: READ (bg1, bank0, row0) -- different bank group, never-opened bank
    // -> Empty. Competing floors: tCCD_S -> last_col_start(5)+2=7; bus
    // turnaround -> bus_free(6)+1=7; tWTR_S -> write complete(6)+4=10.
    // tWTR_S wins: act_start=10, start = 10 + tRCD(5) = 15.
    auto b = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 1, 0, 0), 0);
    DDR_CHECK_EQ(b.start_cycle, 15ull);
    DDR_CHECK(b.row_status == RowStatus::Empty);
}

DDRTEST(refresh_closes_open_rows) {
    // Real REFRESH requires all banks precharged, so every row in that rank
    // must close afterwards -- otherwise the next access is misreported as
    // a page hit when it's really an Empty (fresh activate).
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0;
    cfg.tREFI = 100; cfg.tRFC = 20;
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 1; cfg.tRC = 0;
    cfg.tCCD_S = 0; cfg.tCCD_L = 0; cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 1000;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // A: Read (bank0, row0) at ready_cycle 0 -> Empty. start = 0+tRCD(5) = 5.
    auto a = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(a.start_cycle, 5ull);
    DDR_CHECK(a.row_status == RowStatus::Empty);

    // B: Read (bank0, row0 again) at ready_cycle 150. A refresh came due at
    // 100 and ended at 120; 150 is past it so nothing blocks B's earliest --
    // but that refresh closed the row, so this must be Empty, not Hit:
    // start = 150 + tRCD(5) = 155.
    auto b = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 150);
    DDR_CHECK_EQ(b.start_cycle, 155ull);
    DDR_CHECK(b.row_status == RowStatus::Empty);
}

DDRTEST(fr_fcfs_breaks_ties_by_arrival_order) {
    // Two candidates with the same priority (both Empty/need-activate,
    // targeting different never-opened banks) must resolve in admission
    // order -- the classic FCFS tie-break.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0; // isolate this test from FIX 2's CAS latency
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 1; cfg.tCCD_L = 1; cfg.tRRD_S = 1; cfg.tRRD_L = 1; cfg.tFAW = 0;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    DramCommand first_admitted = make_cmd(TxnType::Read, 0, 0, 0, 0);
    DramCommand second_admitted = make_cmd(TxnType::Read, 0, 0, 1, 0);
    sched.try_admit(first_admitted, 0);
    sched.try_admit(second_admitted, 0);

    DramCommand drained_first = sched.drain_one();
    DDR_CHECK_EQ(drained_first.addr.bank, 0u);
    DramCommand drained_second = sched.drain_one();
    DDR_CHECK_EQ(drained_second.addr.bank, 1u);
}
