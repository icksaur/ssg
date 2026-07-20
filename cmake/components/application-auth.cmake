target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/ApplicationAuthentication.cpp
    $<$<PLATFORM_ID:Windows>:${SSG_SOURCE_DIR}/src/platform/secure_random_windows.cpp>
    $<$<PLATFORM_ID:Linux>:${SSG_SOURCE_DIR}/src/platform/secure_random_linux.cpp>
)

if(WIN32)
    target_link_libraries(ssg PUBLIC bcrypt)
endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_application_auth
        ${SSG_SOURCE_DIR}/tests/test_application_auth.cpp
    )
    target_include_directories(test_application_auth PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_application_auth PRIVATE ssg_http_server)
    add_test(NAME test_application_auth COMMAND test_application_auth)
endif()
