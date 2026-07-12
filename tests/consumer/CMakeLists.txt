cmake_minimum_required(VERSION 3.14)
project(ssg_consumer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Oracle: verify SSG can be consumed via add_subdirectory.
#
# Expected outcomes:
#   1. SSG configures and builds without error.
#   2. The consumer executable links against ssg and includes its headers.
#   3. SSG's internal test targets (ssg_tests, test_types) are NOT registered
#      here — SSG_SOURCE_DIR != CMAKE_SOURCE_DIR in subdirectory mode, so the
#      manifest guard skips test registration.
#   4. CTest for this project finds zero tests (SSG tests are not visible).
#
# Run this oracle with:
#   cmake -S tests/consumer -B build-consumer
#   cmake --build build-consumer --target consumer
#   ./build-consumer/consumer     # exits 0
#   ctest --test-dir build-consumer -N  # lists no tests

add_subdirectory(../.. ssg_subdirectory_build)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE ssg)
