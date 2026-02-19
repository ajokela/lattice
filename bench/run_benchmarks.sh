#!/bin/bash
# Performance comparison: tree-walker vs bytecode VM
# Runs each benchmark 3 times and reports median time

set -e

CLAT="./clat"
BENCH_DIR="bench"
RUNS=3

# Colors
BOLD='\033[1m'
CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
DIM='\033[2m'
NC='\033[0m'

benchmarks=(
    "fib_recursive:Fibonacci (recursive, n=30)"
    "fib_iterative:Fibonacci (iterative, 10K calls)"
    "sieve:Sieve of Eratosthenes (n=10K)"
    "bubble_sort:Bubble sort (500 elements)"
    "nested_loops:Nested loops (250K iterations)"
    "map_operations:Map ops (2K insert + lookup)"
    "string_concat:String concat (2K appends)"
    "matrix_mul:Matrix multiply (30x30)"
)

# Get median of 3 values
median3() {
    echo "$1 $2 $3" | tr ' ' '\n' | sort -n | sed -n '2p'
}

# Time a command, return milliseconds
time_ms() {
    local start end elapsed
    start=$(python3 -c 'import time; print(time.time())')
    eval "$@" > /dev/null 2>&1
    end=$(python3 -c 'import time; print(time.time())')
    elapsed=$(python3 -c "print(round(($end - $start) * 1000, 1))")
    echo "$elapsed"
}

printf "\n${BOLD}Lattice Performance: Tree-Walker vs Bytecode VM${NC}\n"
printf "${DIM}%s${NC}\n" "$(uname -srm), $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo 'unknown CPU')"
printf "${DIM}Runs per benchmark: ${RUNS} (median reported)${NC}\n"
printf "\n"

# Header
printf "${BOLD}%-38s %10s %10s %10s${NC}\n" "Benchmark" "Tree-Walk" "Bytecode" "Speedup"
printf "%-38s %10s %10s %10s\n" "$(printf '%0.s─' {1..38})" "──────────" "──────────" "──────────"

total_tw=0
total_bc=0

for entry in "${benchmarks[@]}"; do
    name="${entry%%:*}"
    label="${entry#*:}"
    file="$BENCH_DIR/${name}.lat"

    if [ ! -f "$file" ]; then
        printf "${RED}%-38s MISSING${NC}\n" "$label"
        continue
    fi

    # Verify both modes produce same output
    out_tw=$($CLAT --tree-walk "$file" 2>/dev/null)
    out_bc=$($CLAT "$file" 2>/dev/null)
    if [ "$out_tw" != "$out_bc" ]; then
        printf "${RED}%-38s OUTPUT MISMATCH${NC}\n" "$label"
        printf "${DIM}  tree-walk: %s${NC}\n" "$out_tw"
        printf "${DIM}  bytecode:  %s${NC}\n" "$out_bc"
        continue
    fi

    # Run benchmarks
    tw_times=()
    bc_times=()
    for ((r=1; r<=RUNS; r++)); do
        tw_times+=($(time_ms "$CLAT --tree-walk '$file'"))
        bc_times+=($(time_ms "$CLAT '$file'"))
    done

    tw_med=$(median3 "${tw_times[0]}" "${tw_times[1]}" "${tw_times[2]}")
    bc_med=$(median3 "${bc_times[0]}" "${bc_times[1]}" "${bc_times[2]}")

    total_tw=$(python3 -c "print($total_tw + $tw_med)")
    total_bc=$(python3 -c "print($total_bc + $bc_med)")

    # Calculate speedup
    if (( $(echo "$bc_med > 0" | bc -l) )); then
        speedup=$(python3 -c "print(round($tw_med / $bc_med, 2))")
    else
        speedup="inf"
    fi

    # Color the speedup
    if (( $(echo "$speedup > 1.0" | bc -l 2>/dev/null) )); then
        color="$GREEN"
        arrow="faster"
    elif (( $(echo "$speedup < 1.0" | bc -l 2>/dev/null) )); then
        color="$RED"
        arrow="slower"
    else
        color="$YELLOW"
        arrow="same"
    fi

    printf "%-38s %8sms %8sms  ${color}%5sx ($arrow)${NC}\n" \
        "$label" "$tw_med" "$bc_med" "$speedup"
done

printf "%-38s %10s %10s %10s\n" "$(printf '%0.s─' {1..38})" "──────────" "──────────" "──────────"

# Total speedup
if (( $(echo "$total_bc > 0" | bc -l) )); then
    total_speedup=$(python3 -c "print(round($total_tw / $total_bc, 2))")
else
    total_speedup="inf"
fi
printf "${BOLD}%-38s %8sms %8sms  %5sx${NC}\n" \
    "TOTAL" "$total_tw" "$total_bc" "$total_speedup"

printf "\n${DIM}Speedup = tree-walk time / bytecode time (>1 means bytecode is faster)${NC}\n\n"
