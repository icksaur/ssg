target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ExternalModificationFlow.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_external_modification
        ENTRY ${SSG_SOURCE_DIR}/tests/test_external_modification.cpp
        SYMBOL test_external_modification)
    ssg_test_include_directories(test_external_modification PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_external_modification PRIVATE ssg_core)

endif()
