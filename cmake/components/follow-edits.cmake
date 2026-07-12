target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/follow_edits.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_follow_edits
        ${SSG_SOURCE_DIR}/tests/test_follow_edits.cpp
    )
    target_include_directories(test_follow_edits PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_follow_edits PRIVATE
        SSG_FOLLOW_EDITS_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/follow_edits"
    )
    target_link_libraries(test_follow_edits PRIVATE ssg)
    add_test(NAME test_follow_edits COMMAND test_follow_edits)
endif()
