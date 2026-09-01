target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/TreeModel.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_tree
        ${SSG_SOURCE_DIR}/tests/test_tree.cpp
    )
    target_include_directories(test_tree PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_tree PRIVATE ssg_core)
    add_test(NAME test_tree COMMAND test_tree)
endif()
