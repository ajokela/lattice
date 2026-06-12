#!/usr/bin/env python3
"""bench_freeze_gate.py — Crystal-by-Reference Stage 3 fix-binding freeze smoke gate.

Runs benchmarks/fix_freeze_loop.lat (1000 fix bindings of a fresh 10k-element
array) on the DEFAULT stack-VM backend and fails if the self-reported elapsed
time regresses past a generous multiple of the pre-Stage-3 baseline.

This is a SMOKE GATE, not a microbenchmark: the design (crystal-by-reference.md
section 2.8 item 2) accepts that freeze of a shareable container becomes an
O(n) one-time region copy instead of an O(n) tag walk — same complexity class.
The gate exists to catch accidental O(n^2) behavior or per-element rc traffic,
hence the wide headroom.

BASELINE_MS was captured on main @ 6d50d03 (v0.4.1 era, deep-copy crystals on
the stack VM), best of 3 runs on the reference dev machine: 190/194/197 ms.

Usage: python3 scripts/bench_freeze_gate.py [--runs N]
Exit status: 0 = within budget, 1 = regression, 2 = harness error.
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLAT = ROOT / "clat"
BENCH = ROOT / "benchmarks" / "fix_freeze_loop.lat"

# Best-of-3 self-reported elapsed on main @ 6d50d03 (pre-Stage-3 deep-copy VM).
BASELINE_MS = 190

# Generous headroom: the Stage 3 materialization is allowed to cost more than
# the old tag walk, but not an order of magnitude more.
TOLERANCE = 3.0

BUDGET_MS = BASELINE_MS * TOLERANCE

EXPECTED_CHECKSUM = "checksum=499500"


def run_once() -> float:
    out = subprocess.run(
        [str(CLAT), str(BENCH)],
        capture_output=True,
        text=True,
        timeout=120,
        cwd=str(ROOT),
    )
    if out.returncode != 0:
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        sys.exit(2)
    if EXPECTED_CHECKSUM not in out.stdout:
        print(f"FAIL: benchmark checksum wrong (expected {EXPECTED_CHECKSUM}):", file=sys.stderr)
        print(out.stdout, file=sys.stderr)
        sys.exit(2)
    m = re.search(r"elapsed:\s*(\d+(?:\.\d+)?)ms", out.stdout)
    if not m:
        print("FAIL: could not parse elapsed time from benchmark output:", file=sys.stderr)
        print(out.stdout, file=sys.stderr)
        sys.exit(2)
    return float(m.group(1))


def main() -> int:
    runs = 3
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "--runs":
        runs = max(1, int(args[1]))
    if not CLAT.exists():
        print("FAIL: ./clat not built (run make first)", file=sys.stderr)
        return 2

    times = [run_once() for _ in range(runs)]
    best = min(times)
    print(f"fix-binding freeze gate: best of {runs} = {best:.0f}ms "
          f"(all: {', '.join(f'{t:.0f}' for t in times)})")
    print(f"baseline {BASELINE_MS}ms x tolerance {TOLERANCE} = budget {BUDGET_MS:.0f}ms")
    if best > BUDGET_MS:
        print(f"FAIL: fix-binding freeze regressed: {best:.0f}ms > {BUDGET_MS:.0f}ms budget",
              file=sys.stderr)
        return 1
    print("OK: within budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
