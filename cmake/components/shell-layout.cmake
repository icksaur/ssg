target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/Layout.cpp
    ${SSG_SOURCE_DIR}/src/ShellState.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(ssg_ui_layout_tests
        ${SSG_SOURCE_DIR}/tests/test_ui_layout.cpp
    )
    target_link_libraries(ssg_ui_layout_tests PRIVATE ssg)
    target_compile_definitions(ssg_ui_layout_tests PRIVATE
        SSG_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    add_test(NAME ssg_ui_layout_tests COMMAND ssg_ui_layout_tests)

    add_executable(test_layout_solver
        ${SSG_SOURCE_DIR}/tests/test_layout_solver.cpp
    )
    target_link_libraries(test_layout_solver PRIVATE ssg)
    add_test(NAME test_layout_solver COMMAND test_layout_solver)
endif()
