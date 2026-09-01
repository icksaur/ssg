target_sources(ssg_grid PRIVATE
    ${SSG_SOURCE_DIR}/src/Layout.cpp
    ${SSG_SOURCE_DIR}/src/ShellState.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_layout_solver
        ${SSG_SOURCE_DIR}/tests/test_layout_solver.cpp
    )
    target_link_libraries(test_layout_solver PRIVATE ssg_grid)
    add_test(NAME test_layout_solver COMMAND test_layout_solver)
endif()
