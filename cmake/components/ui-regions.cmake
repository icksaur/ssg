target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/UiStateResolver.cpp
    ${SSG_SOURCE_DIR}/src/WholeScreenAssembly.cpp
)
target_sources(ssg_grid PRIVATE
    ${SSG_SOURCE_DIR}/src/UiRegionProjection.cpp
    ${SSG_SOURCE_DIR}/src/StatusFieldGrid.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_whole_screen_structure
        ${SSG_SOURCE_DIR}/tests/test_whole_screen_structure.cpp
    )
    target_link_libraries(test_whole_screen_structure PRIVATE ssg_core)
    add_test(NAME test_whole_screen_structure COMMAND test_whole_screen_structure)
endif()
