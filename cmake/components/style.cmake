target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Style.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_style
        ${SSG_SOURCE_DIR}/tests/test_style.cpp
    )
    target_include_directories(test_style PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_compile_definitions(test_style PRIVATE
        SSG_TEST_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    target_link_libraries(test_style PRIVATE ssg_core)
    add_test(NAME test_style COMMAND test_style)
endif()
