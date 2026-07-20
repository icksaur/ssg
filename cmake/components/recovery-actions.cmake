target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/RecoveryActions.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_recovery
        ${SSG_SOURCE_DIR}/tests/test_recovery.cpp
    )
    target_include_directories(test_recovery PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_recovery PRIVATE ssg)
    add_test(NAME test_recovery COMMAND test_recovery)
endif()
