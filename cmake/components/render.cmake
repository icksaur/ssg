# The authoritative cell renderer, compiled into the ssg library so every client
# consumes the same snapshot -> CellGrid transform.

target_sources(ssg PRIVATE ${SSG_SOURCE_DIR}/src/render.cpp)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_render ${SSG_SOURCE_DIR}/tests/test_render.cpp)
    target_include_directories(test_render PRIVATE ${SSG_SOURCE_DIR}/tests)
    target_link_libraries(test_render PRIVATE ssg)
    add_test(NAME test_render COMMAND test_render)
endif()
