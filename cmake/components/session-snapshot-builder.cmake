target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/snapshot.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_session_snapshot_builder
        ENTRY ${SSG_SOURCE_DIR}/tests/test_session_snapshot_builder.cpp
        SYMBOL test_session_snapshot_builder)
    ssg_test_include_directories(test_session_snapshot_builder PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_session_snapshot_builder PRIVATE ssg_tui_objects)

endif()
