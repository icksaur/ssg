cmake_minimum_required(VERSION 3.14)
project(ssg LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ssg STATIC
    src/ssg.cpp
)

target_include_directories(ssg
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()

    add_executable(ssg_tests
        tests/test_ssg.cpp
    )
    target_link_libraries(ssg_tests PRIVATE ssg)

    add_test(NAME ssg_tests COMMAND ssg_tests)
endif()
