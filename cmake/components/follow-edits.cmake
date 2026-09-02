target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/FollowEditsModel.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_follow_edits
        ENTRY ${SSG_SOURCE_DIR}/tests/test_follow_edits.cpp
        SYMBOL test_follow_edits)
    ssg_test_include_directories(test_follow_edits PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_follow_edits PRIVATE
        SSG_FOLLOW_EDITS_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/follow_edits"
    )
    ssg_test_link_libraries(test_follow_edits PRIVATE ssg_core)

endif()
