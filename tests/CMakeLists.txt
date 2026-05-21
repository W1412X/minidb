# MiniDB test registration
#
# Tests live under topical subdirectories:
#   unit/          C++ unit tests (built as standalone executables)
#   sql/           SQL correctness, parser, planner, executor coverage
#   ddl/           DDL / catalog evolution tests
#   index/         B+ tree and index maintenance tests
#   acid/          ACID-focused tests
#     atomicity/   commit/rollback/crash atomicity
#     consistency/ constraint enforcement and post-recovery checks
#     isolation/   MVCC / lock / concurrency isolation
#     durability/  WAL, checkpoint, crash recovery
#   concurrency/   multi-client concurrency
#   storage/       page store, remote page server
#   performance/   performance smoke and regression
#   regression/    broad end-to-end regression / consistency
#
# Every shell test takes the minidb binary as $1 ($<TARGET_FILE:minidb>).
# Every Python test takes the binary as positional argv, plus --seed N.

# ---------------------------------------------------------------------------
# Unit tests (C++)
# ---------------------------------------------------------------------------

set(MINIDB_UNIT_TESTS
    lock_manager_wait_test
    tuple_value_edge_test
    page_store_remote_test
    wal_buffer_pool_test
    transaction_slots_test
    btree_property_test
    index_key_btree_test
)

foreach(_test ${MINIDB_UNIT_TESTS})
    add_executable(${_test} unit/${_test}.cpp)
    target_link_libraries(${_test} PRIVATE minidb_lib pthread)
    target_include_directories(${_test} PRIVATE ${CMAKE_SOURCE_DIR}/src)
endforeach()

add_test(NAME lock_manager_wait_test    COMMAND $<TARGET_FILE:lock_manager_wait_test>)
add_test(NAME tuple_value_edge_test     COMMAND $<TARGET_FILE:tuple_value_edge_test>)
add_test(NAME page_store_remote_test    COMMAND $<TARGET_FILE:page_store_remote_test>)
add_test(NAME wal_buffer_pool_test      COMMAND $<TARGET_FILE:wal_buffer_pool_test>)
add_test(NAME transaction_slots_test    COMMAND $<TARGET_FILE:transaction_slots_test>)
add_test(NAME btree_property_test       COMMAND $<TARGET_FILE:btree_property_test> 12648434)
add_test(NAME index_key_btree_test      COMMAND $<TARGET_FILE:index_key_btree_test>)

# ---------------------------------------------------------------------------
# SQL correctness
# ---------------------------------------------------------------------------

add_test(NAME sql_regression
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/sql/sql_regression.sh $<TARGET_FILE:minidb>)
add_test(NAME sql_keyword_combinations
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/sql/sql_keyword_combinations.sh $<TARGET_FILE:minidb>)
add_test(NAME full_sql_suite
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/sql/full_sql_suite.sh $<TARGET_FILE:minidb>)
add_test(NAME join_syntax_strict
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/sql/join_syntax_strict.sh $<TARGET_FILE:minidb>)
add_test(NAME join_optimizer
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/sql/join_optimizer.sh $<TARGET_FILE:minidb>)
add_test(NAME sql_correctness_matrix
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/sql/sql_correctness_matrix.py $<TARGET_FILE:minidb> --seed 12648430)
add_test(NAME differential_sqlite
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/sql/differential_sqlite.py $<TARGET_FILE:minidb> --seed 12648431)

# ---------------------------------------------------------------------------
# DDL and catalog
# ---------------------------------------------------------------------------

add_test(NAME alter_table_semantics
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/ddl/alter_table_semantics.sh $<TARGET_FILE:minidb>)
add_test(NAME hot_index_semantics
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/ddl/hot_index_semantics.sh $<TARGET_FILE:minidb>)

# ---------------------------------------------------------------------------
# Index maintenance
# ---------------------------------------------------------------------------

add_test(NAME persistence_and_composite
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/index/persistence_and_composite.sh $<TARGET_FILE:minidb>)
add_test(NAME index_delete_rebuild
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/index/index_delete_rebuild.sh $<TARGET_FILE:minidb>)
add_test(NAME index_unique_matrix
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/index/index_unique_matrix.sh $<TARGET_FILE:minidb>)

# ---------------------------------------------------------------------------
# ACID
# ---------------------------------------------------------------------------

add_test(NAME mvcc_lock_regression
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/acid/isolation/mvcc_lock_regression.py $<TARGET_FILE:minidb> --seed 12648433)
add_test(NAME commit_durability
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/acid/atomicity/commit_durability.py $<TARGET_FILE:minidb> --seed 12648435)
add_test(NAME constraints_not_null_default
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/acid/consistency/constraints_not_null_default.py $<TARGET_FILE:minidb> --seed 12648437)
add_test(NAME recovery_smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/acid/durability/recovery_smoke.sh $<TARGET_FILE:minidb>)
add_test(NAME wal_replay_slot_reuse
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/acid/durability/wal_replay_slot_reuse.sh $<TARGET_FILE:minidb>)
add_test(NAME torn_page
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/acid/durability/torn_page.py $<TARGET_FILE:minidb> --seed 12648436)
add_test(NAME crash_recovery_harness
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/acid/durability/crash_recovery_harness.py $<TARGET_FILE:minidb> --seed 12648432)

# ---------------------------------------------------------------------------
# Storage / page server
# ---------------------------------------------------------------------------

add_test(NAME remote_page_store
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/storage/remote_page_store.sh $<TARGET_FILE:minidb>)

# ---------------------------------------------------------------------------
# Performance smoke
# ---------------------------------------------------------------------------

add_test(NAME performance_paths
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/performance/performance_paths.sh $<TARGET_FILE:minidb>)
add_test(NAME performance_delivery
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/performance/performance_delivery.sh $<TARGET_FILE:minidb>)
add_test(NAME performance_optimizations
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/performance/performance_optimizations.sh $<TARGET_FILE:minidb>)
add_test(NAME bulk_update_delete_perf
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/performance/bulk_update_delete_perf.sh $<TARGET_FILE:minidb>)

# ---------------------------------------------------------------------------
# Broad regression
# ---------------------------------------------------------------------------

add_test(NAME production_regression
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/regression/production_regression.sh $<TARGET_FILE:minidb>)
add_test(NAME resource_limits
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/regression/resource_limits.sh $<TARGET_FILE:minidb>)
