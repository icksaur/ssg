target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/EditHistoryCoordinator.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_edit_history_integration
        ENTRY ${SSG_SOURCE_DIR}/tests/test_edit_history_coordinator.cpp
        SYMBOL test_edit_history_integration)
    ssg_test_link_libraries(test_edit_history_integration PRIVATE ssg_core)

endif()
