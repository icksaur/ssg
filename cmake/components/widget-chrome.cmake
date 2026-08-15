target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/Widget.cpp
    ${SSG_SOURCE_DIR}/src/UiTree.cpp
    ${SSG_SOURCE_DIR}/src/ViewSurfaceBacking.cpp
    ${SSG_SOURCE_DIR}/src/MutationPatch.cpp
    ${SSG_SOURCE_DIR}/src/UiPresence.cpp
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

    add_executable(test_ui_tree
        ${SSG_SOURCE_DIR}/tests/test_ui_tree.cpp
    )
    target_link_libraries(test_ui_tree PRIVATE ssg)
    add_test(NAME test_ui_tree COMMAND test_ui_tree)

    add_executable(test_ui_node_state
        ${SSG_SOURCE_DIR}/tests/test_ui_node_state.cpp
    )
    target_link_libraries(test_ui_node_state PRIVATE ssg)
    add_test(NAME test_ui_node_state COMMAND test_ui_node_state)

    add_executable(test_mutation_patch
        ${SSG_SOURCE_DIR}/tests/test_mutation_patch.cpp
    )
    target_link_libraries(test_mutation_patch PRIVATE ssg)
    add_test(NAME test_mutation_patch COMMAND test_mutation_patch)

    add_executable(test_keyboard_focus
        ${SSG_SOURCE_DIR}/tests/test_keyboard_focus.cpp
    )
    target_link_libraries(test_keyboard_focus PRIVATE ssg)
    add_test(NAME test_keyboard_focus COMMAND test_keyboard_focus)

    add_executable(test_interaction_state
        ${SSG_SOURCE_DIR}/tests/test_interaction_state.cpp
    )
    target_link_libraries(test_interaction_state PRIVATE ssg)
    add_test(NAME test_interaction_state COMMAND test_interaction_state)

    add_executable(test_ui_view_surface
        ${SSG_SOURCE_DIR}/tests/test_ui_view_surface.cpp
    )
    target_link_libraries(test_ui_view_surface PRIVATE ssg)
    add_test(NAME test_ui_view_surface COMMAND test_ui_view_surface)
endif()
