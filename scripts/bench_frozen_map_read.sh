#!/usr/bin/env bash
# Driver for benchmarks/frozen_map_read.lat — the frozen-map read baseline.
#
# Runs the benchmark RUNS times (default 3) with the given clat binary
# (default ./clat, i.e. the stack VM), parses the RESULT lines, and reports
# the per-scenario MEDIAN wall time and derived ns/op, with a frozen-vs-fluid
# ratio column. Also verifies that the semantic checksum of every frozen row
# matches its fluid counterpart.
#
# Usage:
#   scripts/bench_frozen_map_read.sh [path-to-clat] [extra clat flags...]
# Examples:
#   scripts/bench_frozen_map_read.sh                # stack VM (default)
#   scripts/bench_frozen_map_read.sh ./clat --regvm # register VM
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLAT="${1:-$ROOT/clat}"
shift || true
RUNS="${RUNS:-3}"
BENCH="$ROOT/benchmarks/frozen_map_read.lat"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for run in $(seq 1 "$RUNS"); do
    echo "run $run/$RUNS ..." >&2
    "$CLAT" "$@" "$BENCH" | grep '^RESULT' > "$TMP/run$run.txt"
done

python3 - "$TMP" "$RUNS" <<'EOF'
import sys, os, statistics

tmp, runs = sys.argv[1], int(sys.argv[2])
data = {}   # (scenario, size, phase) -> {"ops": int, "ms": [..], "check": set}
order = []
for r in range(1, runs + 1):
    for line in open(os.path.join(tmp, f"run{r}.txt")):
        kv = dict(tok.split("=", 1) for tok in line.split()[1:])
        key = (kv["scenario"], int(kv["size"]), kv["phase"])
        if key not in data:
            data[key] = {"ops": int(kv["ops"]), "ms": [], "check": set()}
            order.append(key)
        data[key]["ms"].append(int(kv["ms"]))
        data[key]["check"].add(kv["check"])

bad = False
for (scen, size, phase), d in data.items():
    if len(d["check"]) != 1:
        print(f"CHECK UNSTABLE: {scen}/{size}/{phase}: {d['check']}"); bad = True
for (scen, size, phase) in order:
    if phase != "fluid":
        continue
    fl, fr = data[(scen, size, "fluid")], data.get((scen, size, "frozen"))
    if fr and fl["check"] != fr["check"]:
        print(f"SEMANTIC MISMATCH {scen}/{size}: fluid={fl['check']} frozen={fr['check']}")
        bad = True
print("checksums: " + ("MISMATCH" if bad else f"all frozen==fluid, stable across {runs} runs"))
print()
hdr = f"{'scenario':<12} {'size':>6} {'ops':>9} | {'fluid med ms':>12} {'ns/op':>8} | {'frozen med ms':>13} {'ns/op':>8} | {'frozen/fluid':>12}"
print(hdr); print("-" * len(hdr))
for (scen, size, phase) in order:
    if phase != "fluid":
        continue
    fl, fr = data[(scen, size, "fluid")], data[(scen, size, "frozen")]
    fm, zm = statistics.median(fl["ms"]), statistics.median(fr["ms"])
    fn_, zn = fm * 1e6 / fl["ops"], zm * 1e6 / fr["ops"]
    print(f"{scen:<12} {size:>6} {fl['ops']:>9} | {fm:>12.0f} {fn_:>8.1f} | {zm:>13.0f} {zn:>8.1f} | {zm/fm if fm else float('nan'):>12.3f}")
EOF
