target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/snapshot.cpp
    ${SSG_SOURCE_DIR}/src/protocol.cpp
)

if(NOT TARGET http)
    add_subdirectory(${SSG_SOURCE_DIR}/../http
                     ${CMAKE_CURRENT_BINARY_DIR}/ssg-http
                     EXCLUDE_FROM_ALL)
endif()

add_library(ssg_http_server STATIC
    ${SSG_SOURCE_DIR}/src/http_server.cpp
)
target_include_directories(ssg_http_server PUBLIC
    ${SSG_SOURCE_DIR}/include
)
target_link_libraries(ssg_http_server PUBLIC ssg http)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(ssg_core_websocket_slice_tests
        ${SSG_SOURCE_DIR}/tests/test_core_websocket_slice.cpp
    )
    target_link_libraries(ssg_core_websocket_slice_tests
        PRIVATE ssg_http_server
    )
    add_test(NAME ssg_core_websocket_slice
             COMMAND ssg_core_websocket_slice_tests)
endif()
