#!/usr/bin/env python3
"""Generates the bench/ workload corpus committed alongside this script.

Regeneration is optional -- the committed CSVs/configs under bench/<name>/
are what the harness (tools/golden_check) actually reads, and this script is
not invoked by any build step. It exists so the workloads are reproducible
and auditable rather than hand-edited CSV blobs of unknown provenance.

Determinism: the only workload with any randomness is rand_read, and it uses
an explicit fixed-seed 64-bit LCG (Knuth/MMIX constants) defined right here,
not std::rand/std::mt19937/Python's random module -- those are free to change
bit-for-bit output across versions or platforms, which would silently
invalidate every golden snapshot taken against them. This LCG's arithmetic
is nailed down by this file alone, so its output is stable forever.
"""
import csv
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


class Lcg:
    """64-bit LCG, Knuth/MMIX constants: state' = (A*state + C) mod 2**64.

    Not cryptographic, not even statistically great -- just fixed,
    documented, and identical on every platform/language that implements
    the same three constants and 64-bit wraparound.
    """
    A = 6364136223846793005
    C = 1442695040888963407
    MASK = (1 << 64) - 1

    def __init__(self, seed):
        self.state = seed & self.MASK

    def next_u64(self):
        self.state = (self.A * self.state + self.C) & self.MASK
        return self.state

    def next_uniform_below(self, n):
        # Top bits are the higher-quality ones for a linear congruential
        # generator (low bits have short periods in the low-order structure),
        # so draw from state >> 32 rather than state directly.
        return (self.next_u64() >> 32) % n


def write_csv(path, rows):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["type", "id", "addr", "size", "len", "wstrb"])
        for row in rows:
            w.writerow(row)


def hexaddr(a):
    return f"0x{a:X}"


# ---------------------------------------------------------------------------
# seq_read: single core, purely sequential 64B reads.
# ---------------------------------------------------------------------------
def gen_seq_read():
    n = 8000
    base = 0x1000_0000
    size = 64
    rows = []
    for i in range(n):
        addr = base + i * size
        rows.append(["AR", 0, hexaddr(addr), size, 1, ""])
    write_csv(os.path.join(SCRIPT_DIR, "seq_read", "core0.csv"), rows)
    return n


# ---------------------------------------------------------------------------
# rand_read: single core, uniformly random addresses (fixed-seed LCG).
# ---------------------------------------------------------------------------
def gen_rand_read():
    n = 8000
    size = 64
    seed = 0x5EED_1234
    # Full span reachable by the config's row/bankgroup/bank mapping (bits
    # 11..30 inclusive -- see meta.json) so addresses actually spread across
    # every bank, not just bank 0.
    span = 1 << 31
    rng = Lcg(seed)
    rows = []
    for _ in range(n):
        raw = rng.next_uniform_below(span)
        addr = raw & ~(size - 1)  # align to the 64B beat we claim to request
        rows.append(["AR", 0, hexaddr(addr), size, 1, ""])
    write_csv(os.path.join(SCRIPT_DIR, "rand_read", "core0.csv"), rows)
    return n, seed, span


# ---------------------------------------------------------------------------
# mixed_rw: single core, strictly alternating AR/AW at sequential addresses,
# same bank (deliberately -- see meta.json). Tried routing reads/writes to
# separate banks first, to isolate rd_wr_turnaround/wr_rd_turnaround from
# same-bank recovery; it made no difference (turnaround_overhead_pct stayed
# 0% either way, see meta.json for why), so this keeps the simpler
# single-stream layout, which at least directly exercises tWR write-recovery
# gating on a page-hit streak -- the same pattern the tRTP/tWR over-gate bug
# affects most (see bench/README.md's prediction table).
# ---------------------------------------------------------------------------
def gen_mixed_rw():
    n = 8000
    base = 0x2000_0000
    size = 64
    rows = []
    for i in range(n):
        addr = base + i * size
        if i % 2 == 0:
            rows.append(["AR", 0, hexaddr(addr), size, 1, ""])
        else:
            rows.append(["AW", 1, hexaddr(addr), size, 1, "FFFFFFFFFFFFFFFF"])
    write_csv(os.path.join(SCRIPT_DIR, "mixed_rw", "core0.csv"), rows)
    return n


# ---------------------------------------------------------------------------
# multicore_4: 4 cores, each a sequential stream, base addresses chosen so
# each core lands in its own bank (see meta.json for the bit arithmetic) --
# isolates command-queue/channel arbitration contention, not bank conflicts.
# ---------------------------------------------------------------------------
def gen_multicore_4():
    per_core = 2000
    size = 64
    bank_stride = 1 << 29  # see meta.json: bit 29 is the LSB of the bank field
    for core in range(4):
        base = core * bank_stride
        rows = []
        for i in range(per_core):
            addr = base + i * size
            rows.append(["AR", 0, hexaddr(addr), size, 1, ""])
        write_csv(os.path.join(SCRIPT_DIR, "multicore_4", f"core{core}.csv"), rows)
    return per_core * 4


# ---------------------------------------------------------------------------
# strided: single core, stride == one row's byte span (see meta.json) so
# every access opens a brand-new row in the same bank -- worst-case conflict
# pattern that a purely-random workload wouldn't reliably hit every time.
# ---------------------------------------------------------------------------
def gen_strided():
    n = 6000
    base = 0x3000_0000
    size = 64
    stride = 1 << 11  # 2048 bytes -- one row's span, see meta.json arithmetic
    rows = []
    for i in range(n):
        addr = base + i * stride
        rows.append(["AR", 0, hexaddr(addr), size, 1, ""])
    write_csv(os.path.join(SCRIPT_DIR, "strided", "core0.csv"), rows)
    return n


# ---------------------------------------------------------------------------
# bursty: single core, dense bursts separated by BARRIERs, engineered so each
# burst issues almost instantaneously (in simulated time) but takes far
# longer to actually drain -- which is what turns a BARRIER's "wait for full
# drain" gate (engine.hpp Segment::gate_cycle) into a real, multi-window gap
# in issue-time, since the engine has no timestamps and otherwise always
# issues as early as physically possible with none.
#
# Two things make a burst issue fast but drain slow:
#  - max_outstanding_per_id is set (in this workload's own config.json only)
#    to comfortably exceed per_phase, so this id's outstanding cap never
#    gates issuance -- every txn in the burst is admitted back-to-back at the
#    engine's minimum issue spacing (1 cycle/txn -- see engine.cpp
#    kMinIssueSpacingCycles), regardless of how slowly the channel actually
#    drains them.
#  - each burst strides by one row's span (see strided/meta.json for the
#    2048-byte arithmetic) within the SAME bank, so every access in the
#    burst is a row conflict that must fully serialize on that one bank --
#    the burst's completion time is therefore much larger than its (nearly
#    zero) issue-time footprint.
# The result: issue_cycle for a whole burst spans only ~per_phase cycles,
# but the segment's max_complete (what the next BARRIER gates on) lands far
# later -- so the windows in between see zero dispatched bytes.
# ---------------------------------------------------------------------------
def gen_bursty():
    phases = 40
    per_phase = 150
    base = 0x4000_0000
    size = 64
    stride = 1 << 11  # one row's span -- forces a conflict on every access
    rows = []
    for p in range(phases):
        addr = base + p * stride * per_phase  # phases don't overlap rows
        for _i in range(per_phase):
            rows.append(["AR", 0, hexaddr(addr), size, 1, ""])
            addr += stride
        rows.append(["BARRIER", "", "", "", "", ""])
    write_csv(os.path.join(SCRIPT_DIR, "bursty", "core0.csv"), rows)
    return phases * per_phase, phases


if __name__ == "__main__":
    n_seq = gen_seq_read()
    n_rand, seed, span = gen_rand_read()
    n_mixed = gen_mixed_rw()
    n_multi = gen_multicore_4()
    n_strided = gen_strided()
    n_bursty, phases = gen_bursty()
    print(f"seq_read:     {n_seq} txns")
    print(f"rand_read:    {n_rand} txns (seed={hex(seed)}, span={span})")
    print(f"mixed_rw:     {n_mixed} txns")
    print(f"multicore_4:  {n_multi} txns (4 cores)")
    print(f"strided:      {n_strided} txns")
    print(f"bursty:       {n_bursty} txns ({phases} phases)")
