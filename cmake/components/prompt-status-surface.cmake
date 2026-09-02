target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/PromptSurface.cpp
    ${SSG_SOURCE_DIR}/src/StatusQueue.cpp
)
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME ssg_prompt_status_tests
        ENTRY ${SSG_SOURCE_DIR}/tests/test_prompt_status.cpp
        SYMBOL ssg_prompt_status_tests)
    ssg_test_include_directories(ssg_prompt_status_tests PRIVATE
        ${SSG_SOURCE_DIR}/src
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(ssg_prompt_status_tests PRIVATE
        SSG_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    ssg_test_link_libraries(ssg_prompt_status_tests PRIVATE ssg_tui_objects)

endif()
