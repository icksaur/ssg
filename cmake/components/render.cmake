# The authoritative cell renderer, compiled into the ssg library so every client
# consumes the same snapshot -> CellGrid transform.

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_render
        ENTRY ${SSG_SOURCE_DIR}/tests/test_render.cpp
        SYMBOL test_render)
    ssg_test_include_directories(test_render PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_render PRIVATE ssg_tui_objects)

endif()
