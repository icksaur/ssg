target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/PieceTree.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_piece_tree
        ${SSG_SOURCE_DIR}/tests/test_piece_tree.cpp
    )
    target_include_directories(test_piece_tree PRIVATE
        ${SSG_SOURCE_DIR}/src
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_piece_tree PRIVATE ssg)
    add_test(NAME test_piece_tree COMMAND test_piece_tree)
endif()
