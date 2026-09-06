# bench/ -- regression-verification harness

This directory is a small, fast, deterministic workload corpus plus a
golden-snapshot comparison tool (`tools/golden_check.cpp`, CMake target
`golden_check`). Its job is narrow: make it possible to tell, in seconds,
whether a change to the scheduler or timing model moved a realistic
workload's numbers, by how much, and in which direction -- without anyone
manually re-running the CLI and eyeballing JSON.

**This is not a correctness suite.** The hand-computed unit tests under
`tests/` are what assert the timing model does the right arithmetic on small,
fully-controlled cases. This harness instead answers "did anything change,
and how much" on workloads too large to hand-verify -- see the warning below
about what "golden" does and does not mean here.

## The six workloads

| directory | workload | what it isolates |
|---|---|---|
| `seq_read/` | single core, 8000 sequential 64B reads | page-hit upper bound; sensitive to bank-group ordering |
| `rand_read/` | single core, 8000 uniformly random reads (fixed-seed LCG) | row-conflict lower bound |
| `mixed_rw/` | single core, 8000 strictly-alternating AR/AW, same bank | read/write turnaround and write-recovery paths |
| `multicore_4/` | 4 cores x 2000 sequential streams, each its own bank | cross-stream command-queue/channel-arbitration contention |
| `strided/` | single core, 6000 reads strided by exactly one row's span | pathological address-mapping case: guaranteed conflict, single bank, zero parallelism |
| `bursty/` | single core, 40 barrier-separated bursts of 150 reads each | windowed-history time-axis correctness (dense vs. idle windows) |

Each directory contains:
- `config.json` -- DDRC config (topology/mapping/timing identical across all
  six except `history_window_ns`, tuned per case to land in the 50-200 window
  range, and `bursty/config.json`'s `max_outstanding_per_id`, which is
  deliberately raised -- see `bursty/meta.json`).
- `core*.csv` -- the AXI log(s), assigned `core_id` by filename (`core0.csv`
  is core 0, etc.), matching the CLI's own "each `--log` is assigned
  `core_id` = its position" rule.
- `meta.json` -- what the workload is for, the exact generation parameters,
  and (for `strided`/`bursty`) the address-mapping arithmetic behind the
  numbers, plus the sanity checks that were run before committing the golden.
- `golden.json` -- the committed snapshot `golden_check` compares against.

All six are sized at 6000-8000 transactions specifically so the whole suite
runs in well under a second -- see "Running the check" below.

### Determinism

Every workload is either a pure arithmetic sequence (no randomness at all)
or, for `rand_read`, an explicit fixed-seed 64-bit LCG defined directly in
`bench/generate_workloads.py` (Knuth/MMIX constants, seed `0x5EED1234`) --
deliberately not `std::rand`, not an unseeded/default-seeded engine, and not
Python's own `random` module, none of which guarantee identical output
across platforms or library versions. The LCG's entire specification is the
three constants and the 64-bit wraparound in that one file, so it reproduces
identically anywhere that re-implements them.

## Regenerating the CSVs

```
python bench/generate_workloads.py
```

This rewrites every `bench/<case>/core*.csv` from scratch (config.json,
meta.json, and golden.json are not touched by it). It is **not** part of the
build and `golden_check` does not invoke it -- the committed CSVs are the
actual input the harness runs against, so a change to the generator has no
effect until you re-run it and re-commit the output. Use it if you need to
change a workload's shape (transaction count, address pattern, etc.); it
does not need to be run just to check out this repo.

## Running the check

Built as part of the normal CMake build (`cmake --build build`), and wired
into `ctest` as the `golden_check` test:

```
ctest --test-dir build -R golden_check --output-on-failure
```

or directly:

```
./build/golden_check --bench-root bench
```

Options:
- `--case NAME` (repeatable) -- restrict to specific workloads instead of all
  six.
- `--tolerance PCT` -- relative-tolerance percent for floating-point fields
  (default 0.5).
- `--update-golden` -- rewrite `bench/<case>/golden.json` from the current
  run instead of comparing against it.

### What gets snapshotted, and why

The full `SummaryStats` (see `src/core/engine.hpp`) plus four aggregate
windowed-history statistics: window count, and the mean/min/max of
per-window `avg_bandwidth_gbps`. Per-window arrays are deliberately **not**
snapshotted -- they're large, and brittle in a way that isn't useful:
almost any scheduling change shifts individual window boundaries somewhat
even when the aggregate behavior it produces is completely fine, which would
make the golden fail constantly on noise instead of on real regressions. The
four aggregate numbers still catch a real shape regression (e.g. one channel
starving, or bursts smearing out) without that brittleness.

Two different comparison rules apply, and which one a field gets is a
deliberate choice (see `golden_check.cpp`'s `Field`/classification comment
for the full reasoning):

- **`total_txns`, `total_bytes`, `total_dram_bytes` compare exactly** (any
  difference at all fails). These are structural: they fall out of parsing
  the AXI log against the config's burst/alignment geometry, before any
  scheduling or timing decision happens. A timing-model fix has no path to
  change them -- if one moves, either the log/config changed on purpose, or
  something upstream of the scheduler broke.
- **Everything else** (`total_cycles`, `sim_time_ns`, every rate/bandwidth/
  latency field, and the windowed-history aggregates) **compares with a
  0.5% relative tolerance** (`--tolerance` to change it). These are direct
  functions of the scheduling/timing model and are *expected* to move
  whenever that model changes -- the whole reason this harness exists. The
  tolerance absorbs floating-point summation-order noise, not real drift.

### Reading a failure line

```
FAIL seq_read.avg_dram_bandwidth_gbps: golden 12.4180  actual 15.8820  (+27.90%)
```

Format: `FAIL <case>.<field>: golden <G>  actual <A>  (<signed relative delta>)`.
An exact-match field instead shows `(delta <signed integer>)`; a field whose
golden value is exactly 0 shows an absolute delta instead of a percentage
(a relative delta against zero is undefined). Every FAIL line is
self-contained -- case, field, both numbers, and which direction and by how
much -- specifically so a CI log or a chat message can carry just that one
line and still be useful. Check it against the prediction table below: if
the direction matches a fix that just landed, that's expected; if it
doesn't, or a field moved that shouldn't have been touched by that fix at
all, that's worth a closer look before assuming it's fine.

### `--update-golden`: when it's legitimate vs. when it's papering over a regression

Legitimate:
- A deliberate, reviewed timing-model fix landed, the failing fields moved
  in the direction the prediction table below says they should, and the
  magnitude is plausible for the fix (e.g. a bandwidth change on the order
  of a hit-rate-sized effect, not a 10x swing from a one-line timing tweak).
- The workload itself changed on purpose (regenerated CSVs, edited config)
  and the new numbers are simply the new correct baseline for the new input.

Papering over a regression:
- A field moved that the fix in question has no mechanism to touch (e.g. a
  CAS-latency fix changing `avg_dram_bandwidth_gbps`, which the prediction
  table says it shouldn't -- CAS latency delays completion, not the spacing
  between issued commands).
- A field moved in the *opposite* direction from what the prediction table
  says a landed fix should produce.
- Running `--update-golden` because a test was red and you wanted it green,
  without having looked at *why* it moved.
- Any structural (`total_txns`/`total_bytes`/`total_dram_bytes`) field
  changing when neither the workload's CSV nor its config changed -- there
  is no timing fix that should ever touch these.

If a fix lands and a workload's numbers move the *opposite* way from what
the table below predicts, treat that as a signal that there's an unfound bug
-- possibly in the fix, possibly in this table's own reasoning -- not as a
reason to force the golden to match anyway.

## Known-wrong behavior in the current goldens

**The goldens committed right now capture known-incorrect timing-model
behavior on purpose.** This harness's job is to make *change* visible and
attributable, not to certify these numbers as physically correct DDR
behavior. Specifically, as of this snapshot:

1. **Missing CAS latency.** `command_queue.cpp`'s dispatch path computes
   `complete = col_start + transfer_cycles` with no added CAS latency (tCL)
   between issuing a column command and data actually being ready. Real DRAM
   adds a fixed pipeline delay here; this model currently adds none.
2. **Missing tWTR.** There is no write-to-read turnaround timing anywhere in
   `command_queue.cpp` (`tWTR_S`/`tWTR_L` are parsed from config but never
   referenced) -- a read immediately following a write pays no extra delay
   at all, when real DRAM requires one.
3. **tRTP/tWR over-gate same-bank column commands, including hits.** The
   same-bank recovery gate (`bank.bank_ready_cycle`, set to
   `col_start + tRTP-or-tWR` after every column command) is checked before
   *any* subsequent command to that bank -- including a same-row hit, which
   in real DRAM only needs `tCCD` spacing, not a full read/write-recovery
   wait. Only a *conflict* (a precharge) should actually need tRTP/tWR.
4. **Refresh doesn't close rows.** `apply_refresh_if_due()` blocks time for
   `tRFC` but never touches `bank.row_open`/`bank.open_row` -- a bank can
   still register a page hit against a row that a refresh, in reality, would
   have forced closed.

Do not read these goldens as "this is what a DDR controller does" -- read
them as "this is what this codebase currently computes," which is exactly
what a regression check needs and nothing more.

## Prediction table

How `avg_dram_bandwidth_gbps` and `avg_latency_ns` should move, per
workload, once each of the four bugs above is fixed. This is a verification
instrument, not just documentation: **if a fix lands and a workload moves
the opposite way from its prediction, that means either the fix is wrong or
there's a second, unfound bug interacting with it** -- treat a
contrary-direction move as something to investigate, not something to wave
through with `--update-golden`.

Per-bug effects used below (reasoned from the bug descriptions above):
- **Fix 1 (remove tRTP/tWR over-gate):** only changes the *hit* path (a
  conflict already legitimately needs the recovery wait before precharging)
  -- raises bandwidth / lowers latency specifically where the page-hit rate
  is high, and does nothing where it's ~0%.
  Writes benefit more than reads (`tWR`=15ns vs `tRTP`=7.5ns currently
  over-applied, against a `tCCD_L`=5ns floor once fixed).
- **Fix 2 (add CAS latency):** a fixed delay added to every command's
  completion, independent of hit/conflict/bank -- raises `avg_latency_ns`
  by roughly the same absolute amount everywhere, and does not touch
  `avg_dram_bandwidth_gbps` at all (bandwidth is bytes/sim_time, and
  sim_time is driven by *issue* spacing, which CAS latency doesn't change).
- **Fix 3 (add tWTR):** only changes write-then-read transitions -- lowers
  bandwidth (and raises latency) specifically in workloads where reads
  actually follow writes, and does nothing in a read-only or write-only
  workload.
- **Fix 4 (refresh closes rows):** converts a small number of what are
  currently hits (the first access to a bank right after a refresh) into
  empties -- a small bandwidth decrease and latency increase, sized by how
  much of the workload's traffic falls right after a refresh boundary; does
  nothing where the hit rate is already ~0%, since there's nothing to lose.

| workload | avg_dram_bandwidth_gbps | avg_latency_ns | reasoning |
|---|---|---|---|
| `seq_read` | **up**, clearly | **up**, but partly offset | Fix 1 dominates: 96.9% hit rate, currently gated by tRTP (24 cycles) on every hit when tCCD_L (16 cycles) is the real floor -- that's a ~1/3 reduction in the binding per-command gate on nearly every access. Fix 3 doesn't apply (all reads). Fix 4 shaves a negligible amount off the hit rate. Latency: Fix 2's flat CL addition pushes it up, Fix 1's per-hit savings (~2.5ns each) partially offsets it -- net likely still up, since CL is typically comparable to or larger than that saving. |
| `rand_read` | **flat** (~unchanged) | **up** | 0% hit rate means Fix 1 has nothing to act on (its own conflict path already correctly required the recovery wait). Fix 3 doesn't apply (all reads). Fix 4 has nothing to lose either. Latency moves up by roughly Fix 2's flat CL addition and nothing else. |
| `mixed_rw` | **up, but by less than `seq_read`'s** -- watch this one | **up** | The interesting case: Fix 1 helps a lot here (96.9% hit rate, and writes -- half this workload -- get the larger tWR-vs-tCCD_L saving), but this workload's alternating same-bank R/W pattern means literally every read follows a write, so Fix 3's write-to-read penalty lands on 100% of read transitions here, clawing back some of Fix 1's gain. Net direction is still up (Fix 1's saving is larger per-transition than a plausible tWTR value), but if the actual delta comes out flat or negative, that specifically means Fix 3's tWTR value (or Fix 1's interaction with it) needs a second look -- this workload exists to make that interaction visible. Latency: both Fix 2 (flat) and Fix 3 (write-to-read specific) push it up; Fix 1 offsets partially. |
| `multicore_4` | **up, but by less than `seq_read`'s** | **up** | Same per-stream hit rate (96.85%) as `seq_read`, so Fix 1 still helps -- but this workload's current bottleneck is already closer to the shared channel's tCCD_L spacing than to any single bank's own recovery time (4 banks feed the channel, so one bank's recovery wait is usually papered over by another bank's ready command -- current utilization, 40.8%, sits between the ~33% ceiling a single recovery-bound bank would allow and the ~50% ceiling pure tCCD_L spacing would allow). Fix 1 doesn't touch tCCD_L, so its benefit here is smaller in relative terms than for a lone stream. Latency: up, from Fix 2's flat addition; Fix 1's per-hit savings apply but are individually smaller in effect here for the same reason. |
| `strided` | **flat** (~unchanged) | **up** | 0% hit rate, single bank, no cross-bank parallelism to change the picture -- same reasoning as `rand_read`: Fix 1 and Fix 4 have nothing to act on, Fix 3 doesn't apply (all reads). Latency moves up by Fix 2's flat addition; the relative *size* of that bump is smaller than `seq_read`'s, since `strided`'s baseline latency (382ns) is already much larger. |
| `bursty` | **flat** (~unchanged) | **up** | Shares `strided`'s exact underlying single-bank-conflict address pattern (same `avg_dram_bandwidth_gbps` and `sim_time_ns` today, by construction -- see `bursty/meta.json`) and therefore the same reasoning: no hits to accelerate, no writes for tWTR to touch. Latency moves up from Fix 2 alone. |

A useful cross-check once fixes start landing: `mixed_rw` and `multicore_4`
are the two workloads where the "obvious" prediction (bandwidth up,
following the same-hit-rate story as `seq_read`) is deliberately hedged
above, for two *different* reasons (a competing bug interaction for one, a
different bottleneck resource for the other). If either one instead matches
`seq_read`'s magnitude exactly, that's worth understanding rather than
shrugging off -- it would mean the hedge was wrong, which is itself useful
information about how these fixes actually interact.
