target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/edit_history_coordinator.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_edit_history_integration
        ${SSG_SOURCE_DIR}/tests/test_edit_history_coordinator.cpp
    )
    target_link_libraries(test_edit_history_integration PRIVATE ssg)
    add_test(NAME test_edit_history_integration
             COMMAND test_edit_history_integration)
endif()
