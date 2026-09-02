target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ScratchStore.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_scratch
        ENTRY ${SSG_SOURCE_DIR}/tests/test_scratch.cpp
        SYMBOL test_scratch)
    ssg_test_include_directories(test_scratch PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_scratch PRIVATE ssg_core)

endif()
