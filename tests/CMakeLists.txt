add_test(
    NAME sql_regression
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/sql_regression.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME sql_keyword_combinations
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/sql_keyword_combinations.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME full_sql_suite
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/full_sql_suite.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME performance_paths
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/performance_paths.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME performance_delivery
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/performance_delivery.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME performance_optimizations
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/performance_optimizations.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME join_optimizer
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/join_optimizer.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME join_syntax_strict
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/join_syntax_strict.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME production_regression
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/production_regression.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME sql_correctness_matrix
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/sql_correctness_matrix.py $<TARGET_FILE:minidb> --seed 12648430
)

add_test(
    NAME differential_sqlite
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/differential_sqlite.py $<TARGET_FILE:minidb> --seed 12648431
)

add_test(
    NAME crash_recovery_harness
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/crash_recovery_harness.py $<TARGET_FILE:minidb> --seed 12648432
)

add_test(
    NAME mvcc_lock_regression
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/mvcc_lock_regression.py $<TARGET_FILE:minidb> --seed 12648433
)

add_executable(lock_manager_wait_test lock_manager_wait_test.cpp)
target_link_libraries(lock_manager_wait_test PRIVATE minidb_lib pthread)
target_include_directories(lock_manager_wait_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(
    NAME lock_manager_wait_test
    COMMAND $<TARGET_FILE:lock_manager_wait_test>
)

add_executable(tuple_value_edge_test tuple_value_edge_test.cpp)
target_link_libraries(tuple_value_edge_test PRIVATE minidb_lib pthread)
target_include_directories(tuple_value_edge_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(
    NAME tuple_value_edge_test
    COMMAND $<TARGET_FILE:tuple_value_edge_test>
)

add_executable(page_store_remote_test page_store_remote_test.cpp)
target_link_libraries(page_store_remote_test PRIVATE minidb_lib pthread)
target_include_directories(page_store_remote_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(
    NAME page_store_remote_test
    COMMAND $<TARGET_FILE:page_store_remote_test>
)

add_executable(wal_buffer_pool_test wal_buffer_pool_test.cpp)
target_link_libraries(wal_buffer_pool_test PRIVATE minidb_lib pthread)
target_include_directories(wal_buffer_pool_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(
    NAME wal_buffer_pool_test
    COMMAND $<TARGET_FILE:wal_buffer_pool_test>
)

add_executable(transaction_slots_test transaction_slots_test.cpp)
target_link_libraries(transaction_slots_test PRIVATE minidb_lib pthread)
target_include_directories(transaction_slots_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(
    NAME transaction_slots_test
    COMMAND $<TARGET_FILE:transaction_slots_test>
)

add_executable(btree_property_test btree_property_test.cpp)
target_link_libraries(btree_property_test PRIVATE minidb_lib pthread)
target_include_directories(btree_property_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(
    NAME btree_property_test
    COMMAND $<TARGET_FILE:btree_property_test> 12648434
)

add_executable(index_key_btree_test index_key_btree_test.cpp)
target_link_libraries(index_key_btree_test PRIVATE minidb_lib pthread)
target_include_directories(index_key_btree_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(
    NAME index_key_btree_test
    COMMAND $<TARGET_FILE:index_key_btree_test>
)

add_test(
    NAME persistence_and_composite
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/persistence_and_composite.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME remote_page_store
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/remote_page_store.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME recovery_smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/recovery_smoke.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME wal_replay_slot_reuse
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/wal_replay_slot_reuse.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME index_delete_rebuild
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/index_delete_rebuild.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME hot_index_semantics
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/hot_index_semantics.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME index_unique_matrix
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/index_unique_matrix.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME alter_table_semantics
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/alter_table_semantics.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME bulk_update_delete_perf
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/bulk_update_delete_perf.sh $<TARGET_FILE:minidb>
)
