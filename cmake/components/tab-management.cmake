target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/TabManager.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_tabs
        ENTRY ${SSG_SOURCE_DIR}/tests/test_tabs.cpp
        SYMBOL test_tabs)
    ssg_test_link_libraries(test_tabs PRIVATE ssg_core)

endif()
