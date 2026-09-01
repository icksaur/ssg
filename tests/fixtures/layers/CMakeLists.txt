function(ssg_add_layer_fixture name owner source)
    add_executable(layer_${name} EXCLUDE_FROM_ALL "${source}")
    target_link_libraries(layer_${name} PRIVATE ${owner})
endfunction()

ssg_add_layer_fixture(platform_standalone ssg_platform allowed_platform.cpp)
ssg_add_layer_fixture(core_to_platform ssg_core allowed_core_platform.cpp)
ssg_add_layer_fixture(grid_to_core ssg_grid allowed_grid_core.cpp)

set(_SSG_ALLOWED_LAYER_FIXTURES
    platform_standalone
    core_to_platform
    grid_to_core)

set(_SSG_FORBIDDEN_LAYER_FIXTURES
    core_to_grid:Renderer.h
    platform_to_core:EditorSession.h
    platform_to_grid:Renderer.h)

foreach(_fixture IN LISTS _SSG_ALLOWED_LAYER_FIXTURES)
    add_test(NAME test_layer_include_${_fixture}
        COMMAND ${CMAKE_COMMAND}
            -DSSG_LAYER_BINARY_DIR=${CMAKE_BINARY_DIR}
            -DSSG_LAYER_TARGET=layer_${_fixture}
            -DSSG_LAYER_EXPECT_SUCCESS=ON
            -P ${SSG_SOURCE_DIR}/cmake/run_layer_fixture.cmake)
endforeach()

foreach(_entry IN LISTS _SSG_FORBIDDEN_LAYER_FIXTURES)
    string(REPLACE ":" ";" _fields "${_entry}")
    list(GET _fields 0 _fixture)
    list(GET _fields 1 _header)
    string(REGEX REPLACE "_to_.*$" "" _owner "${_fixture}")
    set(_owner_target ssg_${_owner})
    ssg_add_layer_fixture(
        ${_fixture} ${_owner_target} forbidden_${_fixture}.cpp)
    add_test(NAME test_layer_include_${_fixture}
        COMMAND ${CMAKE_COMMAND}
            -DSSG_LAYER_BINARY_DIR=${CMAKE_BINARY_DIR}
            -DSSG_LAYER_TARGET=layer_${_fixture}
            -DSSG_LAYER_HEADER=${_header}
            -DSSG_LAYER_EXPECT_SUCCESS=OFF
            -P ${SSG_SOURCE_DIR}/cmake/run_layer_fixture.cmake)
endforeach()
