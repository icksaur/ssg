target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/Keymap.cpp
    ${SSG_SOURCE_DIR}/src/CompiledKeymap.cpp
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
endif()
