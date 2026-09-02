target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/PromptSurface.cpp
    ${SSG_SOURCE_DIR}/src/StatusQueue.cpp
)
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(ssg_prompt_status_tests
        ${SSG_SOURCE_DIR}/tests/test_prompt_status.cpp
    )
    target_include_directories(ssg_prompt_status_tests PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(ssg_prompt_status_tests PRIVATE
        SSG_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    target_link_libraries(ssg_prompt_status_tests PRIVATE ssg_tui_objects)
    add_test(NAME ssg_prompt_status_tests COMMAND ssg_prompt_status_tests)
endif()
