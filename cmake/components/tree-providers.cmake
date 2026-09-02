target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/TreeModel.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_tree
        ENTRY ${SSG_SOURCE_DIR}/tests/test_tree.cpp
        SYMBOL test_tree)
    ssg_test_include_directories(test_tree PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_tree PRIVATE ssg_core)

endif()
