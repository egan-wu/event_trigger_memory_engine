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
    --out report.json \
    --windowed-csv history.csv
```

Each `--log` becomes `core_id = 0, 1, 2, ...` in the order given.
`--windowed-csv` requires `"reporting": {"history_window_ns": N}` in the
config — see "Windowed history" below.

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
  clock per channel), `burst_beats` (beats per DRAM burst — DRAM never
  transfers fewer than this per access, so it's what drives over-fetch
  accounting below; default `8` matches DDR4 BL8, use `4` for burst-chop
  (BC4), or whatever reproduces your part's real minimum access granularity),
  `clock_mhz`. Set `clock_mhz` to the DDR speed grade's effective MT/s number
  (e.g. `3200` for DDR4-3200) — this model treats it as a single-pumped
  "effective transfer clock" rather than modeling DDR's double-data-rate
  explicitly, so this is the number that makes
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
- **`reporting`**: `history_window_ns` — bucket size for the windowed
  bandwidth/byte-access history (0/omitted disables it). See "Windowed
  history" below.

## Engine model, briefly

Each AXI burst is split into fixed BL8-style chunks (`data_bus_bytes * 8`
bytes each); each chunk's address is decoded independently, so a burst that
spans multiple banks/rows/channels is handled correctly. Each channel is an
independent physical resource with its own bounded command queue, per-bank
row-buffer tracking (hit/conflict/empty), tRRD/tFAW-gated activates,
tCCD-spaced column commands, R/W bus turnaround, and periodic refresh
insertion. See `src/core/command_queue.cpp` and `src/core/engine.cpp` for the
exact scheduling logic and its comments.

## Report format

Two ways to read results, both backed by the same data: `ddrt_write_report_json()`
(JSON file, what the CLI's `--out` writes) and `ddrt_get_summary()` /
`ddrt_get_result_at()` (the C API's typed structs, `ddrt_summary_t` /
`ddrt_txn_result_t` — identical fields to the JSON, just accessed
programmatically instead of parsed from a file). `summary()`/`ddrt_get_summary()`
is always cumulative since the engine was created, regardless of what's
currently retained in `results()` — see the pruning section below.

### Summary (`ddrt_summary_t`, JSON `"summary"` object)

| field | meaning |
|---|---|
| `total_txns` | count of AXI transactions processed so far |
| `total_bytes` | **logical**: bytes actually requested (`size_bytes * len_beats` summed per txn) |
| `total_dram_bytes` | **physical**: full burst-aligned bytes DRAM actually moved, always `>= total_bytes` — see "Over-fetch" below |
| `total_cycles` | furthest simulated point reached — `max(complete_cycle)` across everything |
| `sim_time_ns` | `total_cycles` converted to ns via the config's clock period |
| `avg_bandwidth_gbps` | `total_bytes / sim_time_ns` — useful throughput actually delivered to the requester |
| `avg_dram_bandwidth_gbps` | `total_dram_bytes / sim_time_ns` — actual DRAM bus traffic, over-fetch included |
| `peak_bandwidth_gbps` | theoretical peak from the config (`data_bus_bytes * clock_mhz/1000 * channels`) — not measured, just the ceiling |
| `bandwidth_utilization_pct` | `avg_dram_bandwidth_gbps / peak_bandwidth_gbps * 100` — physical bus utilization (deliberately the physical number, not the logical one, since that's what the bus itself experiences) |
| `burst_efficiency_pct` | `total_bytes / total_dram_bytes * 100` — 100% = every burst was fully useful, lower = over-fetch waste; see "Over-fetch" below |
| `avg_latency_ns` | mean of every transaction's `(complete_cycle − issue_cycle)`, in ns |
| `page_hit_rate_pct` / `row_conflict_rate_pct` / `row_empty_rate_pct` | classification of every *DRAM column command* (not every transaction — a burst spanning multiple banks/rows contributes multiple classifications); these three sum to 100% |
| `bankgroup_reuse_rate_pct` | % of column commands that paid `tCCD_L` (same bank group as the immediately preceding command on that channel) instead of `tCCD_S` (different bank group) — see "Bank-group ordering" below |
| `refresh_overhead_pct` | % of total channel-cycles (`channels × total_cycles`) spent blocked on refresh |
| `turnaround_overhead_pct` | % of total channel-cycles spent on R↔W bus turnaround |

Not currently broken out: no per-channel or per-bank split — everything above
is summed across all channels/banks into one set of rates. No queue-occupancy
histogram either. Both are in the "explicitly out of scope" list below.

### Per-transaction (`ddrt_txn_result_t`, JSON `"transactions"` array entries)

One entry per AXI transaction (AR/AW — barriers never produce an entry), in
dispatch order (chronological, *not* necessarily push order — see the
per-AXI-ID outstanding discussion above for why independent IDs can complete
out of order relative to each other).

| field | meaning |
|---|---|
| `txn_id` | engine-assigned sequential ID, returned by `push_txn()`/`ddrt_push_txn()` at push time |
| `core_id`, `type` (`AR`/`AW`), `addr` | echoed from the input transaction |
| `bytes` | **logical**: total burst size requested (`size_bytes * len_beats`) |
| `dram_bytes` | **physical**: full burst-aligned bytes DRAM actually moved for this transaction, `>= bytes` — see "Over-fetch" below |
| `issue_cycle` | the cycle the engine determined this could be dispatched, given its outstanding cap, its core's port, and any barrier gate |
| `complete_cycle` | the cycle the last chunk of this burst finished transferring |
| `latency_ns` | `(complete_cycle − issue_cycle)` converted to ns |
| `dominant_row_status` | `hit`/`conflict`/`empty` classification of the burst's *first* DRAM command chunk only |
| `hits`, `conflicts`, `empties` | the same classification, but counted across *every* chunk of this burst — relevant once a burst is large enough to span multiple banks/rows (see "Engine model" above) |

### Over-fetch: `bytes` vs `dram_bytes`

DRAM never transfers less than one full burst (`burst_beats` beats — see
`topology` above) per access, no matter how few bytes you actually asked for.
If an AR/AW's byte range doesn't land on a burst-aligned boundary — because
it's smaller than one burst, or its start address isn't aligned to one — the
DDRC still has to issue a full burst to get it, and everything outside your
requested range comes along for free but wasted. `bytes` is what was
requested; `dram_bytes` is what physically had to move, always `>= bytes`.

Comparing the two (per-transaction, or in aggregate via
`burst_efficiency_pct = total_bytes / total_dram_bytes * 100`) is a direct
health check on how the DMA is issuing its bursts: close to 100% means AXI
requests are landing cleanly on burst-aligned boundaries with little waste;
noticeably below 100% is a real signal to go check the AR/AW sizing and
alignment logic, not a DDRC configuration problem. Concretely, from
`tests/test_engine_basic.cpp`'s `overfetch_metrics_for_undersized_and_misaligned_reads`
(64B bursts, `data_bus_bytes=8` × `burst_beats=8`):

| request | `bytes` | `dram_bytes` | why |
|---|---|---|---|
| 32B, burst-aligned address | 32 | 64 | fits in one burst window, half wasted |
| 64B, address 32B into a window | 64 | 128 | straddles two windows — two full bursts for one logical burst's worth of data |
| 64B, burst-aligned address | 64 | 64 | exactly one window, no waste |

Aggregate efficiency for that mix: `160 / 256 = 62.5%`.

## Windowed history

`summary()` gives one number for the whole run; sometimes you want *when*
performance changed, not just the average — e.g. a bandwidth-over-time chart
that shows exactly which part of a workload dropped into row-conflicts. Set
`"reporting": {"history_window_ns": N}` in the config to turn this on: every
dispatched transaction is bucketed by its `issue_cycle` into a fixed-`N`-ns
window, accumulated incrementally (bytes read/written, DRAM bytes,
hit/conflict/empty counts, transaction count) — the same technique as
`summary()`'s cumulative counters, so windows are **unaffected by
`prune_results_before()`** and cost nothing extra to maintain. Disabled by
default (`history_window_ns: 0`).

A critical point this feature exists specifically to get right: **don't use
however often you happen to call `run()` as your time axis.** The engine's
simulated clock and your wall-clock tick cadence are different clocks that
don't move at a fixed ratio to each other — how much simulated time elapses
between two `run()` calls depends on how much traffic was pushed and how the
DDRC scheduled it, not on how much real time passed. Bucketing by real-world
polling interval produces a distorted chart; bucketing by `issue_cycle` (what
this does) gives you the DDRC's own timeline, which is the one that's
actually meaningful to plot.

`ddrt_get_num_windows()` / `ddrt_get_window_at()` (C API) or `Engine::windows()`
(C++) read it back; the CLI's `--windowed-csv <path>` writes it straight to
CSV for plotting, and `ddrt_write_report_json()` includes a `"windows"` array
automatically whenever windowing is enabled. This library intentionally does
not render charts itself — no third-party dependencies, stays a portable
single binary — it only computes the numbers; plotting is a job for whatever
consumes the CSV/JSON — including `tools/windowed_history_viewer.html`
(below), which is exactly that: a consumer, not part of the library.

Besides bandwidth and row-status, each window also reports two independent
saturation signals, since "why is this slow" can have different answers even
at identical bandwidth:

- **Outstanding occupancy** (`outstanding_high_water` / `outstanding_occupancy_pct`):
  the peak, across every `(core, axi_id)` stream active in the window, of that
  stream's outstanding-request count, sampled exactly at dispatch (occupancy
  only changes at a stream's own dispatch instants, so this is exact, not a
  poll-and-miss sample). Pinned near 100% across a stretch means
  `max_outstanding_per_id`, not DRAM itself, is what's capping throughput
  there.
- **Bank utilization** (`active_bank_count` / `bank_utilization_pct`): how many
  distinct physical banks (post-modulo — the actual `banks_[]` indices
  `ChannelScheduler` schedules against) saw at least one dispatched command in
  the window, out of `channels * ranks_per_channel * bankgroups *
  banks_per_group` total. This is a different failure mode from the other
  two: a workload can be far below both the bus's bandwidth ceiling and the
  outstanding cap and still serialize badly if it's only ever landing on a
  handful of banks — an address-mapping spread problem, not a timing one.
- **Bank-group reuse** (`bankgroup_reuse_count`): of the commands dispatched
  in this window, how many paid `tCCD_L` (same bank group as the immediately
  preceding command *on that channel*) instead of `tCCD_S` (different bank
  group). See "Bank-group ordering" below — this is a third, independent
  failure mode from the two above: a workload can have plenty of bandwidth
  headroom, a healthy outstanding count, and good bank spread, and still run
  at a fraction of peak because its address mapping keeps re-hitting the same
  bank group back to back.

### Bank-group ordering

DDR4/DDR5 splits a rank's banks into bank *groups* specifically so a stream
of column commands can pay the cheap `tCCD_S` spacing (different group —
typically equal to the raw burst transfer time itself, i.e. zero bubble)
instead of the expensive `tCCD_L` spacing (same group — typically ~2x that)
between consecutive commands on a channel. Getting the *value* of the
bank-group field right (spreading traffic across groups at all) is necessary
but not sufficient — the field also has to be the **fastest-changing** bit(s)
among channel/bank/bankgroup for a sequential stream to actually rotate
groups every command. Put it anywhere else (e.g. the slowest-changing of the
interleave bits) and a sequential stream can still spread across every group
in aggregate over time while paying `tCCD_L` on nearly every single command,
because it lingers on each group for several consecutive commands before
moving to the next. This is a real bug this project hit and fixed: moving
`map_bankgroup` from the high bits of a 5-bit interleave field to the low
bits, with everything else unchanged, took one workload from 31% to 72% of
peak bandwidth (12.1%→3.0% row-conflict rate) at the *same* real burst
granularity — no address-hashing, no burst-size changes, no scheduler
changes. `bankgroup_reuse_rate_pct` (summary) and `bankgroup_reuse_count`
(per-window) exist specifically to make this diagnosable in seconds instead
of by re-deriving the bit arithmetic by hand: a value near 0% means the
mapping rotates groups properly; a value that's high *and flat* across the
whole trace (not just in a transient burst) is a structural mapping choice,
not a passing anomaly.

For a multi-channel config, every field above is an **aggregate across all
channels** — which hides a real failure mode of its own: "50% overall
utilization" reads identically whether it's every channel evenly at 50%, or
one channel maxed out and the rest sitting idle. Each window's `channels`
array (JSON) / `ch{N}_dram_bytes` and `ch{N}_avg_bandwidth_gbps` columns
(CSV) — or `ddrt_get_window_channel_stats(engine, window_index, channel_index, &out)`
(C API) — break bandwidth out per physical channel specifically so that
distinction doesn't get lost in the sum. `ddrt_get_num_channels()` gives the
count to iterate. A single-channel config still gets the aggregate fields,
just nothing to compare a lone channel against, so the CLI/viewer skip the
per-channel panel in that case.

### Viewing it: `tools/windowed_history_viewer.html`

A standalone, dependency-free companion page — open it directly in a browser
(`file://`, no server, no build step) and drop in any `--windowed-csv`
output to get the same panels (bandwidth, per-channel bandwidth when there's
more than one channel, row-status, outstanding occupancy, bank utilization)
with a synchronized hover tooltip across all of them. It reads columns by
name from the CSV header, so a CSV from an older version of this tool that's
missing some of the newer columns just renders fewer panels instead of
breaking. For a trace with more rows than fit legibly on screen (tens of
thousands of windows from a long trace), it
automatically downsamples to a fixed number of display buckets — summing the
additive fields (bytes, hits/conflicts/empties) and taking the max of the
"peak" fields (outstanding/bank) across each merged group — and says so
on-screen (`"downsampled to N display buckets"`) rather than silently
reducing resolution. Nothing is uploaded anywhere; it's pure client-side
JS/SVG.

### Analyzing it: `tools/windowed_history_analyze`

The viewer above is for a human looking at a chart. `windowed_history_analyze`
is the same data for a caller that can't look at one — an AI agent, a CI gate,
a script piping into `jq` — anything that needs to *reason about* a trace
without eyeballing it. It's a separate standalone executable (built as part of
this project, not linked into `libddrtiming`): reads a `--windowed-csv` file
alone, no config or engine state needed, and prints a structured statistical
export as JSON.

```
windowed_history_analyze --csv history.csv [--baseline other_history.csv] [--out report.json]
```

**Design principle: numbers, not verdicts.** An earlier version of this tool
scanned for a handful of hardcoded conditions (e.g. "bank utilization below
25%") and emitted pass/fail-style findings with a severity label. Those
thresholds don't generalize — what's a healthy `bank_utilization_pct` on a
4-bank config is nothing like what's healthy on a 32-bank config, so any
fixed cutoff is wrong for someone. This tool now only reports objective
statistics; deciding what counts as a problem, and what to do about it, is
entirely up to the caller.

Output shape:

```json
{
  "summary": {
    "num_windows": 141, "num_channels": 2, "window_ns": 1000000,
    "total_bytes": 123456789, "hit_rate_pct": 96.9, "conflict_rate_pct": 1.2,
    "bankgroup_reuse_rate_pct": 0.1,
    "metrics": {
      "avg_bandwidth_gbps": {
        "mean": 7.61, "stddev": 3.2,
        "p0": 0.0, "p1": 0.4, "p5": 1.1, "p25": 5.0, "p50": 7.9,
        "p75": 10.2, "p95": 14.8, "p99": 16.0, "p100": 17.07
      },
      "outstanding_occupancy_pct": { "mean": 100.0, "...": "..." },
      "bank_utilization_pct": { "mean": 3.15, "...": "..." },
      "conflict_pct": { "mean": 1.2, "...": "..." },
      "hit_pct": { "mean": 96.9, "...": "..." },
      "bankgroup_reuse_pct": { "mean": 0.1, "...": "..." }
    },
    "channels": [
      { "index": 0, "avg_bandwidth_gbps": { "mean": 8.53, "...": "..." } },
      { "index": 1, "avg_bandwidth_gbps": { "mean": 0.6, "...": "..." } }
    ]
  },
  "extremes": {
    "avg_bandwidth_gbps": {
      "lowest": [ { "window_index": 417, "start_ns": 417000, "end_ns": 418000, "value": 0.0 }, "..." ],
      "highest": [ "..." ]
    },
    "channels": [ { "index": 0, "avg_bandwidth_gbps": { "lowest": ["..."], "highest": ["..."] } }, "..." ]
  },
  "series": {
    "bucket_windows": 1,
    "buckets": [
      {
        "window_start": 0, "window_end": 0, "start_ns": 0, "end_ns": 1000000,
        "metrics": {
          "avg_bandwidth_gbps": {
            "mean": 17.07, "p50": 17.07,
            "min": { "value": 17.07, "window_index": 0 },
            "max": { "value": 17.07, "window_index": 0 }
          }
        },
        "channels": [ "..." ]
      }
    ]
  },
  "baseline_delta": {
    "hit_rate_pct": -1.4, "conflict_rate_pct": 0.3,
    "metrics": {
      "avg_bandwidth_gbps": { "target_mean": 11.21, "baseline_mean": 13.74, "delta": -2.53, "delta_pct": -18.4 }
    }
  }
}
```

Four parts:

- **`summary`** — whole-trace distribution per metric (mean, stddev, and a
  `p0`..`p100` percentile ladder), plus per-channel bandwidth distributions.
  Pure descriptive statistics.
- **`extremes`** — the 10 lowest- and 10 highest-value windows per metric,
  each naming its exact `window_index`/`start_ns`/`end_ns` so a caller can go
  read that row straight out of the original CSV. This is what keeps a rare
  single-window anomaly from ever getting smoothed away by aggregation,
  independent of where bucket boundaries happen to fall.
- **`series`** — a fixed number of time buckets (~150) regardless of how many
  windows the trace has, so output size doesn't scale with trace length. Each
  bucket reports `mean` and `p50` (so a caller can tell "a couple of
  outliers" from "the whole bucket shifted") plus the exact `min`/`max`
  window within that bucket — a short burst inside an otherwise-flat bucket
  still shows up instead of being averaged into the mean.
- **`baseline_delta`** (only with `--baseline`) — summary-level comparison
  against a second trace, e.g. the same workload before/after a config
  change: `target_mean - baseline_mean` and `delta_pct` per metric. This is
  summary-only, not per-bucket — the two traces can have different lengths,
  so aligning their bucket boundaries is left unsolved on purpose.

Every metric above runs through the exact same `summary`/`extremes`/`series`
machinery, whether it comes straight from a CSV column or is derived on the
fly:

| metric | meaning | source | always present? |
|---|---|---|---|
| `avg_bandwidth_gbps` | useful bytes/s delivered in that window | CSV column | yes |
| `outstanding_occupancy_pct` | peak in-flight-request occupancy vs `max_outstanding_per_id`, in that window — pinned near 100% means the outstanding cap, not DRAM itself, may be capping throughput there | CSV column | only if the CSV has it |
| `bank_utilization_pct` | % of physical banks touched by at least one command in that window — low means traffic is landing on only a handful of banks, an address-mapping spread problem | CSV column | only if the CSV has it |
| `conflict_pct` / `hit_pct` | % of that window's DRAM column commands classified row-conflict / row-hit | derived per-window from `hits`/`conflicts`/`empties` | yes |
| `bankgroup_reuse_pct` | % of that window's column commands that paid `tCCD_L` (same bank group as the immediately preceding command) instead of the near-free `tCCD_S` — see "Bank-group ordering" above | derived per-window from `bankgroup_reuse_count` | only if the CSV has that column |

`conflict_pct`/`hit_pct` exist so a *trend* across the trace — e.g. conflict
rate climbing across consecutive windows, the signature of a scheduler
greedily deferring one stream's requests behind another's until deferral
itself starts manufacturing conflicts — shows up in `series`/`extremes`
without needing a new engine field; they're arithmetic over columns the CSV
already has. `bankgroup_reuse_pct` needs the engine-side counter (see
"Bank-group ordering" above) since a CSV without it simply doesn't carry the
information.

Like the viewer, it degrades gracefully on an older CSV missing newer columns
(outstanding/bank/bankgroup-reuse/per-channel) — the corresponding metric is
just omitted from `metrics`/`extremes`/`series` rather than the tool failing.

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

### API reference

Every function takes/returns plain C types (no hidden allocation the caller
owns except the engine itself). Convention: functions returning `int` give
`0` on success, `-1` on failure — check `ddrt_last_error(engine)` for why.
Passing a `NULL` engine to any function is handled gracefully (returns `-1` /
`0` / empty rather than crashing), which is what makes `ddrt_last_error(NULL)`
meaningful specifically for a failed `ddrt_create()`.

**Thread safety**: every call on a given `ddrt_engine_t*` is internally
mutex-protected, so it's safe to call from multiple threads without your own
locking — e.g. one OS thread per DMA core, all pushing into the same handle
concurrently, works correctly. This is a coarse per-engine lock (one call
completes before the next starts), not real parallelism inside the engine,
but that costs you nothing: the scheduling algorithm is a single sequential
event-driven simulation regardless, so calls were never going to make
progress concurrently anyway — the lock exists purely to make concurrent
*callers* safe, not to speed anything up. Note it does not make
`ddrt_destroy()` safe to call while another thread might still be calling
into that same handle — no amount of internal locking fixes an object-
lifetime race; make sure every other thread is done with the engine first.

Modeling multiple concurrent DMA cores does **not** require multiple OS
threads in the first place — `AxiTxn::core_id` already models that at the
simulation level (see "Engine model" above). Real OS-level threading is
supported so it doesn't get in your way if that's how your caller happens to
be structured, not because it's the way to represent concurrency here.

| function | returns | does |
|---|---|---|
| `ddrt_create(config_json_path)` | `ddrt_engine_t*`, `NULL` on failure | load a DDRC config, create an engine |
| `ddrt_destroy(engine)` | — | free the engine |
| `ddrt_push_txn(engine, &txn, &out_txn_id)` | `int` | queue one AXI transaction; `out_txn_id` gets the engine-assigned ID |
| `ddrt_push_barrier(engine, core_id)` | `int` | mark a known sync point for this core — see "Barriers" above |
| `ddrt_load_log_file(engine, core_id, path)` | `int` | parse a CSV log (`AR`/`AW`/`BARRIER` rows) and push all of it in one call |
| `ddrt_run(engine)` | `int` | advance the simulation with everything pushed since the last call — safe to call repeatedly; a correct no-op if nothing's new |
| `ddrt_get_summary(engine, &out)` | `int` | cumulative aggregate stats into `out` — see "Report format" above |
| `ddrt_get_num_results(engine)` | `uint64_t` | count of currently-retained per-transaction results |
| `ddrt_get_result_at(engine, index, &out)` | `int` | one per-transaction result by index into `out` — see "Report format" above |
| `ddrt_prune_results_before(engine, max_txn_id)` | `int` | free retained results with `txn_id <= max_txn_id` — see "Feeding it from a long-running DMA model" above |
| `ddrt_get_num_windows(engine)` | `uint64_t` | count of windows in the bandwidth/byte-access history — see "Windowed history" above |
| `ddrt_get_window_at(engine, index, &out)` | `int` | one window's stats by index into `out` |
| `ddrt_get_num_channels(engine)` | `uint64_t` | `topology.channels`, for iterating the next function |
| `ddrt_get_window_channel_stats(engine, window_index, channel_index, &out)` | `int` | one window's per-channel breakdown — see "Windowed history" above |
| `ddrt_write_report_json(engine, out_path)` | `int` | write the full summary + per-transaction report (+ windowed history, if enabled) to a JSON file |
| `ddrt_last_error(engine)` | `const char*` | why the last call on this engine failed; pass `NULL` to read a failed `ddrt_create()`'s error instead |
| `ddrt_version(void)` | `const char*` | library version string |

## Explicitly out of scope (v1)

- Tier-2 SystemC integration/calibration loop
- XOR-hash address interleaving
- Multiple scheduler policies beyond FR-FCFS-lite
- GUI/dashboard reporting (JSON report only)
- Write-strobe affecting timing
