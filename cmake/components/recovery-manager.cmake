target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/RecoveryManager.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_recovery
        ENTRY ${SSG_SOURCE_DIR}/tests/test_recovery.cpp
        SYMBOL test_recovery)
    ssg_test_include_directories(test_recovery PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_recovery PRIVATE ssg_core)

endif()
