target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/Widget.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_widget
        ${SSG_SOURCE_DIR}/tests/test_widget.cpp
    )
    target_link_libraries(test_widget PRIVATE ssg)
    add_test(NAME test_widget COMMAND test_widget)

    add_executable(test_ui_profile
        ${SSG_SOURCE_DIR}/tests/test_ui_profile.cpp
    )
    target_link_libraries(test_ui_profile PRIVATE ssg)
    add_test(NAME test_ui_profile COMMAND test_ui_profile)
endif()
