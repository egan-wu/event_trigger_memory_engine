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
    // CAS latency is deliberately zeroed: these tests hand-compute exact
    // cycle counts to pin down row-status transitions, barrier ordering and
    // window bucketing -- mechanisms tCL/tCWL only shift by a constant.
    // Nonzero CAS is covered directly in test_command_queue.cpp, and
    // end-to-end in cas_latency_is_visible_in_transaction_latency below.
    cfg.tCL = 0; cfg.tCWL = 0;
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

DDRTEST(overfetch_metrics_for_undersized_and_misaligned_reads) {
    // chunk_bytes = data_bus_bytes(8) * burst_beats(8, default) = 64.
    DdrcConfig cfg = make_test_config();
    Engine engine(cfg);

    AxiTxn undersized; // chunk-aligned address, but only asks for half a burst window
    undersized.core_id = 0;
    undersized.type = TxnType::Read;
    undersized.axi_id = 1;
    undersized.addr = 0x2000; // 0x2000 % 64 == 0
    undersized.size_bytes = 32;
    undersized.len_beats = 1;
    engine.push_txn(undersized);

    AxiTxn misaligned; // a full 64B request, but starting mid-window -> straddles two windows
    misaligned.core_id = 0;
    misaligned.type = TxnType::Read;
    misaligned.axi_id = 2;
    misaligned.addr = 0x2020; // 0x2000 + 32, not chunk-aligned
    misaligned.size_bytes = 64;
    misaligned.len_beats = 1;
    engine.push_txn(misaligned);

    AxiTxn aligned_full; // chunk-aligned and exactly one window -> no waste
    aligned_full.core_id = 0;
    aligned_full.type = TxnType::Read;
    aligned_full.axi_id = 3;
    aligned_full.addr = 0x3000;
    aligned_full.size_bytes = 64;
    aligned_full.len_beats = 1;
    engine.push_txn(aligned_full);

    engine.run();
    const auto& r = engine.results();
    DDR_CHECK_EQ(r.size(), static_cast<size_t>(3));

    DDR_CHECK_EQ(r[0].bytes, 32u);
    DDR_CHECK_EQ(r[0].dram_bytes, 64u); // one full burst window, half wasted

    DDR_CHECK_EQ(r[1].bytes, 64u);
    DDR_CHECK_EQ(r[1].dram_bytes, 128u); // straddles two windows -> two full bursts

    DDR_CHECK_EQ(r[2].bytes, 64u);
    DDR_CHECK_EQ(r[2].dram_bytes, 64u); // perfectly aligned, no waste

    const SummaryStats& s = engine.summary();
    DDR_CHECK_EQ(s.total_bytes, static_cast<uint64_t>(32 + 64 + 64));
    DDR_CHECK_EQ(s.total_dram_bytes, static_cast<uint64_t>(64 + 128 + 64));
    // efficiency = 160 / 256 * 100 = 62.5%
    DDR_CHECK(s.burst_efficiency_pct > 62.0 && s.burst_efficiency_pct < 63.0);
}

DDRTEST(zero_sized_transaction_does_not_underflow_chunk_math) {
    // size_bytes=0 and len_beats=0 together used to leave total_bytes at 0,
    // which underflowed (txn.addr + total_bytes - 1) in the burst-window
    // computation and could produce a huge/garbage num_chunks. Must not
    // hang, crash, or corrupt accounting for other, well-formed transactions.
    DdrcConfig cfg = make_test_config();
    Engine engine(cfg);

    AxiTxn degenerate;
    degenerate.core_id = 0;
    degenerate.type = TxnType::Read;
    degenerate.axi_id = 1;
    degenerate.addr = 0x000;
    degenerate.size_bytes = 0;
    degenerate.len_beats = 0;
    engine.push_txn(degenerate);

    engine.push_txn(make_read(0x800, /*axi_id=*/2)); // ordinary, must still work fine

    engine.run();
    const auto& r = engine.results();
    DDR_CHECK_EQ(r.size(), static_cast<size_t>(2));
    DDR_CHECK(r[0].complete_cycle >= r[0].issue_cycle);
    DDR_CHECK(r[0].dram_bytes >= r[0].bytes);
    DDR_CHECK(r[1].complete_cycle >= r[1].issue_cycle);
    DDR_CHECK_EQ(r[1].bytes, 64u); // ordinary transaction unaffected
}

DDRTEST(windowed_history_disabled_by_default) {
    DdrcConfig cfg = make_test_config(); // history_window_ns defaults to 0
    Engine engine(cfg);
    engine.push_txn(make_read(0x000));
    engine.run();
    DDR_CHECK_EQ(engine.windows().size(), static_cast<size_t>(0));
}

DDRTEST(windowed_history_buckets_by_issue_cycle_and_survives_pruning) {
    DdrcConfig cfg = make_test_config();
    cfg.history_window_ns = 10.0; // == 10 cycles exactly (1 ns/cycle in this config)
    Engine engine(cfg);

    engine.push_txn(make_read(0x000)); // issue=0, Empty, complete=13 (hand-verified pattern)
    engine.push_barrier(0);
    engine.push_txn(make_read(0x040)); // gated to issue=13 by the barrier; same page -> Hit, complete=21

    engine.run();

    const auto& r = engine.results();
    DDR_CHECK_EQ(r.size(), static_cast<size_t>(2));
    DDR_CHECK_EQ(r[0].issue_cycle, 0ull);
    DDR_CHECK_EQ(r[1].issue_cycle, 13ull);

    const auto& w = engine.windows();
    DDR_CHECK(w.size() >= 2); // window 0 (cycles 0-9) and window 1 (cycles 10-19)

    DDR_CHECK_EQ(w[0].bytes_read, 64ull);
    DDR_CHECK_EQ(w[0].txn_count, 1ull);
    DDR_CHECK_EQ(w[0].empties, 1ull);
    DDR_CHECK_EQ(w[0].hits, 0ull);

    DDR_CHECK_EQ(w[1].bytes_read, 64ull);
    DDR_CHECK_EQ(w[1].txn_count, 1ull);
    DDR_CHECK_EQ(w[1].hits, 1ull);

    size_t window_count_before_prune = w.size();
    uint64_t max_id = 0;
    for (const auto& res : engine.results()) max_id = std::max(max_id, res.txn_id);
    engine.prune_results_before(max_id);

    DDR_CHECK_EQ(engine.results().size(), static_cast<size_t>(0));
    // Windows are tracked independently of results(), same as summary().
    DDR_CHECK_EQ(engine.windows().size(), window_count_before_prune);
    DDR_CHECK_EQ(engine.windows()[0].bytes_read, 64ull);
    DDR_CHECK_EQ(engine.windows()[1].bytes_read, 64ull);
}

DDRTEST(windowed_history_tracks_outstanding_high_water_and_active_banks) {
    DdrcConfig cfg = make_test_config();
    cfg.bankgroups = 2;
    cfg.banks_per_group = 2; // total_banks = 1 channel * 1 rank * 2 bg * 2 banks = 4
    cfg.map_bank = AddressField::contiguous(10, 1);
    cfg.map_bankgroup = AddressField::contiguous(11, 1);
    cfg.map_row = AddressField::contiguous(12, 10);
    cfg.history_window_ns = 100000.0; // huge -- everything below lands in window 0
    cfg.max_outstanding_per_id = 2;
    DDR_CHECK_EQ(cfg.total_banks(), 4);
    Engine engine(cfg);

    // Three distinct (bank, bankgroup) combinations: (0,0), (1,0), (0,1).
    engine.push_txn(make_read(0x000, /*axi_id=*/9));
    engine.push_txn(make_read(0x400, /*axi_id=*/9)); // bit10 set -> bank 1, same bankgroup
    engine.push_txn(make_read(0x800, /*axi_id=*/9)); // bit11 set -> bank 0, other bankgroup

    // A separate id pushed 3x with cap=2: the 3rd must wait for one of the
    // first two to complete, so occupancy should peak at exactly the cap.
    engine.push_txn(make_read(0x1000, /*axi_id=*/5));
    engine.push_txn(make_read(0x1040, /*axi_id=*/5));
    engine.push_txn(make_read(0x1080, /*axi_id=*/5));

    engine.run();

    DDR_CHECK(engine.windows().size() >= 1);
    const WindowStats& w = engine.windows()[0];
    DDR_CHECK_EQ(w.active_banks.size(), static_cast<size_t>(3));
    DDR_CHECK_EQ(w.max_outstanding_count, 2ull);
}

DDRTEST(windowed_history_reveals_channel_imbalance_invisible_in_aggregate) {
    // The whole point: 2 channels, all traffic on channel 0, channel 1 idle.
    // The aggregate avg_bandwidth_gbps alone can't distinguish this from an
    // evenly split load -- the per-channel breakdown must.
    DdrcConfig cfg = make_test_config();
    cfg.channels = 2;
    cfg.map_channel = AddressField::contiguous(24, 1); // bit24 selects channel
    cfg.history_window_ns = 100000.0; // huge -- everything lands in window 0
    Engine engine(cfg);

    // All on channel 0 (bit24 clear).
    for (int i = 0; i < 4; ++i) {
        engine.push_txn(make_read(static_cast<uint64_t>(i) * 0x40, /*axi_id=*/static_cast<uint32_t>(i + 1)));
    }
    engine.run();

    DDR_CHECK(engine.windows().size() >= 1);
    const WindowStats& w = engine.windows()[0];
    DDR_CHECK(w.dram_bytes_per_channel.size() >= 1);
    DDR_CHECK(w.dram_bytes_per_channel[0] > 0);
    // Channel 1 either wasn't touched at all (vector too short) or is exactly 0.
    bool channel1_idle = (w.dram_bytes_per_channel.size() < 2) || (w.dram_bytes_per_channel[1] == 0);
    DDR_CHECK(channel1_idle);
    DDR_CHECK_EQ(w.dram_bytes_per_channel[0], w.dram_bytes); // channel 0 alone accounts for the whole aggregate
}

// Guards the end-to-end CAS path. Every other test in this file zeroes
// tCL/tCWL so its hand-computed cycle counts stay readable, which would
// otherwise leave the engine-level plumbing of CAS latency -- from
// ChannelScheduler through TxnResult::complete_cycle into latency_ns --
// with no coverage at all. Rather than assert an absolute number, this runs
// the same single transaction twice and pins the *difference* to tCL
// exactly: that isolates CAS from every other constraint without having to
// re-derive them, and fails loudly if CAS ever stops reaching latency_ns.
DDRTEST(cas_latency_is_visible_in_transaction_latency) {
    auto run_one = [](double tCL) {
        DdrcConfig cfg = make_test_config(); // 1 ns/cycle, tRCD = 5
        cfg.tCL = tCL;
        Engine engine(cfg);
        engine.push_txn(make_read(0x000)); // fresh bank -> Empty
        engine.run();
        return engine.results()[0];
    };

    // act=0, col_start = 0 + tRCD(5) = 5, transfer = 64B / 8B = 8 cycles.
    const TxnResult no_cas = run_one(0.0);
    DDR_CHECK_EQ(no_cas.complete_cycle, 13ull); // 5 + 0 + 8

    // Same command, CAS latency 10 ns: data starts 10 cycles after the
    // column command instead of immediately.
    const TxnResult with_cas = run_one(10.0);
    DDR_CHECK_EQ(with_cas.complete_cycle, 23ull); // 5 + 10 + 8
    DDR_CHECK_EQ(with_cas.complete_cycle - no_cas.complete_cycle, 10ull);

    // latency_ns must carry the same delta -- a regression that dropped CAS
    // only in the reported latency (not the cycle count) would slip past the
    // checks above.
    DDR_CHECK(with_cas.latency_ns - no_cas.latency_ns > 9.999);
    DDR_CHECK(with_cas.latency_ns - no_cas.latency_ns < 10.001);
}

// make_test_config() maps row to bits [11, 31), so the map decodes bits
// [0, 31) and anything at bit 31 or above is ignored by the decoder. Two
// cases must be told apart: a whole trace sitting above a constant DRAM
// base offset (harmless -- the offset just drops out, one region) versus
// buffers placed further apart than the modeled capacity (they collapse
// onto the same banks/rows and silently share open rows, >1 region).
DDRTEST(high_address_regions_flags_aliasing_but_not_a_base_offset) {
    {
        Engine engine(make_test_config());
        engine.push_txn(make_read(0x80000000ull)); // bit 31 set: a typical DRAM base
        engine.push_txn(make_read(0x80000040ull));
        engine.push_txn(make_read(0x80000800ull));
        engine.run();
        DDR_CHECK_EQ(engine.summary().mapped_address_bits, 31);
        DDR_CHECK_EQ(engine.summary().high_address_regions, 1ull);
    }
    {
        // Same low address in four "per-core" buffers 4GB apart -- exactly
        // the synthetic llama trace's original layout. All four decode to
        // one bank/row/column.
        Engine engine(make_test_config());
        for (uint64_t core = 0; core < 4; ++core) engine.push_txn(make_read(core << 32));
        engine.run();
        DDR_CHECK_EQ(engine.summary().high_address_regions, 4ull);
        // ...and the consequence the check exists to expose: after the
        // first access opens the row, the other three are page hits.
        DDR_CHECK_EQ(engine.summary().page_hit_rate_pct > 74.9, true);
    }
}
