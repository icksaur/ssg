target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/selection.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_selection
        ${SSG_SOURCE_DIR}/tests/reference_editor.cpp
        ${SSG_SOURCE_DIR}/tests/test_selection.cpp
    )
    target_include_directories(test_selection PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_selection PRIVATE ssg)
    add_test(NAME test_selection COMMAND test_selection)
endif()
