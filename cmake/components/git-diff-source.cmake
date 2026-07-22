target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/GitDiffSource.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_git_diff_source
        ${SSG_SOURCE_DIR}/tests/test_git_diff_source.cpp
    )
    target_include_directories(test_git_diff_source PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_git_diff_source PRIVATE ssg)
    add_test(NAME test_git_diff_source COMMAND test_git_diff_source)
endif()
