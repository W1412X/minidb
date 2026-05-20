#!/usr/bin/env bash
# run_all_tests.sh — Run all MiniDB tests with documentation

set -uo pipefail

BIN=""
SEED=12648430
STRESS=0
SUITE="main"
REPORT="${MINIDB_TEST_REPORT:-build/test-report.md}"

usage() {
    echo "Usage: $0 ./build/minidb [--seed N] [--stress] [--suite pr|main|nightly] [--report path]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --seed) SEED="${2:?missing seed}"; shift 2 ;;
        --stress) STRESS=1; shift ;;
        --suite) SUITE="${2:?missing suite}"; shift 2 ;;
        --report) REPORT="${2:?missing report path}"; shift 2 ;;
        --help) usage; exit 0 ;;
        *)
            if [[ -z "$BIN" ]]; then BIN="$1"; shift
            else echo "unknown argument: $1" >&2; usage; exit 2
            fi
            ;;
    esac
done

if [[ -z "$BIN" ]]; then usage >&2; exit 2; fi

PASS=0
FAIL=0
mkdir -p "$(dirname "$REPORT")"
LOG_DIR="$(dirname "$REPORT")/test-logs"
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"
{
    echo "# MiniDB Test Report"
    echo
    echo "- suite: $SUITE"
    echo "- stress: $STRESS"
    echo "- seed: $SEED"
    echo "- binary: $BIN"
    echo "- started_at: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo
    echo "| Group | Test | Result | Duration ms | Log |"
    echo "|---|---|---:|---:|---|"
} > "$REPORT"

CURRENT_GROUP=""

run_test() {
    local name="$1"
    local cmd="$2"
    local group="${CURRENT_GROUP:-General}"
    local log_name
    log_name="$(printf '%s_%s' "$group" "$name" | tr ' /' '__' | tr -cd '[:alnum:]_.-')"
    local log_file="$LOG_DIR/${log_name}.log"
    echo -n "  [$name] ... "
    local start=$(date +%s%N)
    if eval "$cmd" > "$log_file" 2>&1; then
        local end=$(date +%s%N)
        local ms=$(( (end - start) / 1000000 ))
        echo "PASS (${ms}ms)"
        echo "| $group | $name | PASS | $ms | $log_file |" >> "$REPORT"
        PASS=$((PASS + 1))
    else
        local end=$(date +%s%N)
        local ms=$(( (end - start) / 1000000 ))
        echo "FAIL"
        echo "| $group | $name | FAIL | $ms | $log_file |" >> "$REPORT"
        echo "    command: $cmd" >&2
        echo "    seed: $SEED" >&2
        echo "    log: $log_file" >&2
        echo "    log tail:" >&2
        tail -80 "$log_file" >&2 || true
        FAIL=$((FAIL + 1))
    fi
}

echo "============================================"
echo "  MiniDB Test Suite"
echo "============================================"
echo "  Binary: $BIN"
echo "  Suite:  $SUITE"
echo "  Stress: $STRESS"
echo "  Seed:   $SEED"
echo "  Report: $REPORT"
echo "============================================"

# --- Unit Tests ---
CURRENT_GROUP="Unit"
echo -e "\n[Unit Tests]"
run_test "lock_manager"   "./build/tests/lock_manager_wait_test"
run_test "tuple_value"    "./build/tests/tuple_value_edge_test"
run_test "page_store"     "./build/tests/page_store_remote_test"
run_test "wal_buffer_pool" "./build/tests/wal_buffer_pool_test"
run_test "txn_slots"      "./build/tests/transaction_slots_test"

# --- SQL Regression ---
CURRENT_GROUP="SQL Regression"
echo -e "\n[SQL Regression]"
run_test "regression"     "bash tests/sql_regression.sh $BIN"
run_test "resource_limits" "bash tests/resource_limits.sh $BIN"
run_test "keyword_combos" "bash tests/sql_keyword_combinations.sh $BIN"
run_test "join_syntax"    "bash tests/join_syntax_strict.sh $BIN"
run_test "production_reg" "bash tests/production_regression.sh $BIN"
run_test "join_optimizer" "bash tests/join_optimizer.sh $BIN"
stress_flag=""
if [[ "$STRESS" == "1" ]]; then stress_flag="--stress"; fi
run_test "sql_matrix"     "python3 tests/sql_correctness_matrix.py $BIN --seed $SEED $stress_flag"
run_test "sqlite_diff"    "python3 tests/differential_sqlite.py $BIN --seed $((SEED + 1)) $stress_flag"
run_test "crash_harness"  "python3 tests/crash_recovery_harness.py $BIN --seed $((SEED + 2)) $stress_flag"
run_test "mvcc_lock"      "python3 tests/mvcc_lock_regression.py $BIN --seed $((SEED + 3)) $stress_flag"

if [[ "$SUITE" == "pr" && "$STRESS" != "1" ]]; then
    echo -e "\n============================================"
    echo "  Results: $PASS passed, $FAIL failed"
    echo "  Report: $REPORT"
    echo "============================================"
    {
        echo
        echo "- completed_at: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        echo "- passed: $PASS"
        echo "- failed: $FAIL"
    } >> "$REPORT"
    exit $FAIL
fi

# --- Comprehensive SQL ---
CURRENT_GROUP="Comprehensive SQL"
echo -e "\n[Comprehensive SQL]"
run_test "comprehensive"  "python3 tests/comprehensive_test.py $BIN"

# --- Data Consistency ---
CURRENT_GROUP="Data Consistency"
echo -e "\n[Data Consistency]"
run_test "consistency"    "python3 tests/consistency_test.py $BIN"
run_test "bug_verify"     "python3 tests/bug_verify.py $BIN"

# --- Ultimate Test ---
CURRENT_GROUP="Ultimate"
echo -e "\n[Ultimate Test]"
run_test "ultimate"       "python3 tests/ultimate_test.py $BIN"

# --- Concurrent ---
CURRENT_GROUP="Concurrent"
echo -e "\n[Concurrent]"
run_test "concurrent"     "python3 tests/concurrent_test.py $BIN"

# --- Storage / Recovery / Performance Paths ---
CURRENT_GROUP="Storage and Recovery"
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
echo "  Report: $REPORT"
echo "============================================"

{
    echo
    echo "- completed_at: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "- passed: $PASS"
    echo "- failed: $FAIL"
} >> "$REPORT"

exit $FAIL
