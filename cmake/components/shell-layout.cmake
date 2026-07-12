target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/ui_layout.cpp
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
endif()
