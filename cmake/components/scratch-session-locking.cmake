target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ScratchSession.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_scratch_session
        ENTRY ${SSG_SOURCE_DIR}/tests/test_scratch_session.cpp
        SYMBOL test_scratch_session
        ARGS)
    ssg_test_include_directories(test_scratch_session PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_scratch_session PRIVATE ssg_core)

endif()
