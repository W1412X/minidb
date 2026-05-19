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
echo "  Tests:  full local suite"
echo "============================================"

# --- Unit Tests ---
echo -e "\n[Unit Tests]"
run_test "lock_manager"   "./build/tests/lock_manager_wait_test"
run_test "tuple_value"    "./build/tests/tuple_value_edge_test"
run_test "page_store"     "./build/tests/page_store_remote_test"
run_test "wal_buffer_pool" "./build/tests/wal_buffer_pool_test"
run_test "txn_slots"      "./build/tests/transaction_slots_test"

# --- SQL Regression ---
echo -e "\n[SQL Regression]"
run_test "regression"     "bash tests/sql_regression.sh $BIN"
run_test "resource_limits" "bash tests/resource_limits.sh $BIN"
run_test "keyword_combos" "bash tests/sql_keyword_combinations.sh $BIN"
run_test "join_syntax"    "bash tests/join_syntax_strict.sh $BIN"
run_test "production_reg" "bash tests/production_regression.sh $BIN"
run_test "join_optimizer" "bash tests/join_optimizer.sh $BIN"

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

# --- Storage / Recovery / Performance Paths ---
echo -e "\n[Storage and Recovery]"
run_test "persistence"    "bash tests/persistence_and_composite.sh $BIN"
run_test "remote_store"   "bash tests/remote_page_store.sh $BIN"
run_test "recovery"       "bash tests/recovery_smoke.sh $BIN"
run_test "index_delete"   "bash tests/index_delete_rebuild.sh $BIN"
run_test "bulk_perf"      "bash tests/bulk_update_delete_perf.sh $BIN"
run_test "perf_paths"     "bash tests/performance_paths.sh $BIN"
run_test "perf_delivery"  "bash tests/performance_delivery.sh $BIN"
run_test "perf_opts"      "bash tests/performance_optimizations.sh $BIN"

echo -e "\n============================================"
echo "  Results: $PASS passed, $FAIL failed"
echo "============================================"

exit $FAIL
