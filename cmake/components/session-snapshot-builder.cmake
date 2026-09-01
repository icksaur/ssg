if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_session_snapshot_builder
        ${SSG_SOURCE_DIR}/tests/test_session_snapshot_builder.cpp
    )
    target_include_directories(test_session_snapshot_builder PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_session_snapshot_builder PRIVATE ssg_grid)
    add_test(NAME test_session_snapshot_builder
             COMMAND test_session_snapshot_builder)
endif()
