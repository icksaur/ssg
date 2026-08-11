target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/Keymap.cpp
    ${SSG_SOURCE_DIR}/src/CompiledKeymap.cpp
    ${SSG_SOURCE_DIR}/src/PromptRouting.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_input
        ${SSG_SOURCE_DIR}/tests/test_input.cpp
    )
    target_link_libraries(test_input PRIVATE ssg)
    target_include_directories(test_input PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_input PRIVATE
        SSG_SOURCE_PATH="${SSG_SOURCE_DIR}/src"
    )
    add_test(NAME test_input COMMAND test_input)

    add_executable(test_prompt_routing
        ${SSG_SOURCE_DIR}/tests/test_prompt_routing.cpp
    )
    target_link_libraries(test_prompt_routing PRIVATE ssg)
    target_include_directories(test_prompt_routing PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    add_test(NAME test_prompt_routing COMMAND test_prompt_routing)
endif()
