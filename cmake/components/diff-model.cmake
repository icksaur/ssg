target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/DiffModel.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_diff
        ${SSG_SOURCE_DIR}/tests/test_diff.cpp
    )
    target_include_directories(test_diff PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_diff PRIVATE
        SSG_DIFF_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/diff"
    )
    target_link_libraries(test_diff PRIVATE ssg)
    add_test(NAME test_diff COMMAND test_diff)
endif()
