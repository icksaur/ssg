target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ClipboardRegister.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_clipboard
        ENTRY ${SSG_SOURCE_DIR}/tests/test_clipboard.cpp
        SYMBOL test_clipboard)
    ssg_test_link_libraries(test_clipboard PRIVATE ssg_core)

endif()
