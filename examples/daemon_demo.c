/* Demonstrates the daemon usage pattern: push some AXI transactions, run(),
 * drain+report the new results, prune them, and repeat -- showing that
 * results() only ever holds "new since last prune" while summary() keeps
 * accumulating correctly regardless of pruning. Uses only the public C API
 * (ddrtiming.h), exactly like an external caller would. */
#include <inttypes.h>
#include <stdio.h>

#include "ddrtiming/ddrtiming.h"

static const char* row_status_str(ddrt_row_status_t s) {
    switch (s) {
        case DDRT_ROW_HIT: return "hit";
        case DDRT_ROW_CONFLICT: return "conflict";
        case DDRT_ROW_EMPTY: return "empty";
    }
    return "?";
}

static void print_summary(ddrt_engine_t* e) {
    ddrt_summary_t s;
    ddrt_get_summary(e, &s);
    printf("  summary: total_txns=%" PRIu64 " total_bytes=%" PRIu64
           " avg_bw=%.3f GB/s hit_rate=%.1f%% conflict_rate=%.1f%%\n",
           s.total_txns, s.total_bytes, s.avg_bandwidth_gbps,
           s.page_hit_rate_pct, s.row_conflict_rate_pct);
}

/* Simulates one "tick" of a daemon consumer: read everything currently in
 * results(), print it as if reporting it somewhere, then prune up through
 * the highest txn_id just reported. */
static void drain_report_and_prune(ddrt_engine_t* e) {
    uint64_t n = ddrt_get_num_results(e);
    printf("  results() currently holds %" PRIu64 " transaction(s):\n", n);
    uint64_t max_id = 0;
    for (uint64_t i = 0; i < n; ++i) {
        ddrt_txn_result_t r;
        ddrt_get_result_at(e, i, &r);
        printf("    txn_id=%" PRIu64 " core=%d addr=0x%" PRIx64
               " issue=%" PRIu64 " complete=%" PRIu64 " status=%s\n",
               r.txn_id, r.core_id, r.addr, r.issue_cycle, r.complete_cycle,
               row_status_str(r.dominant_row_status));
        if (r.txn_id > max_id) max_id = r.txn_id;
    }
    if (n > 0) {
        ddrt_prune_results_before(e, max_id);
        printf("  pruned up through txn_id=%" PRIu64 " -> results() now holds %" PRIu64 "\n",
               max_id, ddrt_get_num_results(e));
    }
}

static void push_read(ddrt_engine_t* e, uint64_t addr, uint32_t axi_id) {
    ddrt_axi_txn_t t;
    t.core_id = 0;
    t.type = DDRT_READ;
    t.axi_id = axi_id;
    t.addr = addr;
    t.size_bytes = 64;
    t.len_beats = 1;
    t.wstrb = NULL;
    t.wstrb_len = 0;
    uint64_t txn_id;
    ddrt_push_txn(e, &t, &txn_id);
}

int main(void) {
    ddrt_engine_t* e = ddrt_create("examples/ddrc_config.example.json");
    if (!e) {
        fprintf(stderr, "ddrt_create failed: %s\n", ddrt_last_error(NULL));
        return 1;
    }

    printf("=== Tick 1: daemon pushes 3 sequential reads (same page) ===\n");
    push_read(e, 0x00100000, 1);
    push_read(e, 0x00100040, 1);
    push_read(e, 0x00100080, 1);
    ddrt_run(e);
    drain_report_and_prune(e);
    print_summary(e);

    printf("\n=== Tick 2: daemon hits a known sync point, then pushes more ===\n");
    push_read(e, 0x001000C0, 1);
    ddrt_push_barrier(e, 0);
    push_read(e, 0x00100800, 1); /* different page, same bank -> conflict */
    ddrt_run(e);
    drain_report_and_prune(e);
    print_summary(e);

    printf("\n=== Tick 3: more work pushed, but this tick we DON'T prune ===\n");
    push_read(e, 0x00100840, 2); /* different AXI ID */
    push_read(e, 0x00100880, 2);
    ddrt_run(e);
    {
        uint64_t n = ddrt_get_num_results(e);
        printf("  results() holds %" PRIu64 " transaction(s) (nothing pruned this tick)\n", n);
    }
    print_summary(e);

    printf("\n=== Tick 4: no new data pushed -- run() should be a safe no-op ===\n");
    ddrt_run(e);
    printf("  results() still holds %" PRIu64 " transaction(s)\n", ddrt_get_num_results(e));
    print_summary(e);

    ddrt_destroy(e);
    return 0;
}
