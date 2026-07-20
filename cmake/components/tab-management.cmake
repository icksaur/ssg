target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/TabManager.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_tabs
        ${SSG_SOURCE_DIR}/tests/test_tabs.cpp
    )
    target_link_libraries(test_tabs PRIVATE ssg)
    add_test(NAME test_tabs COMMAND test_tabs)
endif()
