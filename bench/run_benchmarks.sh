#!/bin/bash
# Performance comparison: Tree-Walker vs Stack VM vs Register VM
# Runs each benchmark 3 times and reports median time

set -e

CLAT="./clat"
BENCH_DIR="bench"
RUNS=5

# Colors
BOLD='\033[1m'
CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
DIM='\033[2m'
NC='\033[0m'

benchmarks=(
    "fib_recursive:Fibonacci recursive (n=32)"
    "fib_iterative:Fibonacci iterative (50K)"
    "nested_loops:Nested loops (1M iters)"
    "sieve:Sieve of Eratosthenes (20K)"
    "bubble_sort:Bubble sort (500 elements)"
    "map_operations:Map ops (5K insert+look)"
    "string_concat:String concat (50K)"
    "matrix_mul:Matrix multiply (30x30)"
    "closures:Closures (100K create+call)"
    "scope_spawn:Scope/spawn (500 rounds)"
    "select_channels:Select channels (5K)"
)

# Get median of N values
median() {
    local n=$#
    local mid=$(( (n + 1) / 2 ))
    echo "$@" | tr ' ' '\n' | sort -n | sed -n "${mid}p"
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

# Color a speedup value relative to baseline
color_speedup() {
    local val="$1"
    if (( $(echo "$val > 1.05" | bc -l 2>/dev/null) )); then
        printf "${GREEN}%5sx${NC}" "$val"
    elif (( $(echo "$val < 0.95" | bc -l 2>/dev/null) )); then
        printf "${RED}%5sx${NC}" "$val"
    else
        printf "${YELLOW}%5sx${NC}" "$val"
    fi
}

printf "\n${BOLD}Lattice Performance: Tree-Walker vs Stack VM vs Register VM${NC}\n"
printf "${DIM}%s${NC}\n" "$(uname -srm), $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo 'unknown CPU')"
printf "${DIM}Runs per benchmark: ${RUNS} (median reported)${NC}\n"
printf "\n"

# Header
printf "${BOLD}%-34s %9s %9s %9s  %8s %8s${NC}\n" \
    "Benchmark" "TreeWalk" "StackVM" "RegVM" "TW/SVM" "TW/RVM"
printf "%-34s %9s %9s %9s  %8s %8s\n" \
    "$(printf '%0.s─' {1..34})" "─────────" "─────────" "─────────" "────────" "────────"

total_tw=0
total_svm=0
total_rvm=0

for entry in "${benchmarks[@]}"; do
    name="${entry%%:*}"
    label="${entry#*:}"
    file="$BENCH_DIR/${name}.lat"

    if [ ! -f "$file" ]; then
        printf "${RED}%-34s MISSING${NC}\n" "$label"
        continue
    fi

    # Verify all three modes produce same output
    out_tw=$($CLAT --tree-walk "$file" 2>/dev/null || true)
    out_svm=$($CLAT "$file" 2>/dev/null || true)
    out_rvm=$($CLAT --regvm "$file" 2>/dev/null || true)

    if [ "$out_tw" != "$out_svm" ]; then
        printf "${RED}%-34s OUTPUT MISMATCH (tw vs svm)${NC}\n" "$label"
        printf "${DIM}  tree-walk: %.60s${NC}\n" "$out_tw"
        printf "${DIM}  stack-vm:  %.60s${NC}\n" "$out_svm"
        continue
    fi

    # RegVM may not support all benchmarks; mark but still run what works
    rvm_ok=true
    if [ "$out_tw" != "$out_rvm" ]; then
        rvm_ok=false
    fi

    # Run benchmarks
    tw_times=()
    svm_times=()
    rvm_times=()
    for ((r=1; r<=RUNS; r++)); do
        tw_times+=($(time_ms "$CLAT --tree-walk '$file'"))
        svm_times+=($(time_ms "$CLAT '$file'"))
        if $rvm_ok; then
            rvm_times+=($(time_ms "$CLAT --regvm '$file'"))
        fi
    done

    tw_med=$(median "${tw_times[@]}")
    svm_med=$(median "${svm_times[@]}")

    total_tw=$(python3 -c "print($total_tw + $tw_med)")
    total_svm=$(python3 -c "print($total_svm + $svm_med)")

    # Stack VM speedup (tree-walk / stack-vm)
    if (( $(echo "$svm_med > 0" | bc -l) )); then
        svm_speedup=$(python3 -c "print(round($tw_med / $svm_med, 2))")
    else
        svm_speedup="inf"
    fi

    if $rvm_ok; then
        rvm_med=$(median "${rvm_times[@]}")
        total_rvm=$(python3 -c "print($total_rvm + $rvm_med)")

        if (( $(echo "$rvm_med > 0" | bc -l) )); then
            rvm_speedup=$(python3 -c "print(round($tw_med / $rvm_med, 2))")
        else
            rvm_speedup="inf"
        fi

        printf "%-34s %7sms %7sms %7sms  " "$label" "$tw_med" "$svm_med" "$rvm_med"
        color_speedup "$svm_speedup"
        printf "  "
        color_speedup "$rvm_speedup"
        printf "\n"
    else
        printf "%-34s %7sms %7sms ${DIM}%7s${NC}  " "$label" "$tw_med" "$svm_med" "n/a"
        color_speedup "$svm_speedup"
        printf "  ${DIM}  n/a${NC}\n"
    fi
done

printf "%-34s %9s %9s %9s  %8s %8s\n" \
    "$(printf '%0.s─' {1..34})" "─────────" "─────────" "─────────" "────────" "────────"

# Totals
if (( $(echo "$total_svm > 0" | bc -l) )); then
    total_svm_su=$(python3 -c "print(round($total_tw / $total_svm, 2))")
else
    total_svm_su="inf"
fi

if (( $(echo "$total_rvm > 0" | bc -l) )); then
    total_rvm_su=$(python3 -c "print(round($total_tw / $total_rvm, 2))")
else
    total_rvm_su="—"
fi

total_tw_f=$(python3 -c "print(round($total_tw, 1))")
total_svm_f=$(python3 -c "print(round($total_svm, 1))")
total_rvm_f=$(python3 -c "print(round($total_rvm, 1))")

printf "${BOLD}%-34s %7sms %7sms %7sms  %5sx  %5sx${NC}\n" \
    "TOTAL" "$total_tw_f" "$total_svm_f" "$total_rvm_f" "$total_svm_su" "$total_rvm_su"

printf "\n${DIM}Speedup = tree-walk time / backend time (>1x means backend is faster than tree-walk)${NC}\n"
printf "${DIM}${GREEN}green${NC}${DIM} = faster, ${RED}red${NC}${DIM} = slower, ${YELLOW}yellow${NC}${DIM} = similar${NC}\n\n"
