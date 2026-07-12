# foundation-harness

- Spec: `doc/spec.md`, §Foundation infrastructure
- Depends: reviewed planning baseline commit
- Branch: `foundation-harness-task`

## Scope

Add strong domain/config types, standalone test helpers, sanitizer options, and
component CMake manifests that let parallel tasks add unique source/test
fragments without editing shared lists. Root CMake automatically discovers
`cmake/components/*.cmake`; each fragment contributes sources/tests to the one
library target without later edits to `CMakeLists.txt`.

## Files

`CMakeLists.txt`, `cmake/components/`, `include/ssg/types.h`,
`include/ssg/config.h`, `src/config.cpp`, `tests/test_helpers.h`,
`tests/test_types.cpp`, `tests/consumer/`

## Oracle

Valid construction/accessor round trips, invalid-construction failures
(`std::invalid_argument`), and a compile-only consumer of the public include
path. A fixture adds a new component manifest without editing shared CMake files
and proves its source and standalone test are discovered, built, and registered.
A minimal `tests/consumer/` project uses `add_subdirectory` to load SSG, links
the consumer executable against `ssg`, and confirms SSG's internal test targets
are not registered in the outer CTest (the `if(SSG_SOURCE_DIR STREQUAL
CMAKE_SOURCE_DIR)` guard is false in subdirectory mode).

## Done

The mandatory workflow in `tasks/plan.md` is complete; no editor behavior is
implemented.
