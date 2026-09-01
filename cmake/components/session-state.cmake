target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/CommandInvocation.cpp
    ${SSG_SOURCE_DIR}/src/runtime/command_executor.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_session
        ${SSG_SOURCE_DIR}/tests/test_session.cpp
    )
    target_include_directories(test_session PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_session PRIVATE ssg_core)
    add_test(NAME test_session COMMAND test_session)
endif()
