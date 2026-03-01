#!/usr/bin/env bash
#
# run-benchmarks.sh — Run the Lattice benchmark suite across all three backends.
#
# Usage:
#   ./scripts/run-benchmarks.sh              # run all benchmarks, table output
#   ./scripts/run-benchmarks.sh --csv        # also save results to benchmarks/results.csv
#   ./scripts/run-benchmarks.sh --json       # also save results to benchmarks/results.json
#   ./scripts/run-benchmarks.sh --timeout 60 # override per-benchmark timeout (default: 120s)
#
# Each benchmark is run once per backend. Self-reported elapsed time from each
# benchmark's own time() calls is captured. If unavailable, wall-clock time is
# measured as a fallback.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CLAT="$ROOT_DIR/clat"
BENCH_DIR="$ROOT_DIR/benchmarks"

# Defaults
TIMEOUT=120
SAVE_CSV=false
SAVE_JSON=false
CSV_FILE="$BENCH_DIR/results.csv"
JSON_FILE="$BENCH_DIR/results.json"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --csv)    SAVE_CSV=true; shift ;;
        --json)   SAVE_JSON=true; shift ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--csv] [--json] [--timeout SECONDS]"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# Core benchmark files (the 8 primary benchmarks)
BENCHMARKS=(
    "fib.lat"
    "loop_compute.lat"
    "string_ops.lat"
    "array_ops.lat"
    "map_ops.lat"
    "closure_heavy.lat"
    "method_dispatch.lat"
    "sort.lat"
)

BACKEND_NAMES=("bytecode" "tree-walk" "regvm")
BACKEND_FLAGS=("" "--tree-walk" "--regvm")

# Check binary exists
if [[ ! -x "$CLAT" ]]; then
    echo "Error: $CLAT not found or not executable. Run 'make' first."
    exit 1
fi

# Get version from header
VERSION="unknown"
version_header="$ROOT_DIR/include/lattice.h"
if [[ -f "$version_header" ]]; then
    VERSION=$(grep '#define LATTICE_VERSION' "$version_header" | sed 's/.*"\(.*\)".*/\1/' || echo "unknown")
fi

# Portable millisecond timestamp (works on macOS and Linux)
now_ms() {
    python3 -c 'import time; print(int(time.time()*1000))' 2>/dev/null \
        || perl -MTime::HiRes=time -e 'printf "%d\n", time()*1000' 2>/dev/null \
        || echo "0"
}

echo "============================================================"
echo "  Lattice Benchmark Suite"
echo "  Version: $VERSION"
echo "  Date:    $(date '+%Y-%m-%d %H:%M:%S')"
echo "  Timeout: ${TIMEOUT}s per benchmark"
echo "============================================================"
echo ""

# Column widths
NAME_W=20
COL_W=14
NUM_BACKENDS=${#BACKEND_NAMES[@]}

# Print table header
printf "%-${NAME_W}s" "Benchmark"
for ((b=0; b<NUM_BACKENDS; b++)); do
    printf "%${COL_W}s" "${BACKEND_NAMES[$b]}"
done
echo ""

# Separator line
sep_name=$(printf '%0.s-' $(seq 1 $NAME_W))
sep_col=$(printf '%0.s-' $(seq 1 $COL_W))
printf "%-${NAME_W}s" "$sep_name"
for ((b=0; b<NUM_BACKENDS; b++)); do
    printf "%${COL_W}s" "$sep_col"
done
echo ""

# Storage for results
declare -a RESULT_ROWS=()

# Run each benchmark on each backend
for bench_file in "${BENCHMARKS[@]}"; do
    bench_path="$BENCH_DIR/$bench_file"
    bench_name="${bench_file%.lat}"

    if [[ ! -f "$bench_path" ]]; then
        printf "%-${NAME_W}s" "$bench_name"
        for ((b=0; b<NUM_BACKENDS; b++)); do
            printf "%${COL_W}s" "MISSING"
        done
        echo ""
        continue
    fi

    printf "%-${NAME_W}s" "$bench_name"

    row_data="$bench_name"

    for ((b=0; b<NUM_BACKENDS; b++)); do
        flag="${BACKEND_FLAGS[$b]}"

        # Build the command
        if [[ -n "$flag" ]]; then
            run_cmd="timeout $TIMEOUT $CLAT $flag $bench_path"
        else
            run_cmd="timeout $TIMEOUT $CLAT $bench_path"
        fi

        # Run with timeout and capture output + exit code
        start_ms=$(now_ms)
        output=$($run_cmd 2>&1)
        exit_code=$?
        end_ms=$(now_ms)

        if [[ $exit_code -eq 124 ]]; then
            # Timeout
            printf "%${COL_W}s" "TIMEOUT"
            row_data="$row_data,TIMEOUT"
        elif [[ $exit_code -ne 0 ]]; then
            # Error
            printf "%${COL_W}s" "ERROR"
            row_data="$row_data,ERROR"
        else
            # Try to extract self-reported elapsed from output
            self_ms=$(echo "$output" | grep -oE 'elapsed: [0-9]+ms' | grep -oE '[0-9]+' | tail -1 || true)

            if [[ -n "$self_ms" ]]; then
                printf "%${COL_W}s" "${self_ms}ms"
                row_data="$row_data,${self_ms}"
            else
                # Fall back to wall-clock time
                wall_ms=$(( end_ms - start_ms ))
                printf "%${COL_W}s" "${wall_ms}ms*"
                row_data="$row_data,${wall_ms}"
            fi
        fi
    done

    echo ""
    RESULT_ROWS+=("$row_data")
done

# Separator
printf "%-${NAME_W}s" "$sep_name"
for ((b=0; b<NUM_BACKENDS; b++)); do
    printf "%${COL_W}s" "$sep_col"
done
echo ""
echo ""
echo "Times are self-reported via time() unless marked with * (wall-clock)."
echo "TIMEOUT = exceeded ${TIMEOUT}s, ERROR = runtime error."
echo ""

# Save CSV
if $SAVE_CSV; then
    {
        echo "benchmark,bytecode_ms,tree_walk_ms,regvm_ms"
        for row in "${RESULT_ROWS[@]}"; do
            echo "$row"
        done
    } > "$CSV_FILE"
    echo "Results saved to: $CSV_FILE"
fi

# Save JSON
if $SAVE_JSON; then
    {
        echo "{"
        echo "  \"version\": \"$VERSION\","
        echo "  \"date\": \"$(date -u '+%Y-%m-%dT%H:%M:%SZ')\","
        echo "  \"timeout_seconds\": $TIMEOUT,"
        echo "  \"results\": ["
        first=true
        for row in "${RESULT_ROWS[@]}"; do
            IFS=',' read -r name bytecode treewalk regvm <<< "$row"
            if $first; then first=false; else printf ",\n"; fi
            # Quote non-numeric values
            bq="$bytecode"; [[ "$bytecode" =~ ^[0-9]+$ ]] || bq="\"$bytecode\""
            tq="$treewalk"; [[ "$treewalk" =~ ^[0-9]+$ ]] || tq="\"$treewalk\""
            rq="$regvm";    [[ "$regvm" =~ ^[0-9]+$ ]]    || rq="\"$regvm\""
            printf '    {\n'
            printf '      "name": "%s",\n' "$name"
            printf '      "bytecode_ms": %s,\n' "$bq"
            printf '      "tree_walk_ms": %s,\n' "$tq"
            printf '      "regvm_ms": %s\n' "$rq"
            printf '    }'
        done
        echo ""
        echo "  ]"
        echo "}"
    } > "$JSON_FILE"
    echo "Results saved to: $JSON_FILE"
fi
