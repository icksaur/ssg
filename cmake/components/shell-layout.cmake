target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/PaneTopology.cpp
)
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_pane_topology
        ${SSG_SOURCE_DIR}/tests/test_pane_topology.cpp
    )
    target_include_directories(test_pane_topology PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_pane_topology PRIVATE ssg_core)
    add_test(NAME test_pane_topology COMMAND test_pane_topology)

    add_executable(test_layout_solver
        ${SSG_SOURCE_DIR}/tests/test_layout_solver.cpp
    )
    target_link_libraries(test_layout_solver PRIVATE ssg_tui_objects)
    add_test(NAME test_layout_solver COMMAND test_layout_solver)
endif()
