target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/ScriptHost.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_script_host
        ${SSG_SOURCE_DIR}/tests/test_script_host.cpp
    )
    target_include_directories(test_script_host PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_script_host PRIVATE ssg)
    add_test(NAME test_script_host COMMAND test_script_host)
endif()
