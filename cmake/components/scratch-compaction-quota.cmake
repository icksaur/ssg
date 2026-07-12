target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/scratch.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_scratch
        ${SSG_SOURCE_DIR}/tests/test_scratch.cpp
    )
    target_include_directories(test_scratch PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_scratch PRIVATE ssg)
    add_test(NAME test_scratch COMMAND test_scratch)
endif()
