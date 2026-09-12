# ddrtiming — Tier-1 DDR/DDRC Timing Estimator

## 1. Goal & Audience

A fast, approximate DDR/DDRC performance estimator (Tier 1 of a two-tier ROI
strategy — Tier 2 being a precise, cycle-accurate SystemC DDRC+DRAM model,
out of scope here). It consumes per-core AXI AR/AW transaction logs (dispatch
**order** only, no real timestamps) and estimates DRAM-level performance —
bandwidth, page-hit/row-conflict rate, latency, refresh/turnaround overhead —
by simulating a simplified DDRC command scheduler and DRAM timing model.
Target accuracy: roughly 70-75% of a cycle-accurate reference, in exchange
for iteration times orders of magnitude faster.

No third-party dependencies (C++17 + CMake + a hand-rolled JSON
reader/writer), built as a C-ABI static library plus a CLI, so it drops into
other simulators (C/C++, or Python via `ctypes`) or runs standalone.

Built for:

- **Architects** doing early DMA-workload-vs-DRAM-controller what-if analysis, before committing to a full cycle-accurate model
- **Simulation/tooling engineers** embedding a DRAM timing estimate into an existing simulator or CI pipeline
- **AI agents / automated pipelines** exploring an address-mapping or DDRC config design space that need fast, structured, machine-parseable results — see §6

Because the input log has no timestamps, the engine reconstructs timing under
an explicit "maximum eagerness" assumption: a transaction issues as early as
physically permitted (its AXI-ID's outstanding cap, its core's dispatch port,
any barrier you've inserted — §4.2). This is the single biggest source of
estimation error versus reality — if your DMA model has idle/dependency
points invisible in the AXI stream itself, express them with a barrier or
the estimate will overstate achievable concurrency.

Not in scope: Tier-2 SystemC calibration, scheduler policies other than
FR-FCFS, GUI/dashboard reporting (JSON/CSV only — plotting is left to the
consumers described in §5).

## 2. Install

Requirements: CMake ≥ 3.15, a C++17 compiler. No third-party dependencies.

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build
```

Produces (exact path depends on generator, e.g. `build/Release/` with MSVC):

| binary | what it is |
|---|---|
| `libddrtiming` (static) | the engine, C ABI — link this into your own tool |
| `ddrtiming_cli` | standalone CLI (§4.1) |
| `windowed_history_analyze` | standalone stats exporter for windowed-history CSVs (§5.4) |
| `daemon_demo` | reference implementation of the streaming push/run/prune usage pattern |
| `golden_check` | regression harness used by `ctest` (see `bench/README.md`) |

Smoke test:

```bash
./build/ddrtiming_cli --config examples/ddrc_config.example.json \
    --log examples/core0_axi.example.csv --out report.json
```

## 3. Features

- Event-driven DDRC command scheduler (FR-FCFS) over a full DDR4/5-class timing set: tRCD/tRP/tRAS/tRC, tCL/tCWL (CAS latency), tCCD_S/L, tRRD_S/L, tFAW, tWTR_S/L, tRTP/tWR, R↔W bus turnaround, periodic refresh (tREFI/tRFC)
- Configurable address mapping: contiguous bit-fields or scattered bit-gather per field, with optional XOR-hash interleaving
- Per-bank row-buffer tracking → page-hit / row-conflict / row-empty classification per DRAM command
- Per-core AXI burst-size distribution (mean + quartiles) — see whether one core's requests are systematically smaller than another's before blaming the DRAM scheduler for its throughput
- Over-fetch accounting: logical bytes requested vs. physical burst-aligned bytes DRAM actually moved
- Input-integrity check: flags address aliasing (trace regions that silently collapse onto the same physical DRAM locations — `high_address_regions`, §5.1)
- Windowed time-series history: bandwidth, row-status, outstanding-request occupancy, bank utilization, bank-group reuse — with a per-channel breakdown
- Streaming C API built for long-running/daemon-style callers (push → run → drain → prune), not just one-shot batch files
- Barrier primitive to express real synchronization points a timestamp-less AXI log can't otherwise carry
- Thread-safe engine handle (internally mutex-protected — safe for one OS thread per DMA core to push into the same handle)
- Two ways to consume windowed history: a dependency-free HTML viewer for humans (`tools/windowed_history_viewer.html`, `file://`, no build step) and a JSON statistics exporter for scripts/agents (`windowed_history_analyze`, §5.4)
- `--validate-only`: check a config for free — no log, no simulation run

## 4. API & Input Spec

### 4.1 CLI

```
ddrtiming_cli --config <ddrc_config.json> --log <core0.csv> [--log <core1.csv> ...] \
    [--out <report.json>] [--windowed-csv <history.csv>]
ddrtiming_cli --config <ddrc_config.json> --validate-only
```

Each `--log` is assigned `core_id` = its position (0, 1, 2, ...).
`--windowed-csv` requires `"reporting": {"history_window_ns": N}` in the
config. Exit code `0` on success, `1` on any error (config/log/IO) — the
message goes to stderr.

### 4.2 AXI log format (CSV)

One file per core (matches `core_x_axi_package.log` naming), header row
required:

```
type,id,addr,size,len,wstrb
AR,3,0x80001000,64,8,
AW,1,0x80002000,64,8,FFFFFFFFFFFFFFFF
BARRIER,,,,,
```

| column | meaning |
|---|---|
| `type` | `AR`, `AW`, or `BARRIER` (barrier rows ignore every other column) |
| `id` | AXI ID, decimal or `0x`-hex. Same-ID transactions from a core are a strict in-order stream; different IDs are independently outstanding. Use `0` for everything if you don't track IDs — it degenerates to one FIFO stream per core. |
| `addr` | byte address, decimal or `0x`-hex |
| `size` | literal bytes transferred per beat (e.g. `64`) — not the raw 3-bit AxSIZE encoding |
| `len` | literal beat count — AxLEN + 1 already applied |
| `wstrb` | optional hex byte-string, AW rows only; blank = full-strobe. Recorded in results but doesn't affect timing. |

A `BARRIER` row (or `ddrt_push_barrier`) means this core's next transaction
won't be scheduled until every transaction it has already pushed has
completed. It's per-core, not per-ID. A burst larger than one DRAM burst
(`burst_beats`, §4.3) is split into burst-aligned chunks, each decoded and
scheduled independently — so one AXI transaction can contribute multiple
hit/conflict/empty classifications (§5.2).

### 4.3 DDRC config (JSON)

See `examples/ddrc_config.example.json`. Every field has a default (below);
only override what you need.

| section | field(s) | default | meaning |
|---|---|---|---|
| `topology` | `channels`, `ranks_per_channel`, `bankgroups`, `banks_per_group` | `1, 1, 1, 4` | DRAM topology |
| | `rows`, `columns` | `65536, 1024` | addressable row/column counts |
| | `data_bus_bytes` | `8` | bytes per beat, per channel |
| | `burst_beats` | `8` | beats per minimum DRAM access (BL8=8, BC4=4) — drives over-fetch accounting |
| | `clock_mhz` | `1600` | **effective transfer rate in MT/s** (e.g. `3200` for DDR4-3200) — not the DRAM core clock |
| `address_mapping` | `channel`, `rank`, `bankgroup`, `bank`, `row` | unset field = always 0 | `{"bit_start": N, "bit_width": W}` (contiguous) or `{"bits": [b0, b1, ...]}` (scattered, LSB-first); optional `"hash_bits": [...]` XORs each gathered bit against another physical bit. Unmapped low bits are the implicit column/byte offset. Which field is fastest-changing affects `bankgroup_reuse_rate_pct`, §5.1. |
| `timing_ns` | `tRCD tRP tRAS tRC tCL tCWL tCCD_S/L tRRD_S/L tFAW tWTR_S/L tRTP tWR tREFI tRFC rd_wr_turnaround wr_rd_turnaround` | see `src/core/config.hpp` | all nanoseconds; `_S` = different bank group, `_L` = same bank group |
| `ddrc_resources` | `command_queue_depth` | `32` | per-channel queue depth (backpressure bound) |
| | `max_outstanding_per_id` | `16` | cap per `(core_id, axi_id)` stream, not per core |
| | `scheduling_policy` | `"fr_fcfs"` | only accepted value |
| `reporting` | `history_window_ns` | `0` (off) | window size for windowed history, §5.3 |

`DdrcConfig::validate()` (run automatically on load, or standalone via
`--validate-only`) rejects physically impossible configs — bit overlaps,
non-power-of-two topology counts, inverted timing relationships (e.g.
`tRC < tRAS + tRP`), unsupported enum values — naming the offending field.

### 4.4 C API

`include/ddrtiming/ddrtiming.h` — opaque `ddrt_engine_t*` handle. Functions
return `int` (`0` = ok, `-1` = failure; check `ddrt_last_error`). Every call
on a given handle is internally mutex-protected (safe to call from multiple
threads without your own locking), except `ddrt_destroy` — make sure no
other thread is still calling in before destroying the engine.

```c
ddrt_engine_t* e = ddrt_create("ddrc_config.json");

ddrt_axi_txn_t t = { .core_id = 0, .type = DDRT_READ, .axi_id = 3,
                      .addr = 0x80001000, .size_bytes = 64, .len_beats = 8 };
uint64_t txn_id;
ddrt_push_txn(e, &t, &txn_id);
ddrt_run(e);

ddrt_summary_t s;
ddrt_get_summary(e, &s);
printf("%.2f GB/s (%.1f%% of peak)\n", s.avg_bandwidth_gbps, s.bandwidth_utilization_pct);

ddrt_write_report_json(e, "report.json");
ddrt_destroy(e);
```

| function | does |
|---|---|
| `ddrt_create(path)` / `ddrt_destroy(e)` | load a config, create/free an engine |
| `ddrt_push_txn(e, &txn, &out_id)` | queue one AXI transaction (streaming) |
| `ddrt_push_barrier(e, core_id)` | mark a sync point — §4.2 |
| `ddrt_load_log_file(e, core_id, path)` | parse and push a whole CSV log |
| `ddrt_run(e)` | advance the simulation over everything pushed since the last call — safe to call repeatedly, a no-op if nothing's new |
| `ddrt_get_summary(e, &out)` | cumulative aggregate stats, §5.1 |
| `ddrt_get_num_results(e)` / `ddrt_get_result_at(e, i, &out)` | per-transaction results, §5.2 |
| `ddrt_prune_results_before(e, max_txn_id)` | free retained results with `txn_id ≤ max_txn_id`; `ddrt_get_summary()` is unaffected (tracked cumulatively, independent of pruning) |
| `ddrt_get_num_windows(e)` / `ddrt_get_window_at(e, i, &out)` | windowed history, §5.3 |
| `ddrt_get_num_channels(e)` / `ddrt_get_window_channel_stats(e, wi, ci, &out)` | per-channel breakdown of one window |
| `ddrt_get_num_cores(e)` / `ddrt_get_core_burst_stats_at(e, i, &out)` | per-core AXI burst-size distribution, §5.5 |
| `ddrt_write_report_json(e, path)` | write the full report (summary + transactions + windows) to JSON |
| `ddrt_last_error(e)` | error string for the last failed call; pass `NULL` to read a failed `ddrt_create()`'s error |
| `ddrt_version(void)` | library version string (currently `"0.3.0"`) |

For a long-running caller with no natural "end of log" (a daemon pushing
transactions as they happen): push, call `ddrt_run()` periodically as a
tick, drain `ddrt_get_result_at()` up to `ddrt_get_num_results()`, report it
wherever it needs to go, then `ddrt_prune_results_before()` through the
highest `txn_id` just reported to bound memory. `examples/daemon_demo.c` is
a complete, tested reference implementation of this loop, including a
mid-stream barrier.

## 5. Output Spec

Three ways to read the same underlying fields: the CLI's plain-text summary
(printed to stdout on every run — human-oriented, not meant to be parsed;
see §5.0), JSON (`ddrt_write_report_json` / CLI `--out`, shape
`{"summary": {...}, "transactions": [...], "core_burst_stats": [...], "windows": [...]}`,
`"windows"` present only when windowing is enabled), or the C API's typed
structs (`ddrt_get_summary` / `ddrt_get_result_at` / `ddrt_get_core_burst_stats_at`
/ `ddrt_get_window_at`). `summary()` is always cumulative since the engine
was created, regardless of what `prune_results_before()` has removed from
`results()`.

### 5.0 Plain-text CLI summary

Printed unconditionally by `ddrtiming_cli` (no flag needed — `--out`/
`--windowed-csv` are additional, not alternatives). Two blocks: system-wide
metrics first, then one labeled sub-table per per-core stat category — AXI
burst size (§5.5) is the first such sub-table, so a future per-core metric
adds another sub-table here rather than a new top-level block.

```
==== System Summary ====
Transactions:            4096
...
Address map decodes:     bits [0, 36)  -- 1 distinct region(s) above that

==== Per-Core Summary ====
AXI burst size (bytes):
  core         n     mean      min      p25      p50      p75      max
     0      1024     4096     4096     4096     4096     4096     4096
     ...
```

The `Per-Core Summary` block is omitted entirely if nothing has been pushed
yet (`ddrt_get_num_cores() == 0`).

### 5.1 Summary (`ddrt_summary_t`, JSON `"summary"`)

| field | meaning |
|---|---|
| `total_txns` | count of AXI transactions processed so far |
| `total_bytes` | logical: bytes actually requested (`size_bytes × len_beats`, summed) |
| `total_dram_bytes` | physical: full burst-aligned bytes DRAM actually moved, always ≥ `total_bytes` |
| `total_cycles` | furthest simulated point reached (`max(complete_cycle)`) |
| `sim_time_ns` | `total_cycles` converted to ns |
| `avg_bandwidth_gbps` | `total_bytes / sim_time_ns` — useful throughput delivered to the requester |
| `avg_dram_bandwidth_gbps` | `total_dram_bytes / sim_time_ns` — actual DRAM bus traffic, over-fetch included |
| `peak_bandwidth_gbps` | theoretical ceiling from the config (`data_bus_bytes × clock_mhz/1000 × channels`) |
| `bandwidth_utilization_pct` | `avg_dram_bandwidth_gbps / peak_bandwidth_gbps × 100` (physical, bus-side) |
| `burst_efficiency_pct` | `total_bytes / total_dram_bytes × 100` — 100% = no over-fetch waste |
| `avg_latency_ns` | mean `(complete_cycle − issue_cycle)` across all transactions, in ns |
| `page_hit_rate_pct` / `row_conflict_rate_pct` / `row_empty_rate_pct` | classification per DRAM column command (not per transaction — sum to 100%) |
| `bankgroup_reuse_rate_pct` | % of column commands that paid `tCCD_L` (same bank group as the previous command) instead of `tCCD_S` |
| `refresh_overhead_pct` | % of total channel-cycles spent blocked on refresh |
| `turnaround_overhead_pct` | % of total channel-cycles spent on R↔W bus turnaround |
| `mapped_address_bits` | how many low-order address bits the address map actually decodes |
| `high_address_regions` | distinct values seen of the bits *above* `mapped_address_bits`. `1` is normal; `>1` means separate parts of the trace alias onto the same DRAM locations and every rate above is describing a workload that doesn't exist — fix the trace's base addresses or widen the mapping before trusting anything else here |

No per-channel/per-bank breakdown at the summary level (everything above is
summed across all channels/banks) — use windowed history (§5.3) for that.

### 5.2 Per-transaction (`ddrt_txn_result_t`, JSON `"transactions"[]`)

One entry per AXI transaction (barriers never produce one), in dispatch
(chronological) order — not necessarily push order, since independent AXI
IDs can complete out of order relative to each other.

| field | meaning |
|---|---|
| `txn_id` | engine-assigned sequential ID, returned by `push_txn()` at push time |
| `core_id`, `type` (`AR`/`AW`), `addr` | echoed from the input |
| `bytes` | logical: total burst size requested (`size_bytes × len_beats`) |
| `dram_bytes` | physical: full burst-aligned bytes DRAM actually moved, ≥ `bytes` |
| `issue_cycle` | cycle the engine determined this could dispatch (outstanding cap, core port, barrier gate) |
| `complete_cycle` | cycle the last chunk of this burst finished transferring |
| `latency_ns` | `(complete_cycle − issue_cycle)` in ns |
| `dominant_row_status` | `hit`/`conflict`/`empty` of the burst's *first* DRAM command chunk only |
| `hits`, `conflicts`, `empties` | the same classification counted across *every* chunk of this burst |

### 5.3 Windowed history (`ddrt_window_stats_t`, JSON `"windows"[]`)

Requires `"reporting": {"history_window_ns": N}` (§4.3). Every dispatched
transaction is bucketed by its `issue_cycle` into a fixed-`N`-ns window,
accumulated incrementally — unaffected by `prune_results_before()`, same as
`summary()`.

| field | meaning |
|---|---|
| `window_index`, `start_ns`, `duration_ns` | which window, and its time span |
| `bytes_read`, `bytes_written`, `dram_bytes`, `txn_count` | traffic in this window |
| `hits`, `conflicts`, `empties` | row-status classification in this window |
| `avg_bandwidth_gbps` | `(bytes_read + bytes_written) / duration_ns` |
| `outstanding_high_water` / `outstanding_occupancy_pct` | peak, across every `(core, axi_id)` stream active in the window, of that stream's outstanding-request count vs. `max_outstanding_per_id`. Pinned near 100% means the outstanding cap, not DRAM, is capping throughput there. |
| `active_bank_count` / `bank_utilization_pct` | distinct physical banks touched at least once in the window, out of the topology's total. Low means traffic is landing on only a handful of banks — an address-mapping spread problem, not a timing one. |

`ddrt_get_window_channel_stats(e, window_index, channel_index, &out)`
(fields: `dram_bytes`, `avg_bandwidth_gbps`, `utilization_pct`) gives the
same window's per-channel breakdown — the aggregate fields above can't
distinguish "every channel at 50%" from "one channel maxed, the rest idle."

The JSON `"windows"[]` entries and the `--windowed-csv` output additionally
carry `bankgroup_reuse_count` (count of commands in that window that paid
`tCCD_L` instead of `tCCD_S`) and a per-channel `ch{N}_dram_bytes` /
`ch{N}_avg_bandwidth_gbps` breakdown — both currently exposed via JSON/CSV
only, not yet added to the `ddrt_window_stats_t` C struct.

CSV column order (`--windowed-csv`): `window_index, start_ns, bytes_read,
bytes_written, dram_bytes, txn_count, hits, conflicts, empties,
bankgroup_reuse_count, avg_bandwidth_gbps, outstanding_high_water,
outstanding_occupancy_pct, active_bank_count, bank_utilization_pct`, then
`ch{N}_dram_bytes, ch{N}_avg_bandwidth_gbps` per channel.

View it with `tools/windowed_history_viewer.html` (open directly, `file://`,
no server/build) — synchronized-hover charts for every field above,
degrades gracefully on a CSV missing newer columns, auto-downsamples long
traces.

### 5.4 `windowed_history_analyze` output

A separate tool (§2) that turns a `--windowed-csv` file into statistics for
a caller that can't look at a chart — no config or engine state needed:

```
windowed_history_analyze --csv history.csv [--baseline other_history.csv] [--out report.json]
```

Reports **numbers only, no verdicts** — what counts as "healthy" for e.g.
`bank_utilization_pct` depends on the topology (a 4-bank config reads
nothing like a 32-bank one), so deciding that is left to the caller.

```json
{
  "summary": { "num_windows": 141, "hit_rate_pct": 96.9, "conflict_rate_pct": 1.2,
    "metrics": { "avg_bandwidth_gbps": { "mean": 7.61, "stddev": 3.2,
      "p50": 7.9, "p95": 14.8, "p99": 16.0, "p100": 17.07 },
      "...": "outstanding_occupancy_pct, bank_utilization_pct, conflict_pct, hit_pct, bankgroup_reuse_pct" },
    "channels": [ { "index": 0, "avg_bandwidth_gbps": { "mean": 8.53 } } ] },
  "extremes": { "avg_bandwidth_gbps": {
    "lowest": [ { "window_index": 417, "start_ns": 417000, "end_ns": 418000, "value": 0.0 } ],
    "highest": [ "..." ] } },
  "series": { "bucket_windows": 1, "buckets": [
    { "window_start": 0, "start_ns": 0, "end_ns": 1000000,
      "metrics": { "avg_bandwidth_gbps": { "mean": 17.07, "p50": 17.07,
        "min": { "value": 17.07, "window_index": 0 }, "max": { "value": 17.07, "window_index": 0 } } } } ] },
  "baseline_delta": { "hit_rate_pct": -1.4,
    "metrics": { "avg_bandwidth_gbps": { "target_mean": 11.21, "baseline_mean": 13.74, "delta": -2.53, "delta_pct": -18.4 } } }
}
```

| part | what it is |
|---|---|
| `summary` | whole-trace distribution per metric (mean, stddev, `p0`..`p100` ladder) + per-channel bandwidth |
| `extremes` | the 10 lowest/highest windows per metric, each with its exact `window_index`/`start_ns`/`end_ns` so you can go read that row in the original CSV — never smoothed away by aggregation |
| `series` | a fixed ~150 time buckets regardless of trace length (output size doesn't scale with input size), each with `mean`, `p50`, and the exact `min`/`max` window in that bucket |
| `baseline_delta` (only with `--baseline`) | summary-level `target − baseline` and `delta_pct` per metric, for a before/after comparison |

Metrics available: `avg_bandwidth_gbps` (always), `outstanding_occupancy_pct`
/ `bank_utilization_pct` / `bankgroup_reuse_pct` (only if the CSV has that
column — degrades gracefully on older CSVs), `conflict_pct` / `hit_pct`
(derived from `hits`/`conflicts`/`empties`, always present).

### 5.5 Per-core AXI burst-size distribution (`ddrt_core_burst_stats_t`, JSON `"core_burst_stats"[]`)

One entry per distinct `core_id` seen among pushed transactions (ascending
order), always populated once at least one transaction has been pushed — no
config flag needed, unlike windowed history. Recorded at push time (an
input-stream property, not a scheduling outcome), so it's cumulative since
the engine was created and unaffected by `prune_results_before()`, same as
the summary.

| field | meaning |
|---|---|
| `core_id` | which core this entry describes |
| `txn_count` | transactions pushed by this core so far |
| `total_bytes` | sum of every burst's logical size (`size_bytes × len_beats`) from this core |
| `mean_bytes` | `total_bytes / txn_count` |
| `min_bytes`, `max_bytes` | smallest / largest burst size observed from this core |
| `p25_bytes`, `p50_bytes` (median), `p75_bytes` | quartiles of this core's burst-size distribution |

Percentiles use the **nearest-rank method** (the value at the `⌈pct/100 × count⌉`-th
smallest observation) instead of interpolating between two observed sizes:
burst sizes are naturally few and discrete (a handful of `size_bytes`/`len_beats`
combinations in practice), so every quartile reported here is a size that was
actually sent, never a synthetic in-between number. A core whose `mean_bytes`/
`p50_bytes` is markedly smaller than its peers is paying DDRC/timing overhead
(tRCD/tRP/CAS) over less useful data per command — worth checking before
assuming a low-throughput core is being throttled by the scheduler or the
address mapping.

## 6. Skill Guide for AI Agents

The tool's own design principle carries over to how an agent should use it:
**read the numbers, don't expect a verdict.** Nothing here classifies a run
as "good" or "bad" — thresholds like "healthy bank utilization" depend on
the config's own topology, so an agent reasoning about a run should read the
fields below in the context of *that run's* config, not against a fixed
cutoff.

**Standard pipeline:**

1. `ddrtiming_cli --config cfg.json --validate-only` — free correctness check (no log, no simulation) before iterating on a config. Exit `0`/`1`.
2. `ddrtiming_cli --config cfg.json --log core0.csv [--log core1.csv ...] --out report.json --windowed-csv history.csv`
3. For one run: read `report.json`'s `"summary"` object directly (§5.1) — flat numeric fields, stable names.
4. For anything beyond a single number — trends, outliers, before/after — don't parse `history.csv` directly: run `windowed_history_analyze --csv history.csv [--baseline other.csv] --out analysis.json` (§5.4) and read that. Its output size is bounded regardless of trace length, and `extremes` hands you the exact window to go inspect instead of you having to scan for it.
5. Check `summary.high_address_regions` before trusting any hit/conflict/bandwidth number from step 3 — `>1` means the input trace itself aliases onto overlapping DRAM locations, and every other field describes a workload that doesn't exist (§5.1).

**Reading a lower-than-expected `bandwidth_utilization_pct`** — the fields
below name independent mechanisms the engine actually implements, so they
can be read as a triage order rather than guessed at:

| observation | look at next | indicates |
|---|---|---|
| `outstanding_occupancy_pct` pinned near 100% | — | `max_outstanding_per_id` itself is the limiter, not DRAM |
| `outstanding_occupancy_pct` low | `bank_utilization_pct` | rules out the outstanding cap |
| `bank_utilization_pct` also low | — | address-mapping spread problem: traffic is landing on too few physical banks |
| `bank_utilization_pct` high, `row_conflict_rate_pct` high | — | row-conflict problem: working set revisits rows faster than the mapping spreads them |
| `row_conflict_rate_pct` low, `bankgroup_reuse_rate_pct` high and flat across the trace | — | the bank-group field isn't the fastest-changing bit(s) in `address_mapping` (§4.3) — most commands pay `tCCD_L` instead of `tCCD_S` |
| none of the above stand out | `refresh_overhead_pct` / `turnaround_overhead_pct` | fixed costs of the config itself (`tREFI`/`tRFC`, R↔W turnaround) — not fixable by changing the address mapping |

For a workload longer than a handful of windows, run this triage against
`windowed_history_analyze`'s `summary.metrics` (§5.4) rather than
`report.json`'s single cumulative number — a problem confined to one phase
of a trace can be invisible in the whole-run average.

**A per-core check the table above can't catch**: that triage explains
bandwidth loss the *scheduler and address map* can cause. A core can also be
capped by its own request shape regardless of either — compare
`core_burst_stats[]` (§5.5) across cores. One core with a visibly smaller
`mean_bytes`/`p50_bytes` than its peers is paying the same per-command DDRC
overhead (tRCD/tRP/CAS) over less useful data every time, which limits its
own achievable throughput independent of anything the scheduler or address
mapping do. This is a property of the input trace, not a config choice —
the fix is issuing larger/coalesced bursts upstream of this tool, not
retuning `address_mapping` or `timing_ns`.
