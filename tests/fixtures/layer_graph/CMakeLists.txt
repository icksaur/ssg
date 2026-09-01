cmake_minimum_required(VERSION 3.20)
project(ssg_layer_graph_fixture CXX)

include("${SSG_SOURCE_DIR}/cmake/ssg_layers.cmake")

file(MAKE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/core/ssg"
    "${CMAKE_CURRENT_BINARY_DIR}/protocol/ssg"
    "${CMAKE_CURRENT_BINARY_DIR}/unexpected")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/core/ssg/Core.h" "#pragma once\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/protocol/ssg/Protocol.h" "#pragma once\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/core.cpp" "int core_fixture;\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/protocol.cpp" "int protocol_fixture;\n")

add_library(ssg_fixture_core STATIC "${CMAKE_CURRENT_BINARY_DIR}/core.cpp")
add_library(ssg_fixture_protocol STATIC "${CMAKE_CURRENT_BINARY_DIR}/protocol.cpp")

ssg_declare_layer(
    TARGET ssg_fixture_core
    ROOT "${CMAKE_CURRENT_BINARY_DIR}/core")
ssg_declare_layer(
    TARGET ssg_fixture_protocol
    ROOT "${CMAKE_CURRENT_BINARY_DIR}/protocol"
    ALLOWS ssg_fixture_core)

if(SSG_LAYER_FIXTURE_MODE STREQUAL "edge")
    target_link_libraries(ssg_fixture_core PRIVATE ssg_fixture_protocol)
elseif(SSG_LAYER_FIXTURE_MODE STREQUAL "root")
    target_include_directories(
        ssg_fixture_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/unexpected")
else()
    message(FATAL_ERROR "unknown SSG_LAYER_FIXTURE_MODE")
endif()

ssg_assert_layer_graph()
