target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ScriptHost.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_script_host
        ENTRY ${SSG_SOURCE_DIR}/tests/test_script_host.cpp
        SYMBOL test_script_host)
    ssg_test_include_directories(test_script_host PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_script_host PRIVATE ssg_tui_objects)

endif()
