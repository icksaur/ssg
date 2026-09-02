if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    # Compile app-private presentation once for the TUI and its focused tests
    # without creating an embeddable library component.
    add_library(ssg_tui_objects OBJECT
        ${SSG_SOURCE_DIR}/apps/tui/GridPresenter.cpp
        ${SSG_SOURCE_DIR}/apps/tui/HitTester.cpp
        ${SSG_SOURCE_DIR}/apps/tui/Layout.cpp
        ${SSG_SOURCE_DIR}/apps/tui/PromptLayout.cpp
        ${SSG_SOURCE_DIR}/apps/tui/Renderer.cpp
        ${SSG_SOURCE_DIR}/apps/tui/StatusFieldGrid.cpp
        ${SSG_SOURCE_DIR}/apps/tui/UiRegionProjection.cpp
        ${SSG_SOURCE_DIR}/apps/tui/WidgetLayout.cpp
    )
    target_include_directories(ssg_tui_objects PUBLIC
        "${SSG_SOURCE_DIR}/apps/tui/include")
    target_link_libraries(ssg_tui_objects PUBLIC ssg_core)
    ssg_configure_component(ssg_tui_objects)
endif()
