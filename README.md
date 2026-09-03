# ddrtiming — Tier-1 DDR Timing Estimator

A fast, approximate (targeting roughly 70-75% accuracy) DDRC/DRAM performance
estimator. It consumes per-core AXI AR/AW transaction logs (sequence order
only — no real timestamps) and estimates DRAM-level performance (bandwidth,
page-hit/row-conflict rate, latency, refresh/turnaround overhead) by
simulating a simplified DDRC command scheduler and DRAM timing model.

This is "Tier 1" of a two-tier ROI analysis strategy: fast/approximate now,
to be cross-calibrated later against a precise cycle-accurate SystemC
DDRC+DRAM model ("Tier 2" — not part of this tool).

No third-party dependencies: C++17 + CMake + a small hand-rolled JSON
reader/writer. Builds as a static library with a C ABI (`include/ddrtiming/ddrtiming.h`)
plus a CLI (`ddrtiming_cli`), so it can be linked into other simulators
(C/C++, or via Python `ctypes`) or run standalone.

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
```

Produces `build/libddrtiming.a` (or `.lib` with MSVC) and `build/ddrtiming_cli`
(path varies by generator, e.g. `build/Release/` with MSVC).

Run the tests:

```bash
ctest --test-dir build
```

## CLI usage

```bash
./build/ddrtiming_cli --config examples/ddrc_config.example.json \
    --log examples/core0_axi.example.csv \
    --out report.json
```

Each `--log` becomes `core_id = 0, 1, 2, ...` in the order given.

## The core assumption: no timestamps, and what that means

The input log records dispatch *order*, not *time*. The engine reconstructs
timing under an explicit "maximum eagerness" model: a transaction issues as
early as physically permitted by (a) its own AXI-ID's outstanding cap, (b)
its core's shared dispatch port (one command per cycle), and (c) any
`BARRIER` markers you've inserted — see below. If none of those apply, the
engine assumes there was *no* idle gap. This is the single biggest source of
error versus reality, more so than any DDRC timing parameter. If your DMA
model has known idle/dependency points that aren't visible in the AXI stream
itself (waiting on a semaphore, an unrelated compute step, anything not an
AR/AW), express them with a barrier, or the estimate will over-state
achievable concurrency and bandwidth.

## AXI log format (CSV)

One file per core (matches `core_x_axi_package.log` naming). Header row
required:

```
type,id,addr,size,len,wstrb
AR,3,0x80001000,64,8,
AW,1,0x80002000,64,8,FFFFFFFFFFFFFFFF
BARRIER,,,,,
```

| column | meaning |
|---|---|
| `type`  | `AR`, `AW`, or `BARRIER` (barrier rows ignore every other column) |
| `id`    | AXI transaction ID, decimal or `0x`-hex. Same-ID transactions from a core are treated as a strict in-order stream (matching the AXI spec); different IDs are independently outstanding. If your log doesn't track IDs, leave this `0` for everything — it degenerates to one shared FIFO stream per core. |
| `addr`  | byte address, decimal or `0x`-hex |
| `size`  | **literal bytes transferred per beat** (e.g. `64`) — not the raw 3-bit AxSIZE protocol encoding |
| `len`   | **literal beat count** (i.e. AxLEN + 1 already applied) — not raw AxLEN |
| `wstrb` | optional hex byte-string, AW rows only; blank = full-strobe assumed. Parsed and available in the report, but does not currently affect the timing model (a masked write still occupies the same DQ cycles as a full-width write in essentially every real DDRC). |

### Barriers

A `BARRIER` row (or `ddrt_push_barrier(engine, core_id)` in the streaming
API) means: this core's next transaction, on any AXI ID, will not be
scheduled until every transaction the core has already pushed has completed.
It's per-core, not per-ID, since software-level synchronization generally
doesn't reason about AXI IDs. Insert one anywhere your DMA model actually
waits on something before issuing more requests.

## DDRC config (JSON)

See `examples/ddrc_config.example.json` for a full example. Sections:

- **`topology`**: `channels`, `ranks_per_channel`, `bankgroups`,
  `banks_per_group`, `rows`, `columns`, `data_bus_bytes` (bytes moved per
  clock per channel), `clock_mhz`. Set `clock_mhz` to the DDR speed grade's
  effective MT/s number (e.g. `3200` for DDR4-3200) — this model treats it as
  a single-pumped "effective transfer clock" rather than modeling DDR's
  double-data-rate explicitly, so this is the number that makes
  `data_bus_bytes * clock_mhz(MHz)/1000 * channels` line up with the
  datasheet peak GB/s.
- **`address_mapping`**: for each of `channel`, `rank`, `bankgroup`, `bank`,
  `row` — either the contiguous convenience form:
  ```json
  "bank": { "bit_start": 8, "bit_width": 2 }
  ```
  or an explicit scattered/gather form, for mapping tables where a field
  isn't one contiguous span (common for bank-bit interleave):
  ```json
  "bank": { "bits": [14, 15, 10, 11] }
  ```
  meaning field bit0 comes from address bit 14, bit1 from bit 15, bit2 from
  bit 10, bit3 from bit 11 (LSB-first). Any field you omit stays 0 for every
  address. `columns` isn't separately mapped — only `row`/`bank`/etc. matter
  to the timing model; the remaining low address bits are the implicit
  column/byte-offset. **Not supported**: XOR-hash interleaving (only direct
  bit gather/select) — a documented v1 limitation, not a bug.
- **`timing_ns`**: all DRAM timing parameters in nanoseconds (`tRCD`, `tRP`,
  `tRAS`, `tRC`, `tCCD_S/L`, `tRRD_S/L`, `tFAW`, `tWTR_S/L`, `tRTP`, `tWR`,
  `tREFI`, `tRFC`, `rd_wr_turnaround`, `wr_rd_turnaround`). `_S` applies
  between different bank groups, `_L` within the same bank group (standard
  DDR4/5 convention). Everything is nanoseconds, not cycles — a simplification
  versus JEDEC's "greater of N cycles or X ns" rule.
- **`ddrc_resources`**: `command_queue_depth` (per channel, bounds
  backpressure), `max_outstanding_per_id` (see above), `scheduling_policy`
  (currently informational — the engine always uses one FR-FCFS-lite policy).

## Engine model, briefly

Each AXI burst is split into fixed BL8-style chunks (`data_bus_bytes * 8`
bytes each); each chunk's address is decoded independently, so a burst that
spans multiple banks/rows/channels is handled correctly. Each channel is an
independent physical resource with its own bounded command queue, per-bank
row-buffer tracking (hit/conflict/empty), tRRD/tFAW-gated activates,
tCCD-spaced column commands, R/W bus turnaround, and periodic refresh
insertion. See `src/core/command_queue.cpp` and `src/core/engine.cpp` for the
exact scheduling logic and its comments.

## Feeding it from a long-running DMA model (no "end of log")

`run()` is safe to call repeatedly and is cheap when there's nothing new: each
call only processes transactions pushed since the previous call (internal
scheduling state — bank/channel state, per-ID outstanding pools, per-core
port timing, barrier segments — all persist across calls), and `results()` /
`summary()` always reflect everything processed so far. So if your DMA model
is a daemon with no natural completion signal, you don't need one: push as
it runs, and call `run()` periodically as a tick (every N pushed
transactions, every few seconds of wall clock, whatever's convenient) to get
an always-up-to-date running report. Calling it with nothing new pushed is a
correct no-op. This is exactly the batch/CLI usage too, just with one `run()`
call instead of many — it's the same code path either way.

Memory over a long-running session: a dispatched transaction is freed the
moment it's processed (it's never copied into a separate log store), so the
backlog only holds whatever's genuinely still in flight or queued. The one
thing that *does* accumulate forever is `results()` (the per-transaction
report). Bound it with `prune_results_before(max_txn_id)` /
`ddrt_prune_results_before()`: drain `results()`, report it wherever it
needs to go, then prune up through the highest `txn_id` you just reported.
There's no way to recover a pruned result, so only prune what you've
actually consumed. `summary()` is deliberately tracked with separate
cumulative counters, not by rescanning `results()`, so it stays correct
(cumulative since the engine was created) no matter how aggressively you
prune -- pruning never has to trade off against the running report.

```c
for (;;) {
    ddrt_run(engine); /* advance with whatever's been pushed since last tick */

    uint64_t n = ddrt_get_num_results(engine), max_id = 0;
    for (uint64_t i = 0; i < n; ++i) {
        ddrt_txn_result_t r;
        ddrt_get_result_at(engine, i, &r);
        report(r); /* your dashboard/log/whatever */
        if (r.txn_id > max_id) max_id = r.txn_id;
    }
    if (n > 0) ddrt_prune_results_before(engine, max_id); /* only after report() succeeded */

    sleep(interval);
}
```

### Example: strictly-sequential commands (one barrier per command)

A common shape: your DMA model issues discrete "commands," each of which
fires several AXI packages that *are* meant to be outstanding together, but
successive commands are strictly sequential — command N+1's first AXI
package is never actually issued in real hardware until command N has fully
completed (e.g. gated by a completion/doorbell mechanism), not just
independent work that happens to be tracked separately. In that case, tie a
barrier to every command boundary — reusing the same AXI ID across commands
is fine, since a barrier already gives the next command's transactions a
fresh, isolated outstanding pool on top of enforcing the actual wait:

```c
/* at the end of each command */
for (/* each AXI package belonging to this command */) {
    ddrt_push_txn(engine, &t, &txn_id); /* axi_id can be reused across commands */
}
ddrt_push_barrier(engine, core_id);     /* enforces "wait for this command to finish" */
ddrt_run(engine);                       /* tick: fully schedule this command */

uint64_t n = ddrt_get_num_results(engine), max_id = 0;
for (uint64_t i = 0; i < n; ++i) {
    ddrt_txn_result_t r;
    ddrt_get_result_at(engine, i, &r);
    report(r);
    if (r.txn_id > max_id) max_id = r.txn_id;
}
if (n > 0) ddrt_prune_results_before(engine, max_id);
```

Without the barrier, the engine's default "maximum eagerness" assumption
would let the next command's packages start dispatching as soon as the
outstanding cap/port allow — over-stating concurrency and bandwidth for a
DMA model where that overlap can't actually happen. `examples/daemon_demo.c`
exercises exactly this pattern (see its "Tick 2"), including a barrier mid-run.

## C API

`include/ddrtiming/ddrtiming.h` — opaque `ddrt_engine_t*` handle:

```c
ddrt_engine_t* e = ddrt_create("ddrc_config.json");

ddrt_axi_txn_t t = {
    .core_id = 0, .type = DDRT_READ, .axi_id = 3,
    .addr = 0x80001000, .size_bytes = 64, .len_beats = 8,
    .wstrb = NULL, .wstrb_len = 0,
};
uint64_t txn_id;
ddrt_push_txn(e, &t, &txn_id);
ddrt_push_barrier(e, /*core_id=*/0);
/* ... push more, in the same relative order the DMA actually issued them ... */

ddrt_run(e);

ddrt_summary_t s;
ddrt_get_summary(e, &s);
printf("avg bandwidth: %.2f GB/s (%.1f%% of peak)\n",
       s.avg_bandwidth_gbps, s.bandwidth_utilization_pct);

ddrt_write_report_json(e, "report.json");
ddrt_destroy(e);
```

`ddrt_prune_results_before(e, max_txn_id)` bounds memory for a long-running
caller -- see "Feeding it from a long-running DMA model" above.

`ddrt_load_log_file(e, core_id, path)` parses+pushes a CSV log file in one
call (what the CLI uses internally) if you'd rather not drive it
transaction-by-transaction.

## Explicitly out of scope (v1)

- Tier-2 SystemC integration/calibration loop
- XOR-hash address interleaving
- Multiple scheduler policies beyond FR-FCFS-lite
- GUI/dashboard reporting (JSON report only)
- Write-strobe affecting timing
