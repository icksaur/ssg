target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/CommandRegistry.cpp
    ${SSG_SOURCE_DIR}/src/EditorSession.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_session
        ${SSG_SOURCE_DIR}/tests/test_session.cpp
    )
    target_include_directories(test_session PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_session PRIVATE ssg)
    add_test(NAME test_session COMMAND test_session)
endif()
