target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/HitTester.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_hit_test
        ${SSG_SOURCE_DIR}/tests/test_hit_test.cpp
    )
    target_include_directories(test_hit_test PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_hit_test PRIVATE ssg)
    add_test(NAME test_hit_test COMMAND test_hit_test)
endif()
