# The \`ssg\` terminal editor application.  Owns terminal I/O only; all editor,
# layout, and rendering behavior is in the ssg library.
#
# The web client asset served by `ssg --http` is embedded by
# app-web-asset.cmake, which sorts before this manifest and defines
# SSG_WEB_ASSET_TU. Both ssg_app and ssg_startup_probe compile it in.

if(NOT DEFINED SSG_WEB_ASSET_TU)
    message(FATAL_ERROR
        "SSG_WEB_ASSET_TU is unset: app-web-asset.cmake must be included before "
        "ssg-app.cmake")
endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(ssg_app
        ${SSG_SOURCE_DIR}/apps/ssg_main.cpp
        ${SSG_SOURCE_DIR}/apps/ssg_terminal.cpp
        ${SSG_SOURCE_DIR}/apps/pointer_routing.cpp
        ${SSG_SOURCE_DIR}/apps/init_script.cpp
        ${SSG_SOURCE_DIR}/apps/http_serve.cpp
        ${SSG_WEB_ASSET_TU}
    )
    set_target_properties(ssg_app PROPERTIES OUTPUT_NAME ssg)
    target_include_directories(ssg_app PRIVATE ${SSG_SOURCE_DIR}/apps)
    target_link_libraries(ssg_app PRIVATE ssg http ssg_http_server)

    add_executable(test_ssg_app
        ${SSG_SOURCE_DIR}/tests/test_ssg_app.cpp
        ${SSG_SOURCE_DIR}/apps/ssg_terminal.cpp
        ${SSG_SOURCE_DIR}/apps/pointer_routing.cpp
        ${SSG_SOURCE_DIR}/apps/init_script.cpp
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

    # The web client's local-echo reconciliation is exercised by running the real
    # reconcile.mjs under node -- the client code the browser ships, not a copy.
    # Guarded by node's presence: absence skips (not fails) the test, since node
    # is not a build dependency of the C++ editor.
    find_program(SSG_NODE_EXECUTABLE node)
    if(SSG_NODE_EXECUTABLE)
        add_test(NAME test_web_reconcile
                 COMMAND ${SSG_NODE_EXECUTABLE}
                         ${SSG_SOURCE_DIR}/tests/web/test_reconcile.mjs)
    else()
        message(WARNING
            "node not found: the web client's local-echo reconciliation oracle "
            "(test_web_reconcile) is SKIPPED, leaving reconcile.mjs untested on "
            "this configuration")
    endif()
endif()
