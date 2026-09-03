target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/WholeScreenAssembly.cpp
)
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_whole_screen_structure
        ENTRY ${SSG_SOURCE_DIR}/tests/test_whole_screen_structure.cpp
        SYMBOL test_whole_screen_structure)
    ssg_test_link_libraries(test_whole_screen_structure PRIVATE ssg_core)

endif()
