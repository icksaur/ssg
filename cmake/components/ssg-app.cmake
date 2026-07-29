# The \`ssg\` terminal editor application.  Owns terminal I/O only; all editor,
# layout, and rendering behavior is in the ssg library.

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(ssg_app
        ${SSG_SOURCE_DIR}/apps/ssg_main.cpp
        ${SSG_SOURCE_DIR}/apps/ssg_terminal.cpp
        ${SSG_SOURCE_DIR}/apps/pointer_routing.cpp
    )
    set_target_properties(ssg_app PROPERTIES OUTPUT_NAME ssg)
    target_include_directories(ssg_app PRIVATE ${SSG_SOURCE_DIR}/apps)
    target_link_libraries(ssg_app PRIVATE ssg)

    add_executable(test_ssg_app
        ${SSG_SOURCE_DIR}/tests/test_ssg_app.cpp
        ${SSG_SOURCE_DIR}/apps/ssg_terminal.cpp
        ${SSG_SOURCE_DIR}/apps/pointer_routing.cpp
    )
    target_include_directories(test_ssg_app PRIVATE
        ${SSG_SOURCE_DIR}/apps
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_ssg_app PRIVATE
        SSG_TEST_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    target_link_libraries(test_ssg_app PRIVATE ssg)
    add_test(NAME test_ssg_app COMMAND test_ssg_app)
endif()
