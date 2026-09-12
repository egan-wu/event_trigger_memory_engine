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
    //
    // OLD (pre-rewrite) expectation here was "tCCD_L binds the ACT": that is
    // exactly the bug this rewrite removes -- tCCD is a column-to-column
    // (RD/WR-to-RD/WR) constraint and never gates PRE/ACT. Recomputed for
    // the new model:
    //   visible_cycle = max(ready=0, last_drain_col_start_=P's col_start=5)
    //                  = 5 (Q was admitted only after P was drained, via
    //                  admit_and_drain, so it becomes "visible" at P's
    //                  col_start -- see PendingCmd::visible_cycle).
    //   act_start floor = max(visible=5, bank1.precharge_ready=0) = 5;
    //   tRRD_L (same bg as P's activate) pushes it to P's act(0)+6 = 6;
    //   command-bus: P's own column command already occupies slot [5,7) --
    //   ACT's desired cycle 6 collides, pushed to 7.
    //   row_ready = 7 + tRCD(5) = 12.
    //   earliest (data-bus/tCCD world, unaffected by lookahead): tCCD_L
    //   from P's last_col_start(5)+4=9, beaten by bus_free(P's complete=6).
    //   max(9,6)=9.
    //   col_start = max(earliest=9, row_ready=12) = 12 (row ops exposed by
    //   3 beyond the data-bus floor -- not the fictitious tCCD-on-ACT path).
    //   Its own command-bus slot at 12 doesn't collide (nothing near it).
    auto q = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 1, 0), 0);
    DDR_CHECK_EQ(q.start_cycle, 12ull);
    DDR_CHECK(q.row_status == RowStatus::Empty);

    // R: (rank0, bg1, bank0) -- different bank group from Q's last column cmd.
    //   visible_cycle = max(0, last_drain_col_start_=Q's col_start=12) = 12.
    //   act_start floor = max(12, bank(bg1,0).precharge_ready=0) = 12;
    //   tRRD_S (different bg from Q's activate) -> Q's act(7)+2=9, beaten
    //   by 12.
    //   command-bus: Q's own column slot occupies [12,14) -- ACT's desired
    //   12 collides, pushed to 14.
    //   row_ready = 14 + tRCD(5) = 19.
    //   earliest: tCCD_S from Q's last_col_start(12)+2=14, beaten by
    //   bus_free (Q's complete=13). max(14,13)=14.
    //   col_start = max(earliest=14, row_ready=19) = 19.
    auto r = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 1, 0, 0), 0);
    DDR_CHECK_EQ(r.start_cycle, 19ull);
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
    // five different banks in the same rank, admitted and drained one at a
    // time (admit_and_drain) so each one's visible_cycle equals the
    // previous one's own col_start (see try_admit's visible_cycle comment).
    //
    // Under the OLD (pre-rewrite) model, PRE/ACT was gated by the data-bus/
    // tCCD floor, giving a clean act-then-tRCD cadence of 6 cycles/command
    // (0,6,12,18) until tFAW forced the 5th to 40. Under the NEW model,
    // ACT is gated by visible_cycle instead -- but visible_cycle_n equals
    // the PREVIOUS command's col_start, so ACT #n now wants to start at
    // (roughly) the same cycle its own column command used to start at,
    // one cycle range earlier than before. tRRD_L(1) never binds (tiny),
    // but that early ACT collides on the command bus with the previous
    // command's own column-command slot (which is still occupying
    // [col_start_{n-1}, col_start_{n-1}+kCmdSlotCycles)), so ACT #n gets
    // pushed forward by kCmdSlotCycles(2) each time -- this cascades into a
    // 7-cycle cadence (ACT+tRCD(5)+kCmdSlotCycles(2)) instead of the old
    // 6-cycle one, UNTIL tFAW takes over for the 5th.
    //
    // Hand-traced (act_start / row_ready=act+tRCD(5) / col_start; each
    // col_start's own command-bus slot never collides, since it lands well
    // past the previous command's slot):
    //   n=0: act=0 (fresh, no gating/refresh/collision). col_start=5.
    //   n=1: visible=5 (n=0's col_start). tRRD_L->0+1=1, beaten by 5:
    //        act(pre-slot)=5, but bank-bus slot [5,7) (n=0's column
    //        command's own slot) collides -> act_start=7. col_start=12.
    //   n=2: visible=12. tRRD_L->7+1=8, beaten by 12. Slot [12,14)
    //        (n=1's column slot) collides -> act_start=14. col_start=19.
    //   n=3: visible=19. tRRD_L->14+1=15, beaten by 19. Slot [19,21)
    //        collides -> act_start=21. col_start=26.
    //   n=4: visible=26. tRRD_L->21+1=22, beaten by 26 -- but then the
    //        4-activate window check (recent_activates=[0,7,14,21] at cap)
    //        forces max(26, activate#1(0)+tFAW(40)) = 40; the front(0)
    //        entry pops (0+40<=40). No command-bus collision at 40 (nearest
    //        slot is n=3's column command at 26). act_start=40.
    //        col_start = 40+tRCD(5) = 45 -- happens to match the OLD
    //        model's answer, since tFAW(40) dominates over both the
    //        visible_cycle floor(26) and the intermediate command-bus
    //        pushes that perturbed n=1..3.
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

    uint64_t expected[5] = {5, 12, 19, 26, 45};
    for (int i = 0; i < 5; ++i) {
        auto c = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, static_cast<uint32_t>(i), 0), 0);
        DDR_CHECK_EQ(c.start_cycle, expected[i]);
    }
}

DDRTEST(refresh_inserted_periodically_and_blocks_for_trfc) {
    // Single bank kept open (first access opens it, rest are hits). tRP/
    // tRAS/tRTP/tWR/tCCD/tRRD/tFAW are all zeroed so refresh is the only
    // *row-timing* thing perturbing cadence. Unlike the old version of this
    // test, tRCD is kept at kCmdSlotCycles(2) (not 0) and bytes are large
    // enough (32B -> 4 transfer cycles, > kCmdSlotCycles) that the command
    // bus never becomes a *second* binding constraint alongside refresh --
    // with tRCD=0 and a 1-cycle transfer, an Empty access's own ACT and its
    // column command would want the exact same command-bus slot, which is a
    // genuine, correct new interaction (see the command-bus-contention
    // test) but not what this test is isolating. tREFI=40, tRFC=10.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0; // isolate this test from FIX 2's CAS latency
    cfg.tRCD = 2; cfg.tRP = 0; cfg.tRAS = 0; cfg.tRC = 0;
    cfg.tCCD_S = 0; cfg.tCCD_L = 0; cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 0;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 40; cfg.tRFC = 10;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // Command 0 (Empty): act=0 (fresh bank/rank, refresh not due yet since
    // next_refresh_due initializes to tREFI=40). row_ready = 0+tRCD(2) = 2;
    // earliest=0 (first ever command) so col_start=2 (ACT's own slot [0,2)
    // is adjacent, not overlapping).
    // Commands 1..9 (Hits): each lands exactly transfer_cycles(4) after the
    // previous one -- earliest_n = bus_free_{n-1} = col_start_{n-1}+4
    // (cas=0), and bank.col_ready_cycle matches it exactly, and the gap
    // from the previous command's own command-bus slot is 4 >
    // kCmdSlotCycles(2), so no collision. col_start_n = 2 + 4n. Command 9
    // lands at 2+4*9 = 38.
    DramCommand last;
    for (int k = 0; k <= 9; ++k) {
        last = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0, 32), 0);
    }
    DDR_CHECK_EQ(last.start_cycle, 38ull);

    // Command 10: earliest (pre-refresh) = bus_free after command 9 =
    // col_start_9(38) + transfer(4) = 42 -- >= next_refresh_due(40), so
    // refresh fires: refresh_end = 40 + tRFC(10) = 50, contributing
    // 50-42=8 exposed cycles (not a clean 8=tRFC coincidence here -- earliest
    // overshot the due cycle by 2). The refresh force-closes every bank in
    // the rank, so command 10 is Empty (not the Hit its row would otherwise
    // have been). Its ACT floors on precharge_ready_cycle, which the
    // refresh maxed to 50 (beating visible_cycle=38, command 9's
    // col_start); no gating/command-bus collision (nearest slot is 12
    // cycles back) -- act_start=50. row_ready = 50+tRCD(2) = 52, which also
    // beats earliest(50) -> exposed by 2 more. col_start=52 (adjacent to
    // ACT's [50,52) slot).
    last = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0, 32), 0);
    DDR_CHECK_EQ(last.start_cycle, 52ull);
    DDR_CHECK_EQ(sched.stats().refresh_cycles, 8ull);

    // Commands 11..16 (Hits again): same +4/command cadence resumes from
    // 52. col_start_n = 52 + 4*(n-10); command 16 lands at 52+4*6 = 76.
    for (int k = 11; k <= 16; ++k) {
        last = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0, 32), 0);
    }
    DDR_CHECK_EQ(last.start_cycle, 76ull);

    // Command 17: earliest = bus_free after 16 = 76+4 = 80, exactly equal
    // to the second next_refresh_due (40+40=80) -- fires again: refresh_end
    // = 80+tRFC(10) = 90, contributing exactly tRFC(10) this time (earliest
    // landed exactly on the boundary, no overshoot). Empty again; ACT
    // floors on the refreshed precharge_ready_cycle (90, beating
    // visible_cycle=76), lands at 90 (no collision); row_ready=90+2=92,
    // beating earliest(90) -> exposed by 2; col_start=92.
    last = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0, 32), 0);
    DDR_CHECK_EQ(last.start_cycle, 92ull);
    DDR_CHECK_EQ(sched.stats().refresh_cycles, 18ull); // 8 + 10
}

DDRTEST(rw_turnaround_only_applied_on_direction_change) {
    // tRCD is left at 0 deliberately (unlike the refresh test above) to
    // additionally show a real, if incidental, new effect: with tRCD=0 an
    // Empty access's ACT and its own column command both want cycle 0 on
    // the command bus, which can't happen (two commands can never share a
    // cycle) -- cmd0's column command gets pushed to kCmdSlotCycles(2), not
    // 0. That's a one-time, command-bus-driven offset on cmd0 only; the
    // turnaround behavior under test is otherwise unaffected; the same
    // interaction is also examined directly in the command-bus test.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0; // isolate this test from FIX 2's CAS latency
    cfg.tRCD = 0; cfg.tRP = 0; cfg.tRAS = 0; cfg.tRC = 0;
    cfg.tCCD_S = 0; cfg.tCCD_L = 0; cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 0;
    cfg.tWTR_S = 0; cfg.tWTR_L = 0; cfg.tRTP = 0; cfg.tWR = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 3;
    cfg.wr_rd_turnaround = 5;
    ChannelScheduler sched(cfg, 0);

    // cmd0: Read, opens the row. act=0; row_ready=0+tRCD(0)=0 -- but the
    // ACT itself already claimed command-bus slot [0,2), so the column
    // command (desired cycle 0) collides and is pushed to 2. start=2,
    // complete=2+1=3.
    auto c0 = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(c0.start_cycle, 2ull);

    // cmd1: Write, same row (hit) -- direction change R->W: bus_free(3) +
    // rd_wr_turnaround(3) = 6 beats tCCD's own term (last_col_start(2)+0=2)
    // -- earliest=6, no command-bus collision (nearest slot ends at 2).
    // start=6.
    auto c1 = admit_and_drain(sched, make_cmd(TxnType::Write, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(c1.start_cycle, 6ull);

    // cmd2: Read, same row -- direction change W->R: bus_free(7) +
    // wr_rd_turnaround(5) = 12 beats tCCD's term (6+0=6) -- earliest=12, no
    // collision (nearest slot ends at 8). start=12.
    auto c2 = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(c2.start_cycle, 12ull);

    // cmd3: Read, same row -- SAME direction as cmd2, no turnaround added:
    // earliest = bus_free(13) exactly (beats tCCD's 12+0=12). But now the
    // command-bus slot collides: cmd2's own column slot occupies [12,14),
    // and 13 falls inside it -- pushed to 14.
    auto c3 = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0), 0);
    DDR_CHECK_EQ(c3.start_cycle, 14ull);

    // Command-bus contention never touches the turnaround stat -- still
    // exactly 3 (cmd1) + 5 (cmd2); cmd3 (same direction) adds none.
    DDR_CHECK_EQ(sched.stats().turnaround_cycles, 8ull);
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
    //
    // Also still holds after the lookahead rewrite (item 1): B is admitted
    // and drained via admit_and_drain, i.e. only after A has already
    // drained, so B's visible_cycle == A's col_start(5) -- strictly less
    // than bank.precharge_ready_cycle(17, from A's tRTP/tRAS), which is what
    // actually binds PRE's floor below. A single-command-at-a-time bank-
    // local conflict like this one can never benefit from lookahead (there
    // is nowhere else for it to hide behind -- see the two dedicated
    // lookahead tests for scenarios that do), so this guard is unaffected
    // either way.
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
    // -> Empty.
    //
    // OLD (pre-rewrite) expectation gated ACT on tCCD_S/turnaround/tWTR_S
    // (the data-bus world) directly. Under the new model PRE/ACT floors on
    // visible_cycle instead:
    //   visible_cycle = max(ready=0, last_drain_col_start_=A's col_start=5)
    //                  = 5.
    //   act_start floor = max(5, bank(bg1,0).precharge_ready=0) = 5;
    //   tRRD_S (different bg from A's activate, both 0 here) doesn't push
    //   past 5. Command-bus: A's own column command occupies slot [5,7) --
    //   ACT's desired 5 collides, pushed to 7.
    //   row_ready = 7 + tRCD(5) = 12.
    //   earliest (data-bus world, unaffected by lookahead): tCCD_S ->
    //   A's last_col_start(5)+2=7; bus turnaround -> bus_free(6)+
    //   wr_rd_turnaround(1)=7 (equal, adds nothing new); tWTR_S -> A's
    //   write-complete(6)+4=10. max(7,7,10)=10.
    //   col_start = max(earliest=10, row_ready=12) = 12 -- less than the
    //   old model's 15: tWTR_S/turnaround/tCCD_S no longer force ACT to
    //   wait, so the row ops get a head start and only 2 of the row-miss's
    //   cycles are actually exposed beyond tWTR_S's floor.
    auto b = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 1, 0, 0), 0);
    DDR_CHECK_EQ(b.start_cycle, 12ull);
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

// ---------------------------------------------------------------------
// Lookahead rewrite: PRE/ACT overlap with another bank's column commands.
// ---------------------------------------------------------------------

DDRTEST(row_miss_hides_behind_another_banks_transfers) {
    // Bank A (bg0,bank0) has an open row and three pending hits; bank B
    // (bg1,bank0 -- a different bank group) has a pending row miss that was
    // admitted before ANY drain happened, so its visible_cycle is 0. All
    // five commands are admitted up front, then drained one at a time, so
    // FR-FCFS sees all of them together.
    //
    // Priority at first pick: every command targets a bank whose row isn't
    // open YET (A's open access hasn't been drained yet either), so all
    // five tie at "idle" (priority 1) and admission order (seq) wins ->
    // A's open access goes first. After that drains, A's row is open, so
    // A's three hits (priority 0) all beat B's still-idle row miss
    // (priority 1) -- B is drained last. Only 3 hits are used (kPagematchLimit
    // is 4), so the pagematch cap never triggers; it isn't needed here since
    // Hit's priority(0) already beats Idle's priority(1) on every pick.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0;
    cfg.bankgroups = 2; cfg.banks_per_group = 1; // A = (bg0,bank0), B = (bg1,bank0)
    cfg.tRCD = 4; cfg.tRP = 4; cfg.tRAS = 8; cfg.tRC = 12;
    cfg.tCCD_S = 2; cfg.tCCD_L = 2;
    cfg.tRRD_S = 1; cfg.tRRD_L = 1; cfg.tFAW = 1000;
    cfg.tRTP = 0; cfg.tWR = 0; cfg.tWTR_S = 0; cfg.tWTR_L = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // bytes=16 -> transfer_cycles=2, comfortably above kCmdSlotCycles(2) so
    // consecutive same-bank hits never collide on the command bus (keeps
    // this trace to the mechanism under test).
    DramCommand a_open = make_cmd(TxnType::Read, 0, 0, 0, 0, 16);
    DramCommand b_miss = make_cmd(TxnType::Read, 0, 1, 0, 5, 16); // different bg, fresh bank, arbitrary row
    DramCommand a_hit1 = make_cmd(TxnType::Read, 0, 0, 0, 0, 16);
    DramCommand a_hit2 = make_cmd(TxnType::Read, 0, 0, 0, 0, 16);
    DramCommand a_hit3 = make_cmd(TxnType::Read, 0, 0, 0, 0, 16);
    sched.try_admit(a_open, 0);
    sched.try_admit(b_miss, 0); // visible_cycle = max(0, last_drain_col_start_=0) = 0
    sched.try_admit(a_hit1, 0);
    sched.try_admit(a_hit2, 0);
    sched.try_admit(a_hit3, 0);

    // A's open access: Empty, fresh bank. act=0 (nothing to wait on).
    // row_ready = 0+tRCD(4) = 4; earliest=0 (first command ever) ->
    // col_start=4.
    DramCommand a0 = sched.drain_one();
    DDR_CHECK_EQ(a0.start_cycle, 4ull);
    DDR_CHECK(a0.row_status == RowStatus::Empty);

    // A hit #1: earliest = tCCD_L(A's own last col_start(4)+2=6) vs
    // bus_free(A's complete=4+2=6) -> 6. col_start = max(6, col_ready=6) = 6.
    DramCommand a1 = sched.drain_one();
    DDR_CHECK_EQ(a1.start_cycle, 6ull);
    DDR_CHECK(a1.row_status == RowStatus::Hit);

    // A hit #2: earliest = max(6+2=8, bus_free=8) = 8. col_start=8.
    DramCommand a2 = sched.drain_one();
    DDR_CHECK_EQ(a2.start_cycle, 8ull);
    DDR_CHECK(a2.row_status == RowStatus::Hit);

    // A hit #3: earliest = max(8+2=10, bus_free=10) = 10. col_start=10.
    DramCommand a3 = sched.drain_one();
    DDR_CHECK_EQ(a3.start_cycle, 10ull);
    DDR_CHECK(a3.row_status == RowStatus::Hit);

    // B's row miss, drained LAST but its row ops were placed EARLY:
    //   visible_cycle = 0 (fixed at admission, before any drain).
    //   act_start floor = max(0, bank.precharge_ready=0) = 0; tRRD_S (diff
    //   bg from A's activate) -> A's act(0)+1=1, beats 0 -> gated to 1.
    //   Command-bus: A's open access's own ACT already claimed slot [0,2) --
    //   1 collides, pushed to 2. act_start=2.
    //   row_ready = 2+tRCD(4) = 6.
    // Meanwhile earliest (the data-bus/tCCD world, computed as of THIS
    // drain call): tCCD_S from A's last_col_start(10)+2=12, vs bus_free
    // (A hit#3's complete=10+2=12) -> 12.
    //   col_start = max(earliest=12, row_ready=6) = 12: B's PRE/ACT (done by
    //   cycle 6) were fully HIDDEN behind A's three hit transfers (which
    //   don't free the data bus until cycle 12) -- B's column command
    //   starts exactly when A's last hit's data ends, with NO added tRCD.
    DramCommand b0 = sched.drain_one();
    DDR_CHECK_EQ(b0.start_cycle, 12ull);
    DDR_CHECK(b0.row_status == RowStatus::Empty);
    DDR_CHECK(sched.stats().row_miss_hidden >= 1ull);
}

DDRTEST(row_ops_cannot_start_before_command_is_visible) {
    // Same banks/config as row_miss_hides_behind_another_banks_transfers,
    // but B is admitted only AFTER A's four commands (open + 3 hits) have
    // all drained -- so B's visible_cycle is A's last col_start, not 0.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0;
    cfg.bankgroups = 2; cfg.banks_per_group = 1;
    cfg.tRCD = 4; cfg.tRP = 4; cfg.tRAS = 8; cfg.tRC = 12;
    cfg.tCCD_S = 2; cfg.tCCD_L = 2;
    cfg.tRRD_S = 1; cfg.tRRD_L = 1; cfg.tFAW = 1000;
    cfg.tRTP = 0; cfg.tWR = 0; cfg.tWTR_S = 0; cfg.tWTR_L = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    // A's own numbers are identical to the previous test (they don't depend
    // on whether B happens to be sitting in the queue too): 4, 6, 8, 10.
    auto a0 = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0, 16), 0);
    DDR_CHECK_EQ(a0.start_cycle, 4ull);
    auto a1 = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0, 16), 0);
    DDR_CHECK_EQ(a1.start_cycle, 6ull);
    auto a2 = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0, 16), 0);
    DDR_CHECK_EQ(a2.start_cycle, 8ull);
    auto a3 = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 0, 0, 0, 16), 0);
    DDR_CHECK_EQ(a3.start_cycle, 10ull);

    // B, admitted now: visible_cycle = max(0, last_drain_col_start_=10) = 10
    // (a queue slot only frees -- and so B only becomes "visible" -- once
    // A's last command issues; see try_admit's comment).
    //   act_start floor = max(10, bank.precharge_ready=0) = 10; tRRD_S ->
    //   A's act(0)+1=1, beaten by 10. Command-bus: A hit#3's own column
    //   slot occupies [10,12) -- 10 collides, pushed to 12. act_start=12.
    //   row_ready = 12+tRCD(4) = 16.
    // earliest: tCCD_S from A's last_col_start(10)+2=12, vs bus_free(12) ->
    // 12.
    //   col_start = max(earliest=12, row_ready=16) = 16: exposed by exactly
    //   tRCD(4) beyond the data-bus floor -- the visibility floor (not any
    //   data-bus constraint) is what pushed this row miss's PRE/ACT out.
    auto b0 = admit_and_drain(sched, make_cmd(TxnType::Read, 0, 1, 0, 5, 16), 0);
    DDR_CHECK_EQ(b0.start_cycle, 16ull);
    DDR_CHECK(b0.row_status == RowStatus::Empty);
    DDR_CHECK(sched.stats().row_miss_exposed >= 1ull);
}

DDRTEST(command_bus_slot_contention_serializes_close_commands) {
    // Two different, both-fresh banks in the same bank group, admitted
    // together (both visible at 0) with tRRD/tCCD zeroed out so nothing
    // besides the command bus itself would ever separate their ACTs.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0;
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 0; cfg.tCCD_L = 0; cfg.tRRD_S = 0; cfg.tRRD_L = 0; cfg.tFAW = 1000;
    cfg.tRTP = 0; cfg.tWR = 0; cfg.tWTR_S = 0; cfg.tWTR_L = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    DramCommand x = make_cmd(TxnType::Read, 0, 0, 0, 0);
    DramCommand y = make_cmd(TxnType::Read, 0, 0, 1, 0);
    sched.try_admit(x, 0);
    sched.try_admit(y, 0);

    // X: fresh bank, nothing else pending yet -> act=0 (command-bus empty).
    // row_ready = 0+tRCD(5) = 5; earliest=0 (first command) -> col_start=5.
    DramCommand rx = sched.drain_one();
    DDR_CHECK_EQ(rx.start_cycle, 5ull);

    // Y: also fresh, and gating (tRRD=0) doesn't push its ACT off of 0
    // either -- its "ideal" issue cycle is the SAME as X's (0). But X's ACT
    // already occupies command-bus slot [0,2), so Y's collides and is
    // serialized to slot 2 instead (exactly kCmdSlotCycles later, not 0).
    // row_ready = 2+tRCD(5) = 7.
    // earliest: tCCD(0) from X's last_col_start(5) -> 5; bus_free(X's
    // complete=6) -> 6; max=6.
    //   col_start = max(6, 7) = 7.
    // Without command-bus contention, Y's ACT would have stayed at 0 (same
    // as X's -- nothing else gates it), giving row_ready=5 and
    // col_start=max(6,5)=6 -- one cycle earlier. The 6-vs-7 difference is
    // exactly kCmdSlotCycles(2) worth of ACT displacement working its way
    // through to the column command; it is the direct, observable proof
    // that the two commands' ACTs were serialized on the command bus.
    DramCommand ry = sched.drain_one();
    DDR_CHECK_EQ(ry.start_cycle, 7ull);
}

DDRTEST(trrd_and_tfaw_still_bind_with_simultaneous_early_acts) {
    // Five row misses to five different banks, ALL admitted before any
    // drain (so all five have visible_cycle == 0, the most permissive
    // lookahead condition possible) -- tRRD_S/L and tFAW must still bind
    // exactly as they do without lookahead.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0;
    cfg.banks_per_group = 5;
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 0; cfg.tCCD_L = 0;
    cfg.tRRD_S = 6; cfg.tRRD_L = 6;
    cfg.tFAW = 40;
    cfg.tRTP = 0; cfg.tWR = 0; cfg.tWTR_S = 0; cfg.tWTR_L = 0;
    cfg.tREFI = 1000000; cfg.tRFC = 0;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    DramCommand cmds[5];
    for (uint32_t i = 0; i < 5; ++i) {
        cmds[i] = make_cmd(TxnType::Read, 0, 0, i, 0);
        sched.try_admit(cmds[i], 0); // every one visible at cycle 0
    }

    // All five tie at "idle" priority on every pick (each targets a
    // different, still-fresh bank), so FR-FCFS drains them in admission
    // order: bank0..bank4.
    //
    // n=0: act=0 (nothing pending). row_ready=0+5=5, earliest=0 -> 5.
    // n=1: visible=0; tRRD_L -> bank0's act(0)+6=6 (beats 0) -> act=6, but
    //      bank0's OWN column-command slot [5,7) collides -> act=7.
    //      row_ready=7+5=12; earliest = max(tCCD(0)->5, bus_free(6)) = 6 ->
    //      col_start=12.
    // n=2: tRRD_L -> bank1's act(7)+6=13 (beats visible=0) -> act=13,
    //      collides with bank1's column slot [12,14) -> act=14.
    //      row_ready=19; earliest=max(12,13)=13 -> col_start=19.
    // n=3: tRRD_L -> bank2's act(14)+6=20 -> act=20, collides with bank2's
    //      column slot [19,21) -> act=21. row_ready=26; earliest=
    //      max(19,20)=20 -> col_start=26.
    // n=4: tRRD_L -> bank3's act(21)+6=27 -- but the rolling-4-activate
    //      window (recent_activates=[0,7,14,21], at capacity) forces
    //      max(27, activate#1(0)+tFAW(40)) = 40; front(0) pops (0+40<=40).
    //      No command-bus collision at 40 (nearest slot is bank3's column
    //      command at 26). act=40. row_ready=45; earliest=max(26,27)=27 ->
    //      col_start=45 -- tFAW dominates regardless of every bank's row
    //      ops being eligible to start at cycle 0.
    uint64_t expected[5] = {5, 12, 19, 26, 45};
    for (int i = 0; i < 5; ++i) {
        DramCommand r = sched.drain_one();
        DDR_CHECK_EQ(r.start_cycle, expected[i]);
        DDR_CHECK(r.row_status == RowStatus::Empty);
    }
}

DDRTEST(refresh_window_still_gates_early_acts_and_closes_rows) {
    // Two different-bank-group banks (A, B), both admitted together
    // (visible at 0), with a deliberately tiny tREFI/tRFC so refresh
    // boundaries land right where lookahead would otherwise place B's
    // PRE/ACT and its column command.
    DdrcConfig cfg = base_config();
    cfg.tCL = 0; cfg.tCWL = 0;
    cfg.bankgroups = 2; cfg.banks_per_group = 1; // A = (bg0,bank0), B = (bg1,bank0)
    cfg.tRCD = 3; cfg.tRP = 3; cfg.tRAS = 6; cfg.tRC = 9;
    cfg.tCCD_S = 1; cfg.tCCD_L = 1; cfg.tRRD_S = 1; cfg.tRRD_L = 1; cfg.tFAW = 1000;
    cfg.tRTP = 0; cfg.tWR = 0; cfg.tWTR_S = 0; cfg.tWTR_L = 0;
    cfg.tREFI = 5; cfg.tRFC = 4;
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    ChannelScheduler sched(cfg, 0);

    DramCommand a = make_cmd(TxnType::Read, 0, 0, 0, 0, 16);
    DramCommand b = make_cmd(TxnType::Read, 0, 1, 0, 0, 16);
    sched.try_admit(a, 0);
    sched.try_admit(b, 0); // visible_cycle = 0 (admitted before any drain)

    // A: fresh, first command ever -- refresh not due yet (next due =
    // tREFI(5)). act=0. row_ready=0+tRCD(3)=3; earliest=0 -> col_start=3.
    DramCommand ra = sched.drain_one();
    DDR_CHECK_EQ(ra.start_cycle, 3ull);
    DDR_CHECK(ra.row_status == RowStatus::Empty);

    // B: earliest (data-bus/tCCD world) = max(tCCD_S: A's last_col_start(3)
    // +1=4, bus_free: A's complete(3+2=5)) = 5. The top-level refresh check
    // against this `earliest`(5) fires (next_due=5): refresh_end =
    // 5+tRFC(4) = 9, contributing 4 exposed cycles; earliest becomes 9.
    // This closes EVERY bank in the rank (including A's just-opened row)
    // and floors both banks' precharge_ready_cycle to 9.
    //
    // B's ACT floor = max(visible=0, bank.precharge_ready=9) = 9 --
    // NOT the naive visible_cycle(0), which would have landed squarely
    // inside the just-processed refresh window [5,9). This is exactly the
    // "ACT must never fall inside a refresh window" guarantee: the refresh
    // side effect (via bank.precharge_ready_cycle) forces it out. tRRD_S
    // from A's act(0)+1=1 doesn't push further; no command-bus collision.
    // act_start=9.
    //
    // row_ready = 9+tRCD(3) = 12. A SECOND refresh is now due (next_due
    // advanced to 10 after the first) -- 12>=10 fires again: refresh_end =
    // 10+tRFC(4) = 14, contributing 14-12 = 2 more exposed cycles (earliest_
    // cycle(12) was 2 short of refresh_end(14)). Total refresh_cycles =
    // 4+2 = 6.
    // This is the "refresh comes due between the row ops and the column
    // command" case: rather than a full re-activate (another tRCD after
    // the second refresh -- the fully-correct but substantially more
    // complex behavior), this model conservatively delays the column
    // command straight to the refresh's end (14). That never lets a column
    // command run before its bank is genuinely ready, but slightly
    // understates the true delay in this narrow (tREFI-smaller-than-tRCD)
    // edge case -- see place_activate's / drain_one's comments.
    // col_start = max(earliest=9, row_ready_checked=14) = 14.
    DramCommand rb = sched.drain_one();
    DDR_CHECK_EQ(rb.start_cycle, 14ull);
    DDR_CHECK(rb.row_status == RowStatus::Empty);
    DDR_CHECK_EQ(sched.stats().refresh_cycles, 6ull);
}
