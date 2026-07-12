if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_http_server
        ${SSG_SOURCE_DIR}/tests/test_http_server.cpp
    )
    target_include_directories(test_http_server PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_http_server PRIVATE ssg_http_server)
    add_test(NAME test_http_server COMMAND test_http_server)
endif()
