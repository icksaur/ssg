target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Widget.cpp
    ${SSG_SOURCE_DIR}/src/UiTree.cpp
    ${SSG_SOURCE_DIR}/src/WholeScreenSchema.cpp
    ${SSG_SOURCE_DIR}/src/WholeScreenInteraction.cpp
    ${SSG_SOURCE_DIR}/src/runtime/interaction.cpp
)
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_widget
        ENTRY ${SSG_SOURCE_DIR}/tests/test_widget.cpp
        SYMBOL test_widget)
    ssg_test_link_libraries(test_widget PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_widget_layout
        ENTRY ${SSG_SOURCE_DIR}/tests/test_widget_layout.cpp
        SYMBOL test_widget_layout)
    ssg_test_link_libraries(test_widget_layout PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_ui_tree
        ENTRY ${SSG_SOURCE_DIR}/tests/test_ui_tree.cpp
        SYMBOL test_ui_tree)
    ssg_test_link_libraries(test_ui_tree PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_ui_node_state
        ENTRY ${SSG_SOURCE_DIR}/tests/test_ui_node_state.cpp
        SYMBOL test_ui_node_state)
    ssg_test_link_libraries(test_ui_node_state PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_keyboard_focus
        ENTRY ${SSG_SOURCE_DIR}/tests/test_keyboard_focus.cpp
        SYMBOL test_keyboard_focus)
    ssg_test_link_libraries(test_keyboard_focus PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_ui_view_surface
        ENTRY ${SSG_SOURCE_DIR}/tests/test_ui_view_surface.cpp
        SYMBOL test_ui_view_surface)
    ssg_test_link_libraries(test_ui_view_surface PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_interaction_authority
        ENTRY ${SSG_SOURCE_DIR}/tests/test_interaction_authority.cpp
        SYMBOL test_interaction_authority)
    ssg_test_link_libraries(test_interaction_authority PRIVATE ssg_core)

endif()
