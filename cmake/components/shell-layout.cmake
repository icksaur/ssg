target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/PaneTopology.cpp
)
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_pane_topology
        ENTRY ${SSG_SOURCE_DIR}/tests/test_pane_topology.cpp
        SYMBOL test_pane_topology)
    ssg_test_include_directories(test_pane_topology PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_pane_topology PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_layout_solver
        ENTRY ${SSG_SOURCE_DIR}/tests/test_layout_solver.cpp
        SYMBOL test_layout_solver)
    ssg_test_link_libraries(test_layout_solver PRIVATE ssg_tui_objects)

endif()
