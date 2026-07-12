target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/scratch_session.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_scratch_session
        ${SSG_SOURCE_DIR}/tests/test_scratch_session.cpp
    )
    target_include_directories(test_scratch_session PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_scratch_session PRIVATE ssg)
    add_test(NAME test_scratch_session COMMAND test_scratch_session)
endif()
