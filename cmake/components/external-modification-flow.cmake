target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/ExternalModificationFlow.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_external_modification
        ${SSG_SOURCE_DIR}/tests/test_external_modification.cpp
    )
    target_include_directories(test_external_modification PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_external_modification PRIVATE ssg)
    add_test(NAME test_external_modification COMMAND test_external_modification)
endif()
