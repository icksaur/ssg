# The \`ssg\` terminal editor application.  Owns terminal I/O only; all editor,
# layout, and rendering behavior is in the ssg library.

# The web client asset (served by `ssg --http`) is embedded into the binary at
# build so a node test can exercise the real reconcile.mjs and the markup/JS stay
# readable files. SSG_WEB_ASSET_TU is reused by ssg_startup_probe, which compiles
# the same http_serve.cpp; component includes share scope, and this manifest
# sorts before startup-benchmark.cmake.
set(_SSG_WEB_ASSETS
    "index.html=${SSG_SOURCE_DIR}/apps/web/index.html"
    "client.mjs=${SSG_SOURCE_DIR}/apps/web/client.mjs"
    "reconcile.mjs=${SSG_SOURCE_DIR}/apps/web/reconcile.mjs"
)
set(_SSG_WEB_ASSET_FILES
    ${SSG_SOURCE_DIR}/apps/web/index.html
    ${SSG_SOURCE_DIR}/apps/web/client.mjs
    ${SSG_SOURCE_DIR}/apps/web/reconcile.mjs
)
set(SSG_WEB_ASSET_TU ${CMAKE_BINARY_DIR}/generated/web_assets.cpp)
set(_SSG_WEB_ASSET_SPEC ${CMAKE_BINARY_DIR}/generated/web_assets.spec)
string(JOIN "\n" _SSG_WEB_ASSET_TEXT ${_SSG_WEB_ASSETS})
file(GENERATE OUTPUT ${_SSG_WEB_ASSET_SPEC} CONTENT "${_SSG_WEB_ASSET_TEXT}\n")
add_custom_command(
    OUTPUT ${SSG_WEB_ASSET_TU}
    COMMAND ${CMAKE_COMMAND}
        -DEMBED_SPEC_FILE=${_SSG_WEB_ASSET_SPEC}
        -DEMBED_OUTPUT=${SSG_WEB_ASSET_TU}
        -DEMBED_NAMESPACE=ssg::app
        -DEMBED_ACCESSOR=embeddedWebAsset
        -P ${SSG_SOURCE_DIR}/cmake/embed_text.cmake
    DEPENDS ${_SSG_WEB_ASSET_FILES}
            ${_SSG_WEB_ASSET_SPEC}
            ${SSG_SOURCE_DIR}/cmake/embed_text.cmake
    COMMENT "Embedding ssg web client asset"
    VERBATIM)

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
endif()
