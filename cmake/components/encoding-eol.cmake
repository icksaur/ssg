target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/TextCodec.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_text_encoding
        ENTRY ${SSG_SOURCE_DIR}/tests/test_text_encoding.cpp
        SYMBOL test_text_encoding)
    ssg_test_include_directories(test_text_encoding PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_text_encoding PRIVATE
        SSG_ENCODING_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/encoding"
    )
    ssg_test_link_libraries(test_text_encoding PRIVATE ssg_core)

endif()
