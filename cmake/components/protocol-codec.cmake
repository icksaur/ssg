if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_protocol
        ${SSG_SOURCE_DIR}/tests/test_protocol.cpp
    )
    target_include_directories(test_protocol PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_protocol PRIVATE
        SSG_PROTOCOL_FIXTURES_DIR="${SSG_SOURCE_DIR}/tests/fixtures/protocol"
    )
    target_link_libraries(test_protocol PRIVATE ssg)
    add_test(NAME test_protocol COMMAND test_protocol)
endif()
