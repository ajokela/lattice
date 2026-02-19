#!/usr/bin/env python3
"""Lattice CI benchmark runner.

Pulls latest source, builds, runs benchmarks (tree-walker + bytecode VM),
and stores results in PostgreSQL. Designed to run via cron every 6 hours
on the build server.

Usage:
    python3 bench/run_benchmarks_ci.py
"""

import os
import re
import subprocess
import sys
import time
import statistics
import platform

import psycopg2

# ── Configuration ──────────────────────────────────────────────────────────────

REPO_DIR = os.path.expanduser("~/projects/lattice")
CLAT = os.path.join(REPO_DIR, "clat")
BENCH_DIR = os.path.join(REPO_DIR, "bench")
RUNS = 3

DB_HOST = "database.apidata.red"
DB_PORT = 5432
DB_NAME = "marketsdata"
DB_USER = "markets"
DB_PASS = "R3a7dVXTVRFZSz8EgAkWRzsxRpVXzs"

BENCHMARKS = [
    ("fib_recursive",   "Function calls"),
    ("nested_loops",    "Iteration"),
    ("sieve",           "Array operations"),
    ("map_operations",  "Map operations"),
    ("string_concat",   "String operations"),
    ("closures",        "Closures"),
    ("scope_spawn",     "Scope/spawn"),
    ("select_channels", "Select channels"),
]

# ── Helpers ────────────────────────────────────────────────────────────────────

def run(cmd, cwd=None):
    """Run a shell command and return stdout."""
    result = subprocess.run(cmd, shell=True, cwd=cwd,
                            capture_output=True, text=True)
    if result.returncode != 0:
        print(f"FAIL: {cmd}\n{result.stderr}", file=sys.stderr)
        sys.exit(1)
    return result.stdout.strip()


def get_commit_hash():
    return run("git rev-parse --short HEAD", cwd=REPO_DIR)


def get_version():
    header = os.path.join(REPO_DIR, "include", "lattice.h")
    with open(header) as f:
        for line in f:
            m = re.search(r'#define\s+LATTICE_VERSION\s+"([^"]+)"', line)
            if m:
                return m.group(1)
    return "unknown"


def time_run(cmd):
    """Run a command and return elapsed time in milliseconds."""
    start = time.perf_counter()
    subprocess.run(cmd, shell=True, capture_output=True)
    elapsed = (time.perf_counter() - start) * 1000
    return round(elapsed, 2)


def benchmark_file(name):
    """Run a single benchmark 3x in both modes, return (tw_median, bc_median)."""
    path = os.path.join(BENCH_DIR, f"{name}.lat")
    if not os.path.exists(path):
        print(f"  SKIP {name}: file not found", file=sys.stderr)
        return None, None

    tw_times = []
    bc_times = []
    for _ in range(RUNS):
        tw_times.append(time_run(f"{CLAT} --tree-walk '{path}'"))
        bc_times.append(time_run(f"{CLAT} '{path}'"))

    return statistics.median(tw_times), statistics.median(bc_times)


def get_platform_info():
    uname = platform.uname()
    return f"{uname.system} {uname.machine}"


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    print("=== Lattice Benchmark Runner ===")

    # Pull latest and rebuild
    print("Pulling latest source...")
    run("git pull", cwd=REPO_DIR)

    print("Building...")
    run("make clean && make", cwd=REPO_DIR)

    commit = get_commit_hash()
    version = get_version()
    plat = get_platform_info()
    print(f"Commit: {commit}  Version: {version}  Platform: {plat}")

    # Connect to database
    conn = psycopg2.connect(
        host=DB_HOST, port=DB_PORT, dbname=DB_NAME,
        user=DB_USER, password=DB_PASS, sslmode="require"
    )
    cur = conn.cursor()

    inserted = 0
    skipped = 0

    for name, label in BENCHMARKS:
        print(f"  Running: {label}...")
        tw_ms, bc_ms = benchmark_file(name)
        if tw_ms is None:
            continue

        speedup = round(tw_ms / bc_ms, 2) if bc_ms > 0 else 0.0
        print(f"    TW={tw_ms}ms  BC={bc_ms}ms  Speedup={speedup}x")

        cur.execute("""
            INSERT INTO lattice_benchmarks
                (commit_hash, version, benchmark, tw_ms, bc_ms, speedup, platform)
            VALUES (%s, %s, %s, %s, %s, %s, %s)
            ON CONFLICT (commit_hash, benchmark) DO NOTHING
        """, (commit, version, label, tw_ms, bc_ms, speedup, plat))

        if cur.rowcount > 0:
            inserted += 1
        else:
            skipped += 1

    conn.commit()
    cur.close()
    conn.close()

    print(f"Done: {inserted} inserted, {skipped} skipped (already recorded)")


if __name__ == "__main__":
    main()
