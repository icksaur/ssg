target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/runtime/command_executor.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_session
        ENTRY ${SSG_SOURCE_DIR}/tests/test_session.cpp
        SYMBOL test_session)
    ssg_test_include_directories(test_session PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_session PRIVATE ssg_core)

endif()
