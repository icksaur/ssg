target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Keymap.cpp
    ${SSG_SOURCE_DIR}/src/CompiledKeymap.cpp
    ${SSG_SOURCE_DIR}/src/PromptRouting.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_input
        ENTRY ${SSG_SOURCE_DIR}/tests/test_input.cpp
        SYMBOL test_input)
    ssg_test_link_libraries(test_input PRIVATE ssg_core)
    ssg_test_include_directories(test_input PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_input PRIVATE
        SSG_SOURCE_PATH="${SSG_SOURCE_DIR}/src"
    )

    ssg_add_test_suite(
        NAME test_prompt_routing
        ENTRY ${SSG_SOURCE_DIR}/tests/test_prompt_routing.cpp
        SYMBOL test_prompt_routing)
    ssg_test_link_libraries(test_prompt_routing PRIVATE ssg_core)
    ssg_test_include_directories(test_prompt_routing PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )


endif()
