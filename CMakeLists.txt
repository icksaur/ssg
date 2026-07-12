cmake_minimum_required(VERSION 3.14)
project(ssg LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sanitizer toggle: -DSSG_SANITIZE=ON enables AddressSanitizer + UBSanitizer.
# Requires Clang or GCC.  Applied as PUBLIC so linked test executables inherit
# the flags and link options.
option(SSG_SANITIZE "Build with AddressSanitizer and UBSanitizer" OFF)

# enable_testing() must precede the component-manifest loop so manifests can
# register test executables with add_test() unconditionally inside the guard.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
endif()

add_library(ssg STATIC
    src/ssg.cpp
)

target_include_directories(ssg
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(SSG_SANITIZE)
    target_compile_options(ssg PUBLIC
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -g
    )
    target_link_options(ssg PUBLIC
        -fsanitize=address,undefined
    )
endif()

# Auto-discover component manifests.  CONFIGURE_DEPENDS causes Ninja and
# Makefile generators to re-glob when cmake/components/ changes, so new
# manifests are discovered on the next build invocation without a manual
# cmake run.  Note: the Visual Studio and Xcode generators do not reliably
# re-check globs at build time; with those generators a manual reconfigure
# ("cmake -S . -B build") is required after adding a manifest.  Use Ninja
# or a Makefile generator when the auto-discovery guarantee is required
# (e.g., on Windows with parity gates, prefer -G Ninja).
#
# Manifest contract — a fragment in cmake/components/*.cmake may:
#   1. Call target_sources(ssg PRIVATE <path>) to add sources to the library.
#      Use ${CMAKE_SOURCE_DIR}/<rel-path> for project-root-relative paths.
#   2. Inside "if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)":
#      create a standalone test executable, link it against ssg, and register
#      it with add_test().  Test target names must be unique across manifests.
#   3. Not create additional library targets or modify global state.
# Manifests are included in sorted order for a deterministic build.
file(GLOB _SSG_COMPONENT_MANIFESTS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/components/*.cmake"
)
list(SORT _SSG_COMPONENT_MANIFESTS)
foreach(_manifest ${_SSG_COMPONENT_MANIFESTS})
    include("${_manifest}")
endforeach()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(ssg_tests
        tests/test_ssg.cpp
    )
    target_link_libraries(ssg_tests PRIVATE ssg)
    add_test(NAME ssg_tests COMMAND ssg_tests)
endif()
