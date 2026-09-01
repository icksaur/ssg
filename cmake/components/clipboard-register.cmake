target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ClipboardRegister.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_clipboard
        ${SSG_SOURCE_DIR}/tests/test_clipboard.cpp
    )
    target_link_libraries(test_clipboard PRIVATE ssg_core)
    add_test(NAME test_clipboard COMMAND test_clipboard)
endif()
