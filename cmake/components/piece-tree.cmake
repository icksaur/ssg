target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/PieceTree.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_piece_tree
        ENTRY ${SSG_SOURCE_DIR}/tests/test_piece_tree.cpp
        SYMBOL test_piece_tree)
    ssg_test_include_directories(test_piece_tree PRIVATE
        ${SSG_SOURCE_DIR}/src
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_piece_tree PRIVATE ssg_core)

endif()
