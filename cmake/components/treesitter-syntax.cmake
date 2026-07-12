target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/syntax.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_syntax
        ${SSG_SOURCE_DIR}/tests/test_syntax.cpp
    )
    target_include_directories(test_syntax PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_syntax PRIVATE ssg)
    add_test(NAME test_syntax COMMAND test_syntax)
endif()
