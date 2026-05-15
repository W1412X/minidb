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

add_test(
    NAME persistence_and_composite
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/persistence_and_composite.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME recovery_smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/recovery_smoke.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME index_delete_rebuild
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/index_delete_rebuild.sh $<TARGET_FILE:minidb>
)

add_test(
    NAME bulk_update_delete_perf
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/bulk_update_delete_perf.sh $<TARGET_FILE:minidb>
)
