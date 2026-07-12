target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/history.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_history
        ${SSG_SOURCE_DIR}/tests/reference_editor.cpp
        ${SSG_SOURCE_DIR}/tests/test_history.cpp
    )
    target_include_directories(test_history PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_history PRIVATE ssg)
    add_test(NAME test_history COMMAND test_history)
endif()
