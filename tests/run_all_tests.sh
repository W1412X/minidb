#!/usr/bin/env bash
# run_all_tests.sh — Run all MiniDB tests with documentation

set -uo pipefail

BIN="${1:?Usage: $0 ./build/minidb}"

PASS=0
FAIL=0

run_test() {
    local name="$1"
    local cmd="$2"
    echo -n "  [$name] ... "
    local start=$(date +%s%N)
    if eval "$cmd" > /dev/null 2>&1; then
        local end=$(date +%s%N)
        local ms=$(( (end - start) / 1000000 ))
        echo "PASS (${ms}ms)"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        FAIL=$((FAIL + 1))
    fi
}

echo "============================================"
echo "  MiniDB Test Suite"
echo "============================================"
echo "  Binary: $BIN"
echo "  Tests:  22 test scripts"
echo "============================================"

# --- Unit Tests ---
echo -e "\n[Unit Tests]"
run_test "lock_manager"   "./build/tests/lock_manager_wait_test"
run_test "tuple_value"    "./build/tests/tuple_value_edge_test"

# --- SQL Regression ---
echo -e "\n[SQL Regression]"
run_test "regression"     "bash tests/sql_regression.sh $BIN"
run_test "resource_limits" "bash tests/resource_limits.sh $BIN"
run_test "keyword_combos" "bash tests/sql_keyword_combinations.sh $BIN"
run_test "join_syntax"    "bash tests/join_syntax_strict.sh $BIN"

# --- Comprehensive SQL ---
echo -e "\n[Comprehensive SQL]"
run_test "comprehensive"  "python3 tests/comprehensive_test.py $BIN"

# --- Data Consistency ---
echo -e "\n[Data Consistency]"
run_test "consistency"    "python3 tests/consistency_test.py $BIN"
run_test "bug_verify"     "python3 tests/bug_verify.py $BIN"

# --- Ultimate Test ---
echo -e "\n[Ultimate Test]"
run_test "ultimate"       "python3 tests/ultimate_test.py $BIN"

# --- Concurrent ---
echo -e "\n[Concurrent]"
run_test "concurrent"     "python3 tests/concurrent_test.py $BIN"

echo -e "\n============================================"
echo "  Results: $PASS passed, $FAIL failed"
echo "============================================"

exit $FAIL
