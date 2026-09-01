target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/color.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_color
        ${SSG_SOURCE_DIR}/tests/test_color.cpp
    )
    target_include_directories(test_color PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_color PRIVATE ssg_core)
    add_test(NAME test_color COMMAND test_color)
endif()
