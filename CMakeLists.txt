cmake_minimum_required(VERSION 3.14)
project(ssg LANGUAGES CXX)

set_property(GLOBAL PROPERTY GLOBAL_DEPENDS_NO_CYCLES ON)

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

option(SSG_ASSERTIONS "Keep C and C++ assertions enabled" OFF)
if(SSG_ASSERTIONS)
    if(MSVC)
        add_compile_options(/UNDEBUG)
    else()
        add_compile_options(-UNDEBUG)
    endif()
endif()

# Capture SSG's own source root immediately after project().  Manifests are
# included() from this file and share its variable scope, so they see
# SSG_SOURCE_DIR directly.  Manifests MUST use ${SSG_SOURCE_DIR} for all
# project-root-relative paths; using CMAKE_SOURCE_DIR would resolve to the
# host project root when SSG is loaded via add_subdirectory.
set(SSG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
include("${SSG_SOURCE_DIR}/cmake/ssg_layers.cmake")
include("${SSG_SOURCE_DIR}/cmake/ssg_tests.cmake")

# Sanitizer toggle: -DSSG_SANITIZE=ON enables AddressSanitizer + UBSanitizer.
# Requires Clang or GCC.  Applied as PUBLIC so linked test targets inherit
# the flags and link options.
option(SSG_SANITIZE "Build with AddressSanitizer and UBSanitizer" OFF)
# enable_testing() must precede the component-manifest loop so manifests can
# register test suites.  Skipped when SSG is a subdirectory so host
# projects are not contaminated with SSG's CTest entries.
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    enable_testing()
endif()

add_library(ssg_platform STATIC)
add_library(ssg_core STATIC
    src/ssg.cpp
)

ssg_declare_layer(
    TARGET ssg_platform
    ROOT "${SSG_SOURCE_DIR}/include/platform")
ssg_declare_layer(
    TARGET ssg_core
    ROOT "${SSG_SOURCE_DIR}/include/core"
    ALLOWS ssg_platform)
target_link_libraries(ssg_core PUBLIC ssg_platform)

add_library(ssg INTERFACE)
target_link_libraries(ssg INTERFACE ssg_core ssg_platform)
add_library(ssg::ssg ALIAS ssg)

# Warnings are errors for THIS project's own code, so the fourteen that had
# accumulated cannot come back.  Deliberately PRIVATE and deliberately scoped to
# a top-level build: a consumer embedding ssg via add_subdirectory compiles with
# its own toolchain and its own idea of which warnings matter, and inheriting
# -Werror from a dependency is how a library breaks someone else's build on a
# compiler upgrade.  Vendored third-party sources already build with -w.
option(SSG_WERROR "Treat compiler warnings as errors in ssg's own sources"
       ${PROJECT_IS_TOP_LEVEL})
function(ssg_configure_component target)
    if(SSG_SANITIZE)
        target_compile_options("${target}" PUBLIC
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -g)
        target_link_options("${target}" PUBLIC -fsanitize=address,undefined)
    endif()
    if(SSG_WERROR)
        target_compile_options("${target}" PRIVATE -Werror)
    endif()
endfunction()

ssg_configure_component(ssg_platform)
ssg_configure_component(ssg_core)
ssg_initialize_tests()

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
#   1. Add library sources only to their owning ssg_core or ssg_platform target.
#      Use ${SSG_SOURCE_DIR}/<rel-path> for project-root-relative paths.
#      Never use CMAKE_SOURCE_DIR (breaks add_subdirectory consumers).
#      The early app-tui-presentation manifest defines the top-level-only
#      ssg_tui_objects target; other manifests may link but not extend it.
#   2. Inside "if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)", register ordinary
#      runtime suites with ssg_add_test_suite and its ssg_test_* helpers.
#      Standalone executables are reserved for products, benchmarks, probes, and
#      build oracles whose executable identity is part of the contract.
#      This guard is false when SSG is a subdirectory, so consumer projects
#      are not contaminated with SSG's internal test targets.
#   3. Add no component link edge absent from cmake/ssg_layers.cmake, use no
#      directory-wide include_directories(), and reach no sibling private root.
# Manifests are included in sorted order for a deterministic build.
file(GLOB _SSG_COMPONENT_MANIFESTS
    CONFIGURE_DEPENDS
    "${SSG_SOURCE_DIR}/cmake/components/*.cmake"
)
list(SORT _SSG_COMPONENT_MANIFESTS)
foreach(_manifest ${_SSG_COMPONENT_MANIFESTS})
    include("${_manifest}")
endforeach()

ssg_assert_layer_graph()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_subdirectory(tests/fixtures/layers)
    ssg_add_test_suite(
        NAME ssg_tests
        ENTRY tests/test_ssg.cpp
        SYMBOL test_ssg)
endif()
