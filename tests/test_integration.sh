#!/bin/bash
set -e

# Integration test script for Lattice interpreter
# Compares output of Rust and C implementations on example .lat files

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CLAT_DIR="$SCRIPT_DIR/.."
EXAMPLES_DIR="$PROJECT_ROOT/examples"

# Locate binaries
RUST_BINARY=""
C_BINARY="$CLAT_DIR/clat"

# Try to find Rust binary (prefer release, fallback to debug)
if [ -f "$PROJECT_ROOT/target/release/lattice" ]; then
    RUST_BINARY="$PROJECT_ROOT/target/release/lattice"
elif [ -f "$PROJECT_ROOT/target/debug/lattice" ]; then
    RUST_BINARY="$PROJECT_ROOT/target/debug/lattice"
fi

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Tracking
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
FAILED_TESTS=()

# Verify binaries exist
if [ -z "$RUST_BINARY" ]; then
    echo -e "${RED}Error: Rust binary not found at $PROJECT_ROOT/target/release/lattice or $PROJECT_ROOT/target/debug/lattice${NC}"
    echo "Please build the Rust version with: cargo build --release"
    exit 1
fi

if [ ! -f "$C_BINARY" ]; then
    echo -e "${RED}Error: C binary not found at $C_BINARY${NC}"
    echo "Please build the C version with: cd $CLAT_DIR && make"
    exit 1
fi

if [ ! -d "$EXAMPLES_DIR" ]; then
    echo -e "${RED}Error: Examples directory not found at $EXAMPLES_DIR${NC}"
    exit 1
fi

# Create temporary directory for test outputs
TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

echo "Lattice Integration Test Suite"
echo "=============================="
echo "Rust binary:   $RUST_BINARY"
echo "C binary:      $C_BINARY"
echo "Examples dir:  $EXAMPLES_DIR"
echo ""

# Find all .lat files in examples directory
LAT_FILES=()
while IFS= read -r file; do
    LAT_FILES+=("$file")
done < <(find "$EXAMPLES_DIR" -maxdepth 1 -name "*.lat" -type f | sort)

if [ ${#LAT_FILES[@]} -eq 0 ]; then
    echo -e "${YELLOW}Warning: No .lat files found in $EXAMPLES_DIR${NC}"
    exit 0
fi

# Run tests
for lat_file in "${LAT_FILES[@]}"; do
    test_name=$(basename "$lat_file" .lat)
    TESTS_RUN=$((TESTS_RUN + 1))

    rust_output_file="$TEMP_DIR/${test_name}_rust.txt"
    c_output_file="$TEMP_DIR/${test_name}_c.txt"
    diff_file="$TEMP_DIR/${test_name}_diff.txt"

    # Run Rust binary
    if ! "$RUST_BINARY" "$lat_file" > "$rust_output_file" 2>&1; then
        echo -e "${RED}✗ $test_name${NC} (Rust binary failed)"
        TESTS_FAILED=$((TESTS_FAILED + 1))
        FAILED_TESTS+=("$test_name")
        continue
    fi

    # Run C binary
    if ! "$C_BINARY" "$lat_file" > "$c_output_file" 2>&1; then
        echo -e "${RED}✗ $test_name${NC} (C binary failed)"
        TESTS_FAILED=$((TESTS_FAILED + 1))
        FAILED_TESTS+=("$test_name")
        continue
    fi

    # Compare outputs
    if diff -u "$rust_output_file" "$c_output_file" > "$diff_file" 2>&1; then
        echo -e "${GREEN}✓ $test_name${NC}"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo -e "${RED}✗ $test_name${NC} (output mismatch)"
        TESTS_FAILED=$((TESTS_FAILED + 1))
        FAILED_TESTS+=("$test_name")
        echo "  Diff:"
        sed 's/^/    /' "$diff_file" | head -20
        if [ $(wc -l < "$diff_file") -gt 20 ]; then
            echo "    ... (diff truncated)"
        fi
    fi
done

# Print summary
echo ""
echo "=============================="
echo "Test Summary"
echo "=============================="
echo "Total tests:  $TESTS_RUN"
echo -e "Passed:       ${GREEN}$TESTS_PASSED${NC}"
if [ $TESTS_FAILED -gt 0 ]; then
    echo -e "Failed:       ${RED}$TESTS_FAILED${NC}"
    echo ""
    echo "Failed tests:"
    for test in "${FAILED_TESTS[@]}"; do
        echo -e "  ${RED}• $test${NC}"
    done
else
    echo -e "Failed:       ${GREEN}0${NC}"
fi

# Exit with appropriate code
if [ $TESTS_FAILED -gt 0 ]; then
    exit 1
else
    exit 0
fi
