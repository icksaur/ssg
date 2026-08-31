target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/Widget.cpp
    ${SSG_SOURCE_DIR}/src/UiTree.cpp
    ${SSG_SOURCE_DIR}/src/ViewSurfaceBacking.cpp
    ${SSG_SOURCE_DIR}/src/MutationPatch.cpp
    ${SSG_SOURCE_DIR}/src/UiPresence.cpp
    ${SSG_SOURCE_DIR}/src/UiFrame.cpp
    ${SSG_SOURCE_DIR}/src/WholeScreenSchema.cpp
    ${SSG_SOURCE_DIR}/src/WholeScreenInteraction.cpp
    ${SSG_SOURCE_DIR}/src/CommandTransition.cpp
    ${SSG_SOURCE_DIR}/src/InteractionAuthority.cpp
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

    add_executable(test_ui_frame
        ${SSG_SOURCE_DIR}/tests/test_ui_frame.cpp
    )
    target_link_libraries(test_ui_frame PRIVATE ssg)
    add_test(NAME test_ui_frame COMMAND test_ui_frame)

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

    add_executable(test_whole_screen_interaction
        ${SSG_SOURCE_DIR}/tests/test_whole_screen_interaction.cpp
    )
    target_link_libraries(test_whole_screen_interaction PRIVATE ssg)
    add_test(NAME test_whole_screen_interaction
             COMMAND test_whole_screen_interaction)

    add_executable(test_command_transition
        ${SSG_SOURCE_DIR}/tests/test_command_transition.cpp
    )
    target_link_libraries(test_command_transition PRIVATE ssg)
    add_test(NAME test_command_transition COMMAND test_command_transition)

    add_executable(test_interaction_authority
        ${SSG_SOURCE_DIR}/tests/test_interaction_authority.cpp
    )
    target_link_libraries(test_interaction_authority PRIVATE ssg)
    add_test(NAME test_interaction_authority COMMAND test_interaction_authority)
endif()
