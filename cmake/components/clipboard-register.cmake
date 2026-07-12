target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/clipboard.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_clipboard
        ${SSG_SOURCE_DIR}/tests/test_clipboard.cpp
    )
    target_link_libraries(test_clipboard PRIVATE ssg)
    add_test(NAME test_clipboard COMMAND test_clipboard)
endif()
