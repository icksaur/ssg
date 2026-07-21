cmake_minimum_required(VERSION 3.14)
project(ssg LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use ccache as the compiler launcher when available.  Fresh build trees (e.g.
# per-worktree builds) then reuse cached object files instead of recompiling from
# scratch, which turns a clean build of an already-seen tree into a near-instant
# cache replay.  Opt out with -DSSG_CCACHE=OFF.
option(SSG_CCACHE "Use ccache as the compiler launcher when found" ON)
if(SSG_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(SSG_CCACHE_PROGRAM ccache)
    if(SSG_CCACHE_PROGRAM)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${SSG_CCACHE_PROGRAM}")
    endif()
endif()

# Capture SSG's own source root immediately after project().  Manifests are
# included() from this file and share its variable scope, so they see
# SSG_SOURCE_DIR directly.  Manifests MUST use ${SSG_SOURCE_DIR} for all
# project-root-relative paths; using CMAKE_SOURCE_DIR would resolve to the
# host project root when SSG is loaded via add_subdirectory.
set(SSG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# Sanitizer toggle: -DSSG_SANITIZE=ON enables AddressSanitizer + UBSanitizer.
# Requires Clang or GCC.  Applied as PUBLIC so linked test executables inherit
# the flags and link options.
option(SSG_SANITIZE "Build with AddressSanitizer and UBSanitizer" OFF)
option(SSG_TREESITTER "Enable vendored tree-sitter syntax parsing" OFF)

# enable_testing() must precede the component-manifest loop so manifests can
# register test executables.  Skipped when SSG is a subdirectory so host
# projects are not contaminated with SSG's CTest entries.
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    enable_testing()
endif()

add_library(ssg STATIC
    src/ssg.cpp
)

target_include_directories(ssg
    PUBLIC
        ${SSG_SOURCE_DIR}/include
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
#      Use ${SSG_SOURCE_DIR}/<rel-path> for project-root-relative paths.
#      Never use CMAKE_SOURCE_DIR (breaks add_subdirectory consumers).
#   2. Inside "if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)":
#      create a standalone test executable, link it against ssg, and register
#      it with add_test().  Test target names must be unique across manifests.
#      This guard is false when SSG is a subdirectory, so consumer projects
#      are not contaminated with SSG's internal test targets.
#   3. Not create additional library targets or modify global state.
# Manifests are included in sorted order for a deterministic build.
file(GLOB _SSG_COMPONENT_MANIFESTS
    CONFIGURE_DEPENDS
    "${SSG_SOURCE_DIR}/cmake/components/*.cmake"
)
list(SORT _SSG_COMPONENT_MANIFESTS)
foreach(_manifest ${_SSG_COMPONENT_MANIFESTS})
    include("${_manifest}")
endforeach()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(ssg_tests
        tests/test_ssg.cpp
    )
    target_link_libraries(ssg_tests PRIVATE ssg)
    add_test(NAME ssg_tests COMMAND ssg_tests)
endif()
