target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/RecoveryManager.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    foreach(_suite IN ITEMS
            test_recovery
            test_recovery_tree
            test_recovery_failures
            test_recovery_rollback
            test_recovery_reconstruction
            test_recovery_retry
            test_recovery_budget
            test_recovery_budget_rejection)
        ssg_add_test_suite(
            NAME ${_suite}
            ENTRY ${SSG_SOURCE_DIR}/tests/test_recovery.cpp
            SYMBOL ${_suite})
    endforeach()
    ssg_test_include_directories(test_recovery PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_recovery PRIVATE ssg_core)

endif()
