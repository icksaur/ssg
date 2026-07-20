target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/TextCodec.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_text_encoding
        ${SSG_SOURCE_DIR}/tests/test_text_encoding.cpp
    )
    target_include_directories(test_text_encoding PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_text_encoding PRIVATE
        SSG_ENCODING_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/encoding"
    )
    target_link_libraries(test_text_encoding PRIVATE ssg)
    add_test(NAME test_text_encoding COMMAND test_text_encoding)
endif()
