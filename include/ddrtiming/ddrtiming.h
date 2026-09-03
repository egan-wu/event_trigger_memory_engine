#ifndef DDRTIMING_H
#define DDRTIMING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ddrt_engine ddrt_engine_t;

typedef enum { DDRT_READ = 0, DDRT_WRITE = 1 } ddrt_txn_type_t;
typedef enum { DDRT_ROW_HIT = 0, DDRT_ROW_CONFLICT = 1, DDRT_ROW_EMPTY = 2 } ddrt_row_status_t;

typedef struct {
    int core_id;
    ddrt_txn_type_t type;
    uint32_t axi_id;
    uint64_t addr;
    uint32_t size_bytes;   /* AXSIZE decoded to bytes-per-beat, e.g. 64 */
    uint32_t len_beats;    /* number of beats in the burst (AXLEN + 1 already applied) */
    const uint8_t* wstrb;  /* optional per-beat strobe bytes, NULL = full strobe (write only) */
    uint32_t wstrb_len;
} ddrt_axi_txn_t;

typedef struct {
    uint64_t txn_id;
    int core_id;
    ddrt_txn_type_t type;
    uint64_t addr;
    uint64_t issue_cycle;
    uint64_t complete_cycle;
    double latency_ns;
    ddrt_row_status_t dominant_row_status; /* status of the txn's first DRAM command */
    uint32_t hits;
    uint32_t conflicts;
    uint32_t empties;
} ddrt_txn_result_t;

typedef struct {
    uint64_t total_txns;
    uint64_t total_bytes;
    uint64_t total_cycles;
    double sim_time_ns;
    double avg_bandwidth_gbps;
    double peak_bandwidth_gbps;
    double bandwidth_utilization_pct;
    double avg_latency_ns;
    double page_hit_rate_pct;
    double row_conflict_rate_pct;
    double row_empty_rate_pct;
    double refresh_overhead_pct;
    double turnaround_overhead_pct;
} ddrt_summary_t;

/* Create an engine from a DDRC JSON config file. Returns NULL on failure. */
ddrt_engine_t* ddrt_create(const char* config_json_path);

void ddrt_destroy(ddrt_engine_t* engine);

/* Push one AXI transaction (streaming API, for embedding into other simulators). Returns 0 on success. */
int ddrt_push_txn(ddrt_engine_t* engine, const ddrt_axi_txn_t* txn, uint64_t* out_txn_id);

/* Mark a known synchronization point for core_id: its next transaction (on any
 * AXI ID) will not be scheduled until every transaction this core has already
 * pushed has completed. Use this wherever the source DMA model has a real
 * dependency/idle point that the (timestamp-less) log otherwise can't express
 * -- without it the engine assumes maximum eagerness. Returns 0 on success. */
int ddrt_push_barrier(ddrt_engine_t* engine, int core_id);

/* Convenience: parse a CSV AXI log file (AR/AW/BARRIER rows) and push all its
 * rows tagged with core_id. Returns 0 on success. */
int ddrt_load_log_file(ddrt_engine_t* engine, int core_id, const char* log_csv_path);

/* Run the scheduling simulation over everything pushed/loaded so far. Returns 0 on success. */
int ddrt_run(ddrt_engine_t* engine);

int ddrt_get_summary(ddrt_engine_t* engine, ddrt_summary_t* out);
uint64_t ddrt_get_num_results(ddrt_engine_t* engine);
int ddrt_get_result_at(ddrt_engine_t* engine, uint64_t index, ddrt_txn_result_t* out);

/* Removes every result with txn_id <= max_txn_id from the results accessible
 * via ddrt_get_result_at()/ddrt_get_num_results() -- for a long-running
 * caller (e.g. a daemon with no "end of log") to bound memory: drain results,
 * report them, then prune up through the highest txn_id you just reported.
 * ddrt_get_summary() is unaffected -- it's tracked cumulatively since the
 * engine was created and does not depend on what's still retained. There is
 * no way to recover a pruned result, so only prune what you've already
 * consumed. Returns 0 on success. */
int ddrt_prune_results_before(ddrt_engine_t* engine, uint64_t max_txn_id);

int ddrt_write_report_json(ddrt_engine_t* engine, const char* out_path);

const char* ddrt_last_error(ddrt_engine_t* engine);
const char* ddrt_version(void);

#ifdef __cplusplus
}
#endif

#endif /* DDRTIMING_H */
