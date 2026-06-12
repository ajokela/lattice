#!/usr/bin/env python3
"""bench_cbr.py — Crystal-by-Reference Stage 6 (LAT-459) benchmark suite.

Runs the benchmarks/cbr_*.lat workloads (plus the Stage 3 freeze-loop worst
case) on ALL THREE backends (stack VM, tree-walker, register VM), in normal
(shared) mode and under the LATTICE_FORCE_COPY=1 differential oracle, and
writes a markdown report to benchmarks/RESULTS.md.

Workloads:
  read      — iterate one frozen 100k array (sharing must be ~neutral)
  alias     — 20k aliases of a frozen 10k array (the headline win)
  arg-pass  — 100k calls passing a frozen 20-key map (clone vs retain)
  channel   — producer/consumer streaming 5k frozen 1k payloads
  spawn     — 32 spawns each reading one frozen 10k dataset
  freeze-loop — fix-freeze of a fresh 10k container per iteration (cost case)

Threshold sweep (--sweep / default on):
  benchmarks/cbr_threshold_sweep.lat — freeze+alias+read cycles for strings
  8..256 B and arrays 0..64 elements at two alias ratios, used to validate
  REGION_SHARE_MIN_STR_LEN (src/value.c) and the always-regionize container
  policy.

Cutoff variants (optional):
  --variant LABEL=PATH may be repeated; each PATH is a clat binary built with
  a different REGION_SHARE_MIN_STR_LEN. The string sweep is re-run on each
  (stack VM, shared mode) and reported side by side.

All times are the benchmarks' self-reported elapsed (ms, via Lattice time()),
never shell time. Best-of-N (default 3) per cell.

Usage: python3 scripts/bench_cbr.py [--runs N] [--no-sweep] [--out PATH]
                                    [--variant LABEL=PATH]...
Exit status: 0 = ok, 2 = harness/checksum error.
"""

import os
import platform
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLAT = ROOT / "clat"
OUT = ROOT / "benchmarks" / "RESULTS.md"

WORKLOADS = [
    ("read", "cbr_read_throughput.lat", "checksum=4999950000"),
    ("alias", "cbr_alias_throughput.lat", "checksum=99990000"),
    ("arg-pass", "cbr_arg_pass.lat", "checksum=950000"),
    ("channel", "cbr_channel_throughput.lat", "checksum=2497500"),
    ("spawn", "cbr_spawn_scaling.lat", "checksum=1599840000"),
    ("freeze-loop", "fix_freeze_loop.lat", "checksum=499500"),
]

# Cells slower than this run only once (the slow force-copy oracle cells are
# minutes long on the tree-walker; repeat noise is irrelevant at that scale).
ADAPTIVE_SINGLE_RUN_MS = 2000.0

BACKENDS = [
    ("stack-vm", []),
    ("tree-walk", ["--tree-walk"]),
    ("regvm", ["--regvm"]),
]

MODES = [
    ("shared", {}),
    ("force-copy", {"LATTICE_FORCE_COPY": "1"}),
]

SWEEP_BENCH = "cbr_threshold_sweep.lat"
SWEEP_LINE = re.compile(
    r"sweep (str size|arr n)=(\d+) aliases=(\d+) elapsed=(\d+(?:\.\d+)?)ms"
)


def run(clat, flags, bench, extra_env, timeout=600):
    env = dict(os.environ)
    env.update(extra_env)
    out = subprocess.run(
        [str(clat)] + flags + [str(ROOT / "benchmarks" / bench)],
        capture_output=True, text=True, timeout=timeout, cwd=str(ROOT), env=env,
    )
    if out.returncode != 0:
        print(f"FAIL: {bench} {flags} {extra_env}:", file=sys.stderr)
        print(out.stdout, file=sys.stderr)
        print(out.stderr, file=sys.stderr)
        sys.exit(2)
    return out.stdout


def run_workload(clat, flags, bench, checksum, extra_env, runs):
    times = []
    for _ in range(runs):
        stdout = run(clat, flags, bench, extra_env)
        if checksum not in stdout:
            print(f"FAIL: {bench} {flags} {extra_env}: bad checksum "
                  f"(expected {checksum}):\n{stdout}", file=sys.stderr)
            sys.exit(2)
        m = re.search(r"elapsed:\s*(\d+(?:\.\d+)?)ms", stdout)
        if not m:
            print(f"FAIL: {bench}: no elapsed line:\n{stdout}", file=sys.stderr)
            sys.exit(2)
        times.append(float(m.group(1)))
        if times[-1] > ADAPTIVE_SINGLE_RUN_MS:
            break
    return min(times)


def run_sweep(clat, flags, extra_env, runs):
    """Returns {(kind, size, aliases): best_ms}."""
    best = {}
    for _ in range(runs):
        stdout = run(clat, flags, SWEEP_BENCH, extra_env)
        if "sweep done" not in stdout:
            print(f"FAIL: sweep incomplete:\n{stdout}", file=sys.stderr)
            sys.exit(2)
        for m in SWEEP_LINE.finditer(stdout):
            kind = "str" if m.group(1).startswith("str") else "arr"
            key = (kind, int(m.group(2)), int(m.group(3)))
            t = float(m.group(4))
            if key not in best or t < best[key]:
                best[key] = t
    return best


def machine_info():
    info = [f"{platform.system()} {platform.release()} ({platform.machine()})"]
    if sys.platform == "darwin":
        try:
            cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                                 capture_output=True, text=True).stdout.strip()
            mem = subprocess.run(["sysctl", "-n", "hw.memsize"],
                                 capture_output=True, text=True).stdout.strip()
            info.append(cpu)
            if mem.isdigit():
                info.append(f"{int(mem) // (1024 ** 3)} GB RAM")
        except OSError:
            pass
    return ", ".join(x for x in info if x)


def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True,
                          text=True, cwd=str(ROOT)).stdout.strip()


def fmt_ratio(shared, fc):
    if shared <= 0:
        shared = 1.0  # 1 ms clock-resolution floor: ratio is a lower bound
        return f"{fc / shared:.1f}x (lower bound)"
    return f"{fc / shared:.1f}x"


def main():
    runs = 3
    sweep_runs = 2
    do_sweep = True
    out_path = OUT
    variants = []
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--runs":
            runs = max(1, int(args[i + 1])); i += 2
        elif a == "--no-sweep":
            do_sweep = False; i += 1
        elif a == "--out":
            out_path = Path(args[i + 1]); i += 2
        elif a == "--variant":
            label, _, path = args[i + 1].partition("=")
            variants.append((label, Path(path))); i += 2
        else:
            print(f"unknown arg: {a}", file=sys.stderr)
            return 2
    if not CLAT.exists():
        print("FAIL: ./clat not built (run make first)", file=sys.stderr)
        return 2

    lines = []
    lines.append("# Crystal-by-Reference Stage 6 benchmark results (LAT-459)")
    lines.append("")
    lines.append(f"- Machine: {machine_info()}")
    lines.append(f"- Commit: {git('rev-parse', '--short', 'HEAD')} "
                 f"({git('log', '-1', '--format=%cd')})")
    lines.append(f"- Best of {runs} runs per cell; times are each benchmark's "
                 f"self-reported elapsed (Lattice `time()`, ms resolution).")
    lines.append(f"- `shared` = normal crystal-by-reference; `force-copy` = "
                 f"`LATTICE_FORCE_COPY=1` differential oracle (every alias is "
                 f"a deep clone). Shared cells at 0–1 ms are at clock "
                 f"resolution, so those ratios are lower bounds.")
    lines.append("")
    lines.append("## Main suite")
    lines.append("")
    lines.append("| workload | backend | shared (ms) | force-copy (ms) | speedup |")
    lines.append("|---|---|---:|---:|---:|")

    for wname, bench, checksum in WORKLOADS:
        for bname, flags in BACKENDS:
            cell = {}
            for mname, env in MODES:
                cell[mname] = run_workload(CLAT, flags, bench, checksum, env, runs)
                print(f"  {wname:12s} {bname:10s} {mname:10s} "
                      f"{cell[mname]:8.0f} ms", flush=True)
            lines.append(f"| {wname} | {bname} | {cell['shared']:.0f} | "
                         f"{cell['force-copy']:.0f} | "
                         f"{fmt_ratio(cell['shared'], cell['force-copy'])} |")

    if do_sweep:
        lines.append("")
        lines.append("## Threshold sweep (`value_worth_regionizing`, src/value.c)")
        lines.append("")
        lines.append("20k cycles at 8 aliases / 5k cycles at 64 aliases; each cycle = "
                     "clone fresh value + `fix` (freeze) + N aliased reads. "
                     "String cutoff `REGION_SHARE_MIN_STR_LEN` = 32; arrays "
                     "always regionize (no element-count cutoff exists).")
        sweeps = {}
        for bname, flags in BACKENDS:
            for mname, env in MODES:
                print(f"  sweep {bname} {mname}...", flush=True)
                sweeps[(bname, mname)] = run_sweep(CLAT, flags, env, sweep_runs)
        for kind, label, sizes in [
            ("str", "Strings (bytes)", [8, 16, 24, 32, 40, 48, 64, 128, 256]),
            ("arr", "Arrays (elements)", [0, 1, 2, 4, 8, 16, 32, 64]),
        ]:
            lines.append("")
            lines.append(f"### {label}")
            lines.append("")
            header = "| size | aliases |"
            sep = "|---:|---:|"
            for bname, _ in BACKENDS:
                header += f" {bname} shared | {bname} force-copy |"
                sep += "---:|---:|"
            lines.append(header)
            lines.append(sep)
            for aliases in (8, 64):
                for size in sizes:
                    row = f"| {size} | {aliases} |"
                    for bname, _ in BACKENDS:
                        for mname, _ in MODES:
                            t = sweeps[(bname, mname)].get((kind, size, aliases))
                            row += f" {t:.0f} |" if t is not None else " n/a |"
                    lines.append(row)

    if variants:
        lines.append("")
        lines.append("## String-cutoff variants (stack VM, shared mode)")
        lines.append("")
        lines.append("Same sweep re-run on binaries built with different "
                     "`REGION_SHARE_MIN_STR_LEN` values.")
        lines.append("")
        vsweeps = []
        for label, path in variants:
            if not path.exists():
                print(f"FAIL: variant binary missing: {path}", file=sys.stderr)
                return 2
            print(f"  variant {label}...", flush=True)
            vsweeps.append((label, run_sweep(path, [], {}, sweep_runs)))
        header = "| size | aliases |"
        sep = "|---:|---:|"
        for label, _ in vsweeps:
            header += f" cutoff={label} |"
            sep += "---:|"
        lines.append(header)
        lines.append(sep)
        for aliases in (8, 64):
            for size in [8, 16, 24, 32, 40, 48, 64, 128, 256]:
                row = f"| {size} | {aliases} |"
                for _, sw in vsweeps:
                    t = sw.get(("str", size, aliases))
                    row += f" {t:.0f} |" if t is not None else " n/a |"
                lines.append(row)

    lines.append("")
    out_path.write_text("\n".join(lines) + "\n")
    print(f"\nwrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
