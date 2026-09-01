target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/EditHistoryCoordinator.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_edit_history_integration
        ${SSG_SOURCE_DIR}/tests/test_edit_history_coordinator.cpp
    )
    target_link_libraries(test_edit_history_integration PRIVATE ssg_core)
    add_test(NAME test_edit_history_integration
             COMMAND test_edit_history_integration)
endif()
