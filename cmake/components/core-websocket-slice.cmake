target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/snapshot.cpp
)
target_sources(ssg_protocol PRIVATE
    ${SSG_SOURCE_DIR}/src/Protocol.cpp
    ${SSG_SOURCE_DIR}/src/protocol/editor_state_codec.cpp
    ${SSG_SOURCE_DIR}/src/protocol/session_codec.cpp
    ${SSG_SOURCE_DIR}/src/protocol/ui_codec.cpp
    ${SSG_SOURCE_DIR}/src/protocol/workspace_state_codec.cpp
    ${SSG_SOURCE_DIR}/src/UiTreeProtocol.cpp
    ${SSG_SOURCE_DIR}/src/UiStateProtocol.cpp
    ${SSG_SOURCE_DIR}/src/PresenceProtocol.cpp
    ${SSG_SOURCE_DIR}/src/PaletteProtocol.cpp
)

if(NOT TARGET http)
    add_subdirectory(${SSG_SOURCE_DIR}/../http
                     ${CMAKE_CURRENT_BINARY_DIR}/ssg-http
                     EXCLUDE_FROM_ALL)
endif()

add_library(ssg_http_server STATIC
    ${SSG_SOURCE_DIR}/src/HttpEditorServer.cpp
)
target_include_directories(ssg_http_server PUBLIC
    ${SSG_SOURCE_DIR}/include/transport
)
target_link_libraries(ssg_http_server PUBLIC ssg_protocol http PRIVATE ssg_core)
ssg_configure_component(ssg_http_server)
ssg_declare_layer(
    TARGET ssg_http_server
    ROOT "${SSG_SOURCE_DIR}/include/transport"
    ALLOWS ssg_protocol ssg_core)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_ui_tree_protocol
        ${SSG_SOURCE_DIR}/tests/test_ui_tree_protocol.cpp
    )
    target_link_libraries(test_ui_tree_protocol PRIVATE ssg_protocol)
    add_test(NAME test_ui_tree_protocol COMMAND test_ui_tree_protocol)

    add_executable(test_ui_state_protocol
        ${SSG_SOURCE_DIR}/tests/test_ui_state_protocol.cpp
    )
    target_link_libraries(test_ui_state_protocol PRIVATE ssg_protocol)
    add_test(NAME test_ui_state_protocol COMMAND test_ui_state_protocol)

    add_executable(test_presence_protocol
        ${SSG_SOURCE_DIR}/tests/test_presence_protocol.cpp
    )
    target_link_libraries(test_presence_protocol PRIVATE ssg_protocol)
    add_test(NAME test_presence_protocol COMMAND test_presence_protocol)

    add_executable(test_palette_protocol
        ${SSG_SOURCE_DIR}/tests/test_palette_protocol.cpp
    )
    target_link_libraries(test_palette_protocol PRIVATE ssg_protocol)
    add_test(NAME test_palette_protocol COMMAND test_palette_protocol)
endif()
