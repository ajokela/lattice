#!/usr/bin/env bash
# Frozen-vs-fluid map read benchmark harness.
#
# Runs benchmarks/frozen_map_read.lat RUNS times (default 3) with the given
# binary/flags, takes the per-scenario MEDIAN ms, derives ns/op, prints a
# frozen/fluid ratio column, and verifies that every frozen checksum equals
# its fluid counterpart across all runs.
#
# Usage:
#   scripts/bench_frozen_map_read.sh [binary] [extra-flags...]
#   RUNS=5 scripts/bench_frozen_map_read.sh ./clat --regvm
set -euo pipefail

BIN="${1:-./clat}"
shift || true
RUNS="${RUNS:-3}"
BENCH="benchmarks/frozen_map_read.lat"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for r in $(seq 1 "$RUNS"); do
    echo "run $r/$RUNS..." >&2
    "$BIN" "$@" "$BENCH" | grep '^RESULT' > "$TMP/run$r.txt"
done

awk -v runs="$RUNS" -v dir="$TMP" '
function median(arr, n,   i, j, t) {
    for (i = 1; i < n; i++)
        for (j = i + 1; j <= n; j++)
            if (arr[j] < arr[i]) { t = arr[i]; arr[i] = arr[j]; arr[j] = t }
    if (n % 2) return arr[(n + 1) / 2]
    return (arr[n / 2] + arr[n / 2 + 1]) / 2
}
BEGIN {
    nscen = 0
    for (r = 1; r <= runs; r++) {
        file = dir "/run" r ".txt"
        while ((getline line < file) > 0) {
            split(line, f, " ")
            scen = f[2]; size = f[3]; phase = f[4]
            split(f[5], a, "="); ops = a[2]
            split(f[6], b, "="); ms = b[2]
            split(f[7], c, "="); cs = c[2]
            key = scen "|" size "|" phase
            if (!(key in seen)) { seen[key] = 1; order[++nscen] = key }
            msv[key "|" r] = ms
            opsv[key] = ops
            if (key in csv && csv[key] != cs) {
                print "CHECKSUM UNSTABLE across runs: " key > "/dev/stderr"; bad = 1
            }
            csv[key] = cs
        }
        close(file)
    }
    # checksum: frozen must equal fluid
    for (i = 1; i <= nscen; i++) {
        key = order[i]
        if (key ~ /\|frozen$/) {
            fk = key; sub(/\|frozen$/, "|fluid", fk)
            if (csv[key] != csv[fk]) {
                print "CHECKSUM MISMATCH frozen vs fluid: " key " (" csv[key] " vs " csv[fk] ")" > "/dev/stderr"
                bad = 1
            }
        }
    }
    printf "%-12s %7s  %12s %13s  %12s\n", "scenario", "size", "fluid ns/op", "frozen ns/op", "frozen/fluid"
    for (i = 1; i <= nscen; i++) {
        key = order[i]
        if (key !~ /\|fluid$/) continue
        split(key, kf, "|")
        for (r = 1; r <= runs; r++) tmp[r] = msv[key "|" r] + 0
        fl_ms = median(tmp, runs)
        fk = kf[1] "|" kf[2] "|frozen"
        for (r = 1; r <= runs; r++) tmp[r] = msv[fk "|" r] + 0
        fz_ms = median(tmp, runs)
        fl_ns = fl_ms * 1e6 / opsv[key]
        fz_ns = fz_ms * 1e6 / opsv[fk]
        ratio = (fl_ns > 0) ? fz_ns / fl_ns : 0
        printf "%-12s %7s  %12.1f %13.1f  %12.3f\n", kf[1], kf[2], fl_ns, fz_ns, ratio
    }
    if (bad) { print "CHECKSUM VERIFICATION FAILED" > "/dev/stderr"; exit 1 }
    print "checksums: all frozen == fluid, stable across runs" > "/dev/stderr"
}'
