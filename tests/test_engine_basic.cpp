#include "testing.hpp"
#include "core/config.hpp"
#include "core/engine.hpp"

#include <algorithm>

using namespace ddrtiming;

namespace {
DdrcConfig make_test_config() {
    DdrcConfig cfg;
    cfg.channels = 1;
    cfg.ranks_per_channel = 1;
    cfg.bankgroups = 1;
    cfg.banks_per_group = 1; // force every access onto the same bank
    cfg.rows = 1 << 20;
    cfg.columns = 1 << 10;
    cfg.data_bus_bytes = 8;
    cfg.clock_mhz = 1000.0; // exactly 1 ns/cycle, so ns_to_cycles is exact
    cfg.map_row = AddressField::contiguous(11, 20); // 2KB "page"
    cfg.tRCD = 5; cfg.tRP = 5; cfg.tRAS = 10; cfg.tRC = 15;
    cfg.tCCD_S = 1; cfg.tCCD_L = 1; cfg.tRRD_S = 1; cfg.tRRD_L = 1;
    cfg.tFAW = 4; cfg.tWTR_S = 1; cfg.tWTR_L = 1; cfg.tRTP = 1; cfg.tWR = 1;
    cfg.tREFI = 100000; cfg.tRFC = 50; // refresh effectively never fires in these short tests
    cfg.rd_wr_turnaround = 0; cfg.wr_rd_turnaround = 0;
    cfg.command_queue_depth = 8;
    cfg.max_outstanding_per_id = 4;
    return cfg;
}

AxiTxn make_read(uint64_t addr, uint32_t axi_id = 0) {
    AxiTxn t;
    t.core_id = 0;
    t.type = TxnType::Read;
    t.axi_id = axi_id;
    t.addr = addr;
    t.size_bytes = 64;
    t.len_beats = 1; // 64 bytes total = exactly one command chunk (data_bus_bytes*8)
    return t;
}
} // namespace

DDRTEST(page_hit_then_conflict_matches_hand_computed_cycles) {
    DdrcConfig cfg = make_test_config();
    Engine engine(cfg);

    engine.push_txn(make_read(0x000)); // row 0, bank was never opened -> empty
    engine.push_txn(make_read(0x040)); // same 2KB page -> hit
    engine.push_txn(make_read(0x800)); // different page, same bank -> conflict

    engine.run();

    const auto& r = engine.results();
    DDR_CHECK_EQ(r.size(), static_cast<size_t>(3));

    DDR_CHECK(r[0].dominant_row_status == RowStatus::Empty);
    DDR_CHECK_EQ(r[0].issue_cycle, 0ull);
    DDR_CHECK_EQ(r[0].complete_cycle, 13ull);

    DDR_CHECK(r[1].dominant_row_status == RowStatus::Hit);
    DDR_CHECK_EQ(r[1].issue_cycle, 1ull);
    DDR_CHECK_EQ(r[1].complete_cycle, 21ull);

    DDR_CHECK(r[2].dominant_row_status == RowStatus::Conflict);
    DDR_CHECK_EQ(r[2].issue_cycle, 2ull);
    DDR_CHECK_EQ(r[2].complete_cycle, 39ull);
}

DDRTEST(outstanding_cap_gates_new_issues_within_one_id) {
    DdrcConfig cfg = make_test_config();
    cfg.max_outstanding_per_id = 1; // force fully serialized issuing on this id
    Engine engine(cfg);

    engine.push_txn(make_read(0x000, /*axi_id=*/7));
    engine.push_txn(make_read(0x040, /*axi_id=*/7));

    engine.run();
    const auto& r = engine.results();
    DDR_CHECK_EQ(r.size(), static_cast<size_t>(2));
    DDR_CHECK(r[1].issue_cycle >= r[0].complete_cycle);
}

DDRTEST(different_axi_ids_are_independently_outstanding) {
    // Same scenario, but the second transaction uses a different AXI ID with
    // its own cap of 1. It should NOT be blocked by the first id's txn still
    // being in flight -- only by the shared 1-cycle-per-dispatch port.
    DdrcConfig cfg = make_test_config();
    cfg.max_outstanding_per_id = 1;
    Engine engine(cfg);

    engine.push_txn(make_read(0x000, /*axi_id=*/1));
    engine.push_txn(make_read(0x040, /*axi_id=*/2));

    engine.run();
    const auto& r = engine.results();
    DDR_CHECK_EQ(r.size(), static_cast<size_t>(2));
    // Port spacing (1 cycle) should gate this, not the first txn's completion.
    DDR_CHECK(r[1].issue_cycle < r[0].complete_cycle);
}

DDRTEST(run_is_safe_to_call_repeatedly_as_a_daemon_tick) {
    // No "end of log": push some, run(), push more (incl. a barrier), run()
    // again. Results must accumulate correctly and not be recomputed/duplicated.
    DdrcConfig cfg = make_test_config();
    Engine engine(cfg);

    engine.push_txn(make_read(0x000)); // empty
    engine.push_txn(make_read(0x040)); // hit
    engine.run();

    DDR_CHECK_EQ(engine.results().size(), static_cast<size_t>(2));
    TxnResult first_result_snapshot = engine.results()[0];

    engine.run(); // nothing new pushed -- must be a safe no-op
    DDR_CHECK_EQ(engine.results().size(), static_cast<size_t>(2));
    DDR_CHECK_EQ(engine.results()[0].complete_cycle, first_result_snapshot.complete_cycle);

    engine.push_barrier(0);
    engine.push_txn(make_read(0x800)); // conflict, gated by the barrier
    engine.run();

    const auto& r = engine.results();
    DDR_CHECK_EQ(r.size(), static_cast<size_t>(3));
    // Earlier results must be untouched by the second run() call.
    DDR_CHECK_EQ(r[0].complete_cycle, first_result_snapshot.complete_cycle);
    DDR_CHECK(r[2].dominant_row_status == RowStatus::Conflict);
    DDR_CHECK(r[2].issue_cycle >= r[1].complete_cycle);
}

DDRTEST(pruning_results_does_not_corrupt_cumulative_summary) {
    DdrcConfig cfg = make_test_config();
    Engine engine(cfg);

    engine.push_txn(make_read(0x000)); // empty
    engine.push_txn(make_read(0x040)); // hit
    engine.run();

    DDR_CHECK_EQ(engine.results().size(), static_cast<size_t>(2));
    SummaryStats before_prune = engine.summary();
    DDR_CHECK_EQ(before_prune.total_txns, static_cast<uint64_t>(2));

    uint64_t max_id_so_far = 0;
    for (const auto& r : engine.results()) max_id_so_far = std::max(max_id_so_far, r.txn_id);
    engine.prune_results_before(max_id_so_far);

    DDR_CHECK_EQ(engine.results().size(), static_cast<size_t>(0));
    // Cumulative summary must be untouched by pruning -- it's tracked
    // independently precisely so a drain-then-prune daemon loop stays correct.
    DDR_CHECK_EQ(engine.summary().total_txns, before_prune.total_txns);
    DDR_CHECK_EQ(engine.summary().total_bytes, before_prune.total_bytes);
    DDR_CHECK(engine.summary().avg_bandwidth_gbps == before_prune.avg_bandwidth_gbps);

    // More work after a prune must still schedule correctly (continuing the
    // same simulation, not restarting from a blank state).
    engine.push_txn(make_read(0x080)); // still same page -> hit
    engine.run();

    DDR_CHECK_EQ(engine.results().size(), static_cast<size_t>(1));
    DDR_CHECK(engine.results()[0].dominant_row_status == RowStatus::Hit);
    DDR_CHECK_EQ(engine.summary().total_txns, static_cast<uint64_t>(3));
}

DDRTEST(barrier_forces_next_txn_to_wait_for_prior_completion) {
    DdrcConfig cfg = make_test_config();
    Engine engine(cfg);

    engine.push_txn(make_read(0x000, /*axi_id=*/1));
    engine.push_txn(make_read(0x040, /*axi_id=*/2)); // independent id, would normally overlap
    engine.push_barrier(0);
    engine.push_txn(make_read(0x800, /*axi_id=*/1)); // must wait for BOTH prior txns

    engine.run();
    const auto& r = engine.results();
    DDR_CHECK_EQ(r.size(), static_cast<size_t>(3));

    uint64_t max_pre_barrier_complete = std::max(r[0].complete_cycle, r[1].complete_cycle);
    DDR_CHECK(r[2].issue_cycle >= max_pre_barrier_complete);
}
