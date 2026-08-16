# Embeds the `ssg --http` web client (apps/web/{index.html,client.mjs,
# reconcile.mjs,fuzzy.mjs}) into the binary at build so a node test can exercise the real
# reconcile.mjs and the markup/JS stay readable files rather than a C++ string.
#
# This manifest is named to sort BEFORE ssg-app.cmake and startup-benchmark.cmake
# (both compile http_serve.cpp and consume SSG_WEB_ASSET_TU), so the variable and
# the generated TU exist before either target references them. The consumers
# assert SSG_WEB_ASSET_TU is defined, so a rename that breaks the ordering fails
# loudly at configure time rather than silently dropping the asset.

set(_SSG_WEB_ASSETS
    "index.html=${SSG_SOURCE_DIR}/apps/web/index.html"
    "client.mjs=${SSG_SOURCE_DIR}/apps/web/client.mjs"
    "reconcile.mjs=${SSG_SOURCE_DIR}/apps/web/reconcile.mjs"
    "fuzzy.mjs=${SSG_SOURCE_DIR}/apps/web/fuzzy.mjs"
)
set(_SSG_WEB_ASSET_FILES
    ${SSG_SOURCE_DIR}/apps/web/index.html
    ${SSG_SOURCE_DIR}/apps/web/client.mjs
    ${SSG_SOURCE_DIR}/apps/web/reconcile.mjs
    ${SSG_SOURCE_DIR}/apps/web/fuzzy.mjs
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
