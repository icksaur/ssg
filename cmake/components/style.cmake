target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/Style.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_style
        ${SSG_SOURCE_DIR}/tests/test_style.cpp
    )
    target_include_directories(test_style PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_style PRIVATE ssg)
    add_test(NAME test_style COMMAND test_style)
endif()
