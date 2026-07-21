target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/SyntaxModel.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_syntax
        ${SSG_SOURCE_DIR}/tests/test_syntax.cpp
    )
    target_include_directories(test_syntax PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_syntax PRIVATE ssg)
    add_test(NAME test_syntax COMMAND test_syntax)

    add_executable(test_syntax_language_detection
        ${SSG_SOURCE_DIR}/tests/test_syntax_language_detection.cpp
    )
    target_include_directories(test_syntax_language_detection PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_syntax_language_detection PRIVATE ssg)
    add_test(NAME test_syntax_language_detection COMMAND test_syntax_language_detection)
endif()
