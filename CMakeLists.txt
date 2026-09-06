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

# Keep project paths correct when SSG is loaded through add_subdirectory.
set(SSG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
include("${SSG_SOURCE_DIR}/cmake/ssg_tests.cmake")

# Sanitizer toggle: -DSSG_SANITIZE=ON enables AddressSanitizer + UBSanitizer.
# Requires Clang or GCC.  Applied as PUBLIC so linked test targets inherit
# the flags and link options.
option(SSG_SANITIZE "Build with AddressSanitizer and UBSanitizer" OFF)
# Do not add SSG's internal tests to projects that embed it.
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    enable_testing()
endif()

add_library(ssg_platform STATIC)
add_library(ssg_core STATIC)

target_include_directories(ssg_platform PUBLIC "${SSG_SOURCE_DIR}/include")
target_include_directories(ssg_core PUBLIC "${SSG_SOURCE_DIR}/include")
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
    target_compile_options("${target}" PRIVATE -Wall -Wextra)
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

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    # Compile app-private presentation once for the TUI and its focused tests
    # without creating an embeddable library component.
    add_library(ssg_tui_objects OBJECT
        ${SSG_SOURCE_DIR}/src/GridPresenter.cpp
        ${SSG_SOURCE_DIR}/src/HitTester.cpp
        ${SSG_SOURCE_DIR}/src/Layout.cpp
        ${SSG_SOURCE_DIR}/src/Renderer.cpp
        ${SSG_SOURCE_DIR}/src/StatusFieldGrid.cpp
        ${SSG_SOURCE_DIR}/src/UiRegionProjection.cpp
        ${SSG_SOURCE_DIR}/src/WidgetLayout.cpp
    )
    target_include_directories(ssg_tui_objects PUBLIC
        "${SSG_SOURCE_DIR}/include")
    target_link_libraries(ssg_tui_objects PUBLIC ssg_core)
    ssg_configure_component(ssg_tui_objects)
endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ClipboardRegister.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_clipboard
        ENTRY ${SSG_SOURCE_DIR}/tests/test_clipboard.cpp
        SYMBOL test_clipboard)
    ssg_test_link_libraries(test_clipboard PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/color.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_color
        ENTRY ${SSG_SOURCE_DIR}/tests/test_color.cpp
        SYMBOL test_color)
    ssg_test_include_directories(test_color PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_color PRIVATE ssg_core)

endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_command_metadata
        ENTRY ${SSG_SOURCE_DIR}/tests/test_command_metadata.cpp
        SYMBOL test_command_metadata)
    ssg_test_include_directories(test_command_metadata PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_command_metadata PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/CommandCatalog.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_command_catalog
        ENTRY ${SSG_SOURCE_DIR}/tests/test_command_catalog.cpp
        SYMBOL test_command_catalog)
    ssg_test_include_directories(test_command_catalog PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_command_catalog PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_commands
        ENTRY ${SSG_SOURCE_DIR}/tests/test_commands.cpp
        SYMBOL test_commands)
    ssg_test_include_directories(test_commands PRIVATE
        ${SSG_SOURCE_DIR}/tests
        ${SSG_SOURCE_DIR}/src
    )
    ssg_test_compile_definitions(test_commands PRIVATE
        SSG_COMMAND_DOC_PATH="${SSG_SOURCE_DIR}/doc/commands.md"
    )
    ssg_test_link_libraries(test_commands PRIVATE ssg_core)

endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_config_doc
        ENTRY ${SSG_SOURCE_DIR}/tests/test_config_doc.cpp
        SYMBOL test_config_doc)
    ssg_test_include_directories(test_config_doc PRIVATE
        ${SSG_SOURCE_DIR}/include
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_config_doc PRIVATE
        SSG_CONFIG_DOC_PATH="${SSG_SOURCE_DIR}/doc/config.md"
    )
    ssg_test_link_libraries(test_config_doc PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/DiffModel.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_diff
        ENTRY ${SSG_SOURCE_DIR}/tests/test_diff.cpp
        SYMBOL test_diff)
    ssg_test_include_directories(test_diff PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_diff PRIVATE
        SSG_DIFF_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/diff"
    )
    ssg_test_link_libraries(test_diff PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Document.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_document
        ENTRY ${SSG_SOURCE_DIR}/tests/test_document.cpp
        SYMBOL test_document
        SOURCES
            ${SSG_SOURCE_DIR}/tests/reference_editor.cpp)
    ssg_test_include_directories(test_document PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_document PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/DraftAutosaveScheduler.cpp
    ${SSG_SOURCE_DIR}/src/DraftReopenClassifier.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_draft_autosave
        ENTRY ${SSG_SOURCE_DIR}/tests/test_draft_autosave.cpp
        SYMBOL test_draft_autosave)
    ssg_test_link_libraries(test_draft_autosave PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_draft_reopen
        ENTRY ${SSG_SOURCE_DIR}/tests/test_draft_reopen.cpp
        SYMBOL test_draft_reopen)
    ssg_test_link_libraries(test_draft_reopen PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/EditCommands.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_edit_commands
        ENTRY ${SSG_SOURCE_DIR}/tests/test_edit_commands.cpp
        SYMBOL test_edit_commands)
    ssg_test_link_libraries(test_edit_commands PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/StatusFields.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_status_fields
        ENTRY ${SSG_SOURCE_DIR}/tests/test_status_fields.cpp
        SYMBOL test_status_fields)
    ssg_test_include_directories(test_status_fields PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_status_fields PRIVATE ssg_tui_objects)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Editor.cpp
    ${SSG_SOURCE_DIR}/src/GitDiffIngress.cpp
    ${SSG_SOURCE_DIR}/src/editing.cpp
    ${SSG_SOURCE_DIR}/src/input_routing.cpp
    ${SSG_SOURCE_DIR}/src/files.cpp
    ${SSG_SOURCE_DIR}/src/GitDiffWorker.cpp
    ${SSG_SOURCE_DIR}/src/help.cpp
    ${SSG_SOURCE_DIR}/src/language_services.cpp
    ${SSG_SOURCE_DIR}/src/navigation.cpp
    ${SSG_SOURCE_DIR}/src/ViewCommands.cpp
    ${SSG_SOURCE_DIR}/src/EditorViews.cpp
)
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_session_presentation
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_presentation.cpp
        SYMBOL test_session_presentation)
    ssg_test_compile_definitions(test_session_presentation PRIVATE
        SSG_SOURCE_SCAN_ROOT="${SSG_SOURCE_DIR}"
    )
    ssg_test_include_directories(test_session_presentation PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_session_presentation PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_syntax_injection
        ENTRY ${SSG_SOURCE_DIR}/tests/test_syntax_injection.cpp
        SYMBOL test_syntax_injection)
    ssg_test_include_directories(test_syntax_injection PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_syntax_injection PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_session_files
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_files.cpp
        SYMBOL test_session_files)
    ssg_test_link_libraries(test_session_files PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_reopen
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_files.cpp
        SYMBOL test_session_reopen)
    ssg_test_link_libraries(test_session_reopen PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_drafts
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_files.cpp
        SYMBOL test_session_drafts)
    ssg_test_link_libraries(test_session_drafts PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_notices
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_files.cpp
        SYMBOL test_session_notices)
    ssg_test_link_libraries(test_session_notices PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_conflicts
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_files.cpp
        SYMBOL test_session_conflicts)
    ssg_test_link_libraries(test_session_conflicts PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_session_external_modification
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_external_modification.cpp
        SYMBOL test_session_external_modification)
    ssg_test_include_directories(test_session_external_modification PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_session_external_modification PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_session_editing
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_editing.cpp
        SYMBOL test_session_editing)
    ssg_test_link_libraries(test_session_editing PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_session_grid_parity
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_grid_parity.cpp
        SYMBOL test_session_grid_parity)
    ssg_test_link_libraries(test_session_grid_parity PRIVATE ssg_tui_objects)
    ssg_test_compile_definitions(test_session_grid_parity PRIVATE
        SSG_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )

    ssg_add_test_suite(
        NAME test_session_navigation
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_navigation.cpp
        SYMBOL test_session_navigation)
    ssg_test_include_directories(test_session_navigation PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_session_navigation PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_follow
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_navigation.cpp
        SYMBOL test_session_follow)
    ssg_test_include_directories(test_session_follow PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_session_follow PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_pickers
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_navigation.cpp
        SYMBOL test_session_pickers)
    ssg_test_include_directories(test_session_pickers PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_session_pickers PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_interaction
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_navigation.cpp
        SYMBOL test_session_interaction)
    ssg_test_include_directories(test_session_interaction PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_session_interaction PRIVATE ssg_tui_objects)
    ssg_add_test_suite(
        NAME test_session_layout
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_navigation.cpp
        SYMBOL test_session_layout)
    ssg_test_include_directories(test_session_layout PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_session_layout PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_session_language_services
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_language_services.cpp
        SYMBOL test_session_language_services)
    ssg_test_link_libraries(test_session_language_services PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_session_totality
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_session_totality.cpp
        SYMBOL test_session_totality)
    ssg_test_link_libraries(test_session_totality PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_startup_path
        ENTRY ${SSG_SOURCE_DIR}/tests/test_startup_path.cpp
        SYMBOL test_startup_path)
    ssg_test_include_directories(test_startup_path PRIVATE
        ${SSG_SOURCE_DIR}/src
        ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_startup_path PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_command_dispatch
        ENTRY ${SSG_SOURCE_DIR}/tests/test_command_dispatch.cpp
        SYMBOL test_command_dispatch)
    ssg_test_include_directories(test_command_dispatch PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_command_dispatch PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_help
        ENTRY ${SSG_SOURCE_DIR}/tests/session/test_help.cpp
        SYMBOL test_help)
    ssg_test_link_libraries(test_help PRIVATE ssg_tui_objects)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/TextCodec.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_text_encoding
        ENTRY ${SSG_SOURCE_DIR}/tests/test_text_encoding.cpp
        SYMBOL test_text_encoding)
    ssg_test_include_directories(test_text_encoding PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_text_encoding PRIVATE
        SSG_ENCODING_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/encoding"
    )
    ssg_test_link_libraries(test_text_encoding PRIVATE ssg_core)

endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_end_to_end
        ENTRY ${SSG_SOURCE_DIR}/tests/test_end_to_end_session.cpp
        SYMBOL test_end_to_end
        SOURCES
            ${SSG_SOURCE_DIR}/tests/tui_fixture.cpp)
    ssg_test_include_directories(test_end_to_end PRIVATE
        ${SSG_SOURCE_DIR}/tests
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_end_to_end PRIVATE
        SSG_E2E_WORKFLOW_PATH="${SSG_SOURCE_DIR}/tests/fixtures/end_to_end/mandatory-workflow.tsv"
    )
    ssg_test_link_libraries(test_end_to_end PRIVATE ssg_tui_objects)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ExternalModificationFlow.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_external_modification
        ENTRY ${SSG_SOURCE_DIR}/tests/test_external_modification.cpp
        SYMBOL test_external_modification)
    ssg_test_include_directories(test_external_modification PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_external_modification PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Workspace.cpp
    ${SSG_SOURCE_DIR}/src/FileCommands.cpp
    ${SSG_SOURCE_DIR}/src/FileArchive.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_workspace
        ENTRY ${SSG_SOURCE_DIR}/tests/test_workspace.cpp
        SYMBOL test_workspace)
    ssg_test_link_libraries(test_workspace PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_file_commands
        ENTRY ${SSG_SOURCE_DIR}/tests/test_file_commands.cpp
        SYMBOL test_file_commands)
    ssg_test_include_directories(test_file_commands PRIVATE ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_file_commands PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_path_prompt
        ENTRY ${SSG_SOURCE_DIR}/tests/test_path_prompt.cpp
        SYMBOL test_path_prompt)
    ssg_test_include_directories(test_path_prompt PRIVATE
        ${SSG_SOURCE_DIR}/tests
        ${SSG_SOURCE_DIR}/src
    )
    ssg_test_link_libraries(test_path_prompt PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_name_clash
        ENTRY ${SSG_SOURCE_DIR}/tests/test_name_clash.cpp
        SYMBOL test_name_clash)
    ssg_test_include_directories(test_name_clash PRIVATE
        ${SSG_SOURCE_DIR}/tests
        ${SSG_SOURCE_DIR}/src
    )
    ssg_test_link_libraries(test_name_clash PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_file_archive
        ENTRY ${SSG_SOURCE_DIR}/tests/test_file_archive.cpp
        SYMBOL test_file_archive)
    ssg_test_include_directories(test_file_archive PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_file_archive PRIVATE ssg_core)

endif()

target_sources(ssg_platform PRIVATE
    ${SSG_SOURCE_DIR}/src/FilesystemWatcher.cpp
)

if(WIN32)
    target_sources(ssg_platform PRIVATE
        ${SSG_SOURCE_DIR}/src/WindowsFilesystemWatcher.cpp
        ${SSG_SOURCE_DIR}/src/WindowsGitMetadataWatcher.cpp
    )
else()
    target_sources(ssg_platform PRIVATE
        ${SSG_SOURCE_DIR}/src/LinuxFilesystemWatcher.cpp
        ${SSG_SOURCE_DIR}/src/LinuxGitMetadataWatcher.cpp
    )
endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME ssg_watcher_tests
        ENTRY ${SSG_SOURCE_DIR}/tests/test_watcher.cpp
        SYMBOL ssg_watcher_tests)
    ssg_test_link_libraries(ssg_watcher_tests PRIVATE ssg_platform)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/FindReplace.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_find_replace
        ENTRY ${SSG_SOURCE_DIR}/tests/test_find_replace.cpp
        SYMBOL test_find_replace)
    ssg_test_include_directories(test_find_replace PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_find_replace PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/FollowEditsModel.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_follow_edits
        ENTRY ${SSG_SOURCE_DIR}/tests/test_follow_edits.cpp
        SYMBOL test_follow_edits)
    ssg_test_include_directories(test_follow_edits PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_follow_edits PRIVATE
        SSG_FOLLOW_EDITS_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/follow_edits"
    )
    ssg_test_link_libraries(test_follow_edits PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_types
        ENTRY ${SSG_SOURCE_DIR}/tests/test_types.cpp
        SYMBOL test_types)
    ssg_test_link_libraries(test_types PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/GitDiffSource.cpp
    ${SSG_SOURCE_DIR}/src/GitRepository.cpp
)

if(EXISTS "${SSG_SOURCE_DIR}/vendor/libgit2/CMakeLists.txt")
    set(_ssg_had_c_standard FALSE)
    if(DEFINED CMAKE_C_STANDARD)
        set(_ssg_had_c_standard TRUE)
        set(_ssg_saved_c_standard "${CMAKE_C_STANDARD}")
    endif()
    set(_ssg_had_c_standard_required FALSE)
    if(DEFINED CMAKE_C_STANDARD_REQUIRED)
        set(_ssg_had_c_standard_required TRUE)
        set(_ssg_saved_c_standard_required "${CMAKE_C_STANDARD_REQUIRED}")
    endif()
    set(_ssg_had_c_extensions FALSE)
    if(DEFINED CMAKE_C_EXTENSIONS)
        set(_ssg_had_c_extensions TRUE)
        set(_ssg_saved_c_extensions "${CMAKE_C_EXTENSIONS}")
    endif()

    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(USE_SSH OFF CACHE BOOL "" FORCE)
    set(USE_HTTPS OFF CACHE BOOL "" FORCE)
    set(USE_GSSAPI OFF CACHE BOOL "" FORCE)
    set(USE_NTLMCLIENT OFF CACHE BOOL "" FORCE)
    set(USE_NSEC OFF CACHE BOOL "" FORCE)
    set(USE_BUNDLED_ZLIB ON CACHE BOOL "" FORCE)
    set(REGEX_BACKEND builtin CACHE STRING "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${SSG_SOURCE_DIR}/vendor/libgit2
                     ${CMAKE_BINARY_DIR}/vendor-libgit2
                     EXCLUDE_FROM_ALL)
    if(_ssg_had_c_standard)
        set(CMAKE_C_STANDARD "${_ssg_saved_c_standard}" CACHE STRING "" FORCE)
    else()
        unset(CMAKE_C_STANDARD CACHE)
    endif()
    if(_ssg_had_c_standard_required)
        set(CMAKE_C_STANDARD_REQUIRED "${_ssg_saved_c_standard_required}" CACHE BOOL "" FORCE)
    else()
        unset(CMAKE_C_STANDARD_REQUIRED CACHE)
    endif()
    if(_ssg_had_c_extensions)
        set(CMAKE_C_EXTENSIONS "${_ssg_saved_c_extensions}" CACHE BOOL "" FORCE)
    else()
        unset(CMAKE_C_EXTENSIONS CACHE)
    endif()
    target_link_libraries(ssg_core PUBLIC libgit2package)
    if(DEFINED LIBGIT2_SYSTEM_LIBS)
        target_link_libraries(ssg_core PUBLIC ${LIBGIT2_SYSTEM_LIBS})
    endif()
    target_compile_definitions(ssg_core PRIVATE SSG_LIBGIT2)
endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_git_diff_source
        ENTRY ${SSG_SOURCE_DIR}/tests/test_git_diff_source.cpp
        SYMBOL test_git_diff_source)
    ssg_test_include_directories(test_git_diff_source PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_git_diff_source PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_git_repository
        ENTRY ${SSG_SOURCE_DIR}/tests/test_git_repository.cpp
        SYMBOL test_git_repository)
    ssg_test_include_directories(test_git_repository PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_git_repository PRIVATE ssg_core)

endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_grid_presentation_builder
        ENTRY ${SSG_SOURCE_DIR}/tests/test_grid_presentation_builder.cpp
        SYMBOL test_grid_presentation_builder)
    ssg_test_include_directories(test_grid_presentation_builder PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_grid_presentation_builder PRIVATE ssg_tui_objects)

endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_hit_test
        ENTRY ${SSG_SOURCE_DIR}/tests/test_hit_test.cpp
        SYMBOL test_hit_test)
    ssg_test_include_directories(test_hit_test PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_hit_test PRIVATE ssg_tui_objects)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Keymap.cpp
    ${SSG_SOURCE_DIR}/src/CompiledKeymap.cpp
    ${SSG_SOURCE_DIR}/src/PromptRouting.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_input
        ENTRY ${SSG_SOURCE_DIR}/tests/test_input.cpp
        SYMBOL test_input)
    ssg_test_link_libraries(test_input PRIVATE ssg_core)
    ssg_test_include_directories(test_input PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_input PRIVATE
        SSG_SOURCE_PATH="${SSG_SOURCE_DIR}/src"
    )

    ssg_add_test_suite(
        NAME test_prompt_routing
        ENTRY ${SSG_SOURCE_DIR}/tests/test_prompt_routing.cpp
        SYMBOL test_prompt_routing)
    ssg_test_link_libraries(test_prompt_routing PRIVATE ssg_core)
    ssg_test_include_directories(test_prompt_routing PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )


endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/LspFeatureController.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_lsp_features
        ENTRY ${SSG_SOURCE_DIR}/tests/test_lsp_features.cpp
        SYMBOL test_lsp_features
        SOURCES
            ${SSG_SOURCE_DIR}/tests/fake_lsp_server.cpp)
    ssg_test_include_directories(test_lsp_features PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_lsp_features PRIVATE
        SSG_TEST_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    ssg_test_link_libraries(test_lsp_features PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/LspSyncClient.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_lsp_sync
        ENTRY ${SSG_SOURCE_DIR}/tests/test_lsp_sync.cpp
        SYMBOL test_lsp_sync
        SOURCES
            ${SSG_SOURCE_DIR}/tests/fake_lsp_server.cpp)
    ssg_test_include_directories(test_lsp_sync PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_lsp_sync PRIVATE
        SSG_TEST_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    ssg_test_link_libraries(test_lsp_sync PRIVATE ssg_core)

endif()

find_path(SSG_LUA_INCLUDE_DIR lua.h
    PATH_SUFFIXES lua5.4
)
find_library(SSG_LUA_LIBRARY
    NAMES lua5.4 lua54
)
if(NOT SSG_LUA_INCLUDE_DIR OR NOT SSG_LUA_LIBRARY)
    message(FATAL_ERROR "Lua 5.4 development files are required for the Lua command host")
endif()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES "${SSG_LUA_INCLUDE_DIR}")
check_cxx_source_compiles(
    "#include <lua.h>
     #if LUA_VERSION_NUM != 504
     #error SSG requires Lua 5.4 headers
     #endif
     int main() { return 0; }"
    SSG_LUA_HEADERS_ARE_54
)
unset(CMAKE_REQUIRED_INCLUDES)
if(NOT SSG_LUA_HEADERS_ARE_54)
    message(FATAL_ERROR "SSG Lua command host requires Lua 5.4 headers")
endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/LuaCommandHost.cpp
)
target_include_directories(ssg_core PRIVATE ${SSG_LUA_INCLUDE_DIR})
target_link_libraries(ssg_core PRIVATE ${SSG_LUA_LIBRARY})

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_lua
        ENTRY ${SSG_SOURCE_DIR}/tests/test_lua.cpp
        SYMBOL test_lua)
    ssg_test_include_directories(test_lua PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_lua PRIVATE
        SSG_TEST_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    ssg_test_link_libraries(test_lua PRIVATE ssg_core)

endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_open_equivalence
        ENTRY ${SSG_SOURCE_DIR}/tests/test_open_equivalence.cpp
        SYMBOL test_open_equivalence)
    ssg_test_link_libraries(test_open_equivalence PRIVATE ssg_core)
    ssg_test_compile_definitions(test_open_equivalence PRIVATE
        SSG_OPEN_GOLDEN="${SSG_SOURCE_DIR}/tests/fixtures/open/golden.txt")

    ssg_add_test_suite(
        NAME test_open_metrics
        ENTRY ${SSG_SOURCE_DIR}/tests/test_open_metrics.cpp
        SYMBOL test_open_metrics)
    ssg_test_include_directories(test_open_metrics PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_link_libraries(test_open_metrics PRIVATE ssg_core)
endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/PieceTree.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_piece_tree
        ENTRY ${SSG_SOURCE_DIR}/tests/test_piece_tree.cpp
        SYMBOL test_piece_tree)
    ssg_test_include_directories(test_piece_tree PRIVATE
        ${SSG_SOURCE_DIR}/src
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_piece_tree PRIVATE ssg_core)

endif()

target_sources(ssg_platform PRIVATE
    ${SSG_SOURCE_DIR}/src/platform_files.cpp
)

if(WIN32)
    target_sources(ssg_platform PRIVATE
        ${SSG_SOURCE_DIR}/src/windows_files.cpp
    )
    target_link_libraries(ssg_platform PRIVATE advapi32)
else()
    target_sources(ssg_platform PRIVATE
        ${SSG_SOURCE_DIR}/src/linux_files.cpp
    )
endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_platform_files
        ENTRY ${SSG_SOURCE_DIR}/tests/test_platform_files.cpp
        SYMBOL test_platform_files)
    ssg_test_include_directories(test_platform_files PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_platform_files PRIVATE ssg_platform)

    ssg_add_test_suite(
        NAME test_platform_file_seam
        ENTRY ${SSG_SOURCE_DIR}/tests/test_platform_file_seam.cpp
        SYMBOL test_platform_file_seam)
    ssg_test_include_directories(test_platform_file_seam PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_platform_file_seam PRIVATE ssg_platform)

    ssg_add_test_suite(
        NAME test_platform_interface
        ENTRY ${SSG_SOURCE_DIR}/tests/test_platform_interface.cpp
        SYMBOL test_platform_interface)
    ssg_test_link_libraries(test_platform_interface PRIVATE ssg_platform)

    ssg_add_test_suite(
        NAME test_file_seam_guard
        ENTRY ${SSG_SOURCE_DIR}/tests/test_file_seam_guard.cpp
        SYMBOL test_file_seam_guard)
    ssg_test_link_libraries(test_file_seam_guard PRIVATE ssg_platform)
    set_tests_properties(test_file_seam_guard PROPERTIES
        WORKING_DIRECTORY ${SSG_SOURCE_DIR})

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/PromptSurface.cpp
    ${SSG_SOURCE_DIR}/src/StatusBar.cpp
)
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME ssg_prompt_status_tests
        ENTRY ${SSG_SOURCE_DIR}/tests/test_prompt_status.cpp
        SYMBOL ssg_prompt_status_tests)
    ssg_test_include_directories(ssg_prompt_status_tests PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(ssg_prompt_status_tests PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/RecoveryManager.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_recovery
        ENTRY ${SSG_SOURCE_DIR}/tests/test_recovery.cpp
        SYMBOL test_recovery)
    ssg_add_test_suite(
        NAME test_recovery_tree
        ENTRY ${SSG_SOURCE_DIR}/tests/test_recovery.cpp
        SYMBOL test_recovery_tree)
    ssg_add_test_suite(
        NAME test_recovery_failures
        ENTRY ${SSG_SOURCE_DIR}/tests/test_recovery.cpp
        SYMBOL test_recovery_failures)
    ssg_add_test_suite(
        NAME test_recovery_rollback
        ENTRY ${SSG_SOURCE_DIR}/tests/test_recovery.cpp
        SYMBOL test_recovery_rollback)
    ssg_add_test_suite(
        NAME test_recovery_reconstruction
        ENTRY ${SSG_SOURCE_DIR}/tests/test_recovery.cpp
        SYMBOL test_recovery_reconstruction)
    ssg_add_test_suite(
        NAME test_recovery_retry
        ENTRY ${SSG_SOURCE_DIR}/tests/test_recovery.cpp
        SYMBOL test_recovery_retry)
    ssg_add_test_suite(
        NAME test_recovery_budget
        ENTRY ${SSG_SOURCE_DIR}/tests/test_recovery.cpp
        SYMBOL test_recovery_budget)
    ssg_add_test_suite(
        NAME test_recovery_budget_rejection
        ENTRY ${SSG_SOURCE_DIR}/tests/test_recovery.cpp
        SYMBOL test_recovery_budget_rejection)
    ssg_test_include_directories(test_recovery PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_recovery PRIVATE ssg_core)

endif()

# The reference editor is the independent string-based oracle (spec I13).
# Its sources are NOT added to the ssg library; the oracle shares no editing,
# selection, or history code with SSG.  The aggregate test target compiles
# reference_editor.cpp and test_reference_editor.cpp directly without
# linking to ssg (test_helpers.h is a header-only standalone helper).

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_reference_editor
        ENTRY ${SSG_SOURCE_DIR}/tests/test_reference_editor.cpp
        SYMBOL test_reference_editor
        SOURCES
            ${SSG_SOURCE_DIR}/tests/reference_editor.cpp)
    ssg_test_include_directories(test_reference_editor PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    # No link to ssg: oracle independence is a hard requirement (I13).

endif()

# The authoritative cell renderer, compiled into the ssg library so every client
# consumes the same snapshot -> CellGrid transform.

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_render
        ENTRY ${SSG_SOURCE_DIR}/tests/test_render.cpp
        SYMBOL test_render)
    ssg_test_include_directories(test_render PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_render PRIVATE ssg_tui_objects)

endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_renderer_diff_overlay
        ENTRY ${SSG_SOURCE_DIR}/tests/test_renderer_diff_overlay.cpp
        SYMBOL test_renderer_diff_overlay)
    ssg_test_include_directories(test_renderer_diff_overlay
        PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_renderer_diff_overlay PRIVATE ssg_tui_objects)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ScratchStore.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_scratch
        ENTRY ${SSG_SOURCE_DIR}/tests/test_scratch.cpp
        SYMBOL test_scratch)
    ssg_test_include_directories(test_scratch PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_scratch PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ScratchJournal.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_scratch_journal
        ENTRY ${SSG_SOURCE_DIR}/tests/test_scratch_journal.cpp
        SYMBOL test_scratch_journal)
    ssg_test_include_directories(test_scratch_journal PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_scratch_journal PRIVATE
        SSG_SCRATCH_JOURNAL_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/scratch/journal"
    )
    ssg_test_link_libraries(test_scratch_journal PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ScratchSession.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_scratch_session
        ENTRY ${SSG_SOURCE_DIR}/tests/test_scratch_session.cpp
        SYMBOL test_scratch_session
        ARGS)
    ssg_test_include_directories(test_scratch_session PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_scratch_session PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ScriptHost.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_script_host
        ENTRY ${SSG_SOURCE_DIR}/tests/test_script_host.cpp
        SYMBOL test_script_host)
    ssg_test_include_directories(test_script_host PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_script_host PRIVATE ssg_tui_objects)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Search.cpp
    ${SSG_SOURCE_DIR}/src/PaletteSearcher.cpp
    ${SSG_SOURCE_DIR}/src/Picker.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_search
        ENTRY ${SSG_SOURCE_DIR}/tests/test_search.cpp
        SYMBOL test_search)
    ssg_test_include_directories(test_search PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_search PRIVATE
        SSG_SEARCH_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/search"
    )
    ssg_test_link_libraries(test_search PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_palette
        ENTRY ${SSG_SOURCE_DIR}/tests/test_palette.cpp
        SYMBOL test_palette)
    ssg_test_include_directories(test_palette PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_palette PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_palette_host_ranking
        ENTRY ${SSG_SOURCE_DIR}/tests/test_palette_host_ranking.cpp
        SYMBOL test_palette_host_ranking)
    ssg_test_include_directories(test_palette_host_ranking PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_palette_host_ranking PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_picker
        ENTRY ${SSG_SOURCE_DIR}/tests/test_picker.cpp
        SYMBOL test_picker)
    ssg_test_include_directories(test_picker PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_picker PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_fuzzy_corpus
        ENTRY ${SSG_SOURCE_DIR}/tests/test_fuzzy_corpus.cpp
        SYMBOL test_fuzzy_corpus)
    ssg_test_include_directories(test_fuzzy_corpus PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_fuzzy_corpus PRIVATE
        SSG_FUZZY_CORPUS="${SSG_SOURCE_DIR}/tests/fixtures/fuzzy_corpus.tsv"
    )
    ssg_test_link_libraries(test_fuzzy_corpus PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Selection.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_selection
        ENTRY ${SSG_SOURCE_DIR}/tests/test_selection.cpp
        SYMBOL test_selection
        SOURCES
            ${SSG_SOURCE_DIR}/tests/reference_editor.cpp)
    ssg_test_include_directories(test_selection PRIVATE
        ${SSG_SOURCE_DIR}/tests
        ${SSG_SOURCE_DIR}/src
    )
    ssg_test_link_libraries(test_selection PRIVATE ssg_core)

endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_session
        ENTRY ${SSG_SOURCE_DIR}/tests/test_session.cpp
        SYMBOL test_session)
    ssg_test_include_directories(test_session PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_session PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Settings.cpp
)
target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/linux_settings.cpp
    ${SSG_SOURCE_DIR}/src/windows_settings.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_settings
        ENTRY ${SSG_SOURCE_DIR}/tests/test_settings.cpp
        SYMBOL test_settings)
    ssg_test_include_directories(test_settings PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_settings PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_settings_persistence
        ENTRY ${SSG_SOURCE_DIR}/tests/test_settings_persistence.cpp
        SYMBOL test_settings_persistence)
    ssg_test_include_directories(test_settings_persistence PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_settings_persistence PRIVATE ssg_core)

endif()

# The immutable ref-counted byte-buffer handle (LF-2) shared by the decoded text,
# the piece-tree original, and the initial persisted_text (wired in LF-3b).

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/SharedBytes.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_shared_bytes
        ENTRY ${SSG_SOURCE_DIR}/tests/test_shared_bytes.cpp
        SYMBOL test_shared_bytes)
    ssg_test_link_libraries(test_shared_bytes PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/PaneTopology.cpp
)
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_pane_topology
        ENTRY ${SSG_SOURCE_DIR}/tests/test_pane_topology.cpp
        SYMBOL test_pane_topology)
    ssg_test_include_directories(test_pane_topology PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_pane_topology PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_layout_solver
        ENTRY ${SSG_SOURCE_DIR}/tests/test_layout_solver.cpp
        SYMBOL test_layout_solver)
    ssg_test_link_libraries(test_layout_solver PRIVATE ssg_tui_objects)

endif()

# The \`ssg\` terminal editor application.  Owns terminal I/O only; all editor,
# layout, and rendering behavior is in the ssg library.
#
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(ssg_app
        ${SSG_SOURCE_DIR}/src/main.cpp
        ${SSG_SOURCE_DIR}/src/ssg_terminal.cpp
        ${SSG_SOURCE_DIR}/src/pointer_routing.cpp
        ${SSG_SOURCE_DIR}/src/InitScriptWatcher.cpp
    )
    set_target_properties(ssg_app PROPERTIES OUTPUT_NAME ssg)
    target_link_libraries(ssg_app PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_ssg_app
        ENTRY ${SSG_SOURCE_DIR}/tests/test_ssg_app.cpp
        SYMBOL test_ssg_app
        SOURCES
            ${SSG_SOURCE_DIR}/src/ssg_terminal.cpp
            ${SSG_SOURCE_DIR}/src/pointer_routing.cpp
            ${SSG_SOURCE_DIR}/src/InitScriptWatcher.cpp)
    ssg_test_include_directories(test_ssg_app PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_ssg_app PRIVATE
        SSG_TEST_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    ssg_test_link_libraries(test_ssg_app PRIVATE ssg_tui_objects)

endif()

target_sources(ssg_platform PRIVATE
    ${SSG_SOURCE_DIR}/src/OptionalSubsystemAudit.cpp
)

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Style.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_style
        ENTRY ${SSG_SOURCE_DIR}/tests/test_style.cpp
        SYMBOL test_style)
    ssg_test_include_directories(test_style PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_compile_definitions(test_style PRIVATE
        SSG_TEST_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    ssg_test_link_libraries(test_style PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/TabManager.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_tabs
        ENTRY ${SSG_SOURCE_DIR}/tests/test_tabs.cpp
        SYMBOL test_tabs)
    ssg_test_link_libraries(test_tabs PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/TextInputCommands.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_text_input_commands
        ENTRY ${SSG_SOURCE_DIR}/tests/test_text_input_commands.cpp
        SYMBOL test_text_input_commands)
    ssg_test_link_libraries(test_text_input_commands PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Theme.cpp
    ${SSG_SOURCE_DIR}/src/DefaultTheme.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_theme
        ENTRY ${SSG_SOURCE_DIR}/tests/test_theme.cpp
        SYMBOL test_theme)
    ssg_test_include_directories(test_theme PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_compile_definitions(test_theme PRIVATE
        SSG_SOURCE_ROOT="${SSG_SOURCE_DIR}"
        SSG_THEME_ROLES_PATH="${SSG_SOURCE_DIR}/tests/fixtures/theme_roles.json"
    )
    ssg_test_link_libraries(test_theme PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/TreeModel.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_tree
        ENTRY ${SSG_SOURCE_DIR}/tests/test_tree.cpp
        SYMBOL test_tree)
    ssg_test_include_directories(test_tree PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_tree PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/SyntaxModel.cpp
)

# Tree-sitter is compiled unconditionally.  Highlighting is disabled at RUNTIME
# by constructing the runtime with a null EditorConfig::syntaxParser,
# which yields plain text.
enable_language(C)

set(_SSG_TREESITTER_VENDOR_DIR ${SSG_SOURCE_DIR}/vendor)

# Highlight queries embedded into the binary.  ONE LINE PER QUERY: add a
# "key=path" entry here and it is compiled in; nothing else in the build
# needs to change.  The keys are what src/TreeSitterParser.cpp's kGrammars
# table refers to.
set(_SSG_EMBEDDED_QUERIES
    "c=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c/queries/highlights.scm"
    "cpp=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-cpp/queries/highlights.scm"
    "javascript=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-javascript/queries/highlights.scm"
    "typescript=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-typescript/queries/highlights.scm"
    "csharp=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c-sharp/queries/highlights.scm"
    "lua=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-lua/queries/highlights.scm"
    "markdown=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown/queries/highlights.scm"
    "markdown_inline=${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown-inline/queries/highlights.scm"
)

# Depend on the query files themselves so editing one regenerates the TU.
set(_SSG_EMBEDDED_QUERY_FILES "")
foreach(_entry IN LISTS _SSG_EMBEDDED_QUERIES)
    string(FIND "${_entry}" "=" _split)
    math(EXPR _rest "${_split} + 1")
    string(SUBSTRING "${_entry}" ${_rest} -1 _path)
    list(APPEND _SSG_EMBEDDED_QUERY_FILES "${_path}")
endforeach()

set(_SSG_EMBEDDED_QUERIES_TU
    ${CMAKE_BINARY_DIR}/generated/treesitter_queries.cpp)
# The spec goes through a FILE rather than a -D argument.  Neither a
# ';'-separated list (truncated at the first ';' by the command-line parser)
# nor a newline-joined string (newlines stripped under VERBATIM) survives
# transit intact, and both failure modes are silent -- they yield a partial
# table that still compiles.
set(_SSG_EMBEDDED_QUERIES_SPEC
    ${CMAKE_BINARY_DIR}/generated/treesitter_queries.spec)
string(JOIN "\n" _SSG_EMBEDDED_QUERIES_TEXT ${_SSG_EMBEDDED_QUERIES})
file(GENERATE OUTPUT ${_SSG_EMBEDDED_QUERIES_SPEC}
     CONTENT "${_SSG_EMBEDDED_QUERIES_TEXT}\n")
add_custom_command(
    OUTPUT ${_SSG_EMBEDDED_QUERIES_TU}
    COMMAND ${CMAKE_COMMAND}
        -DEMBED_SPEC_FILE=${_SSG_EMBEDDED_QUERIES_SPEC}
        -DEMBED_OUTPUT=${_SSG_EMBEDDED_QUERIES_TU}
        -DEMBED_NAMESPACE=ssg
        -DEMBED_ACCESSOR=embeddedHighlightQuery
        -P ${SSG_SOURCE_DIR}/cmake/embed_text.cmake
    DEPENDS ${_SSG_EMBEDDED_QUERY_FILES}
            ${_SSG_EMBEDDED_QUERIES_SPEC}
            ${SSG_SOURCE_DIR}/cmake/embed_text.cmake
    COMMENT "Embedding tree-sitter highlight queries"
    VERBATIM)

set(_SSG_TREESITTER_VENDOR_SOURCES
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter/lib/src/lib.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-cpp/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-cpp/src/scanner.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-javascript/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-javascript/src/scanner.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-typescript/typescript/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-typescript/typescript/src/scanner.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c-sharp/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c-sharp/src/scanner.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-lua/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-lua/src/scanner.c
    # Only the BLOCK grammar is vendored.  Upstream splits markdown in two, and
    # the inline grammar (emphasis, links, code spans) can only be applied
    # through a language injection, which this parser does not implement.
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown/src/scanner.c
    # The inline half, run as a language injection inside the block grammar's
    # `inline` nodes (emphasis, code spans, inline links).
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown-inline/src/parser.c
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown-inline/src/scanner.c
)

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/TreeSitterParser.cpp
    ${_SSG_EMBEDDED_QUERIES_TU}
    ${_SSG_TREESITTER_VENDOR_SOURCES}
)

target_include_directories(ssg_core PRIVATE
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter/lib/include
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-cpp/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-javascript/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-typescript/typescript/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-c-sharp/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-lua/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown/src
    ${_SSG_TREESITTER_VENDOR_DIR}/tree-sitter-markdown-inline/src
)

set_source_files_properties(${_SSG_TREESITTER_VENDOR_SOURCES}
    PROPERTIES
        COMPILE_OPTIONS "-w"
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_syntax
        ENTRY ${SSG_SOURCE_DIR}/tests/test_syntax.cpp
        SYMBOL test_syntax)
    ssg_test_include_directories(test_syntax PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_syntax PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_syntax_language_detection
        ENTRY ${SSG_SOURCE_DIR}/tests/test_syntax_language_detection.cpp
        SYMBOL test_syntax_language_detection)
    ssg_test_include_directories(test_syntax_language_detection PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_syntax_language_detection PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_treesitter_syntax
        ENTRY ${SSG_SOURCE_DIR}/tests/test_treesitter_syntax.cpp
        SYMBOL test_treesitter_syntax)
    ssg_test_include_directories(test_treesitter_syntax PRIVATE
        ${SSG_SOURCE_DIR}/tests
        ${SSG_SOURCE_DIR}/src
    )
    ssg_test_compile_definitions(test_treesitter_syntax PRIVATE
        SSG_TREESITTER_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/syntax"
        SSG_TREESITTER_VENDOR_DIR="${_SSG_TREESITTER_VENDOR_DIR}"
        SSG_TREESITTER_SOURCE_DIR="${SSG_SOURCE_DIR}/src"
    )
    ssg_test_link_libraries(test_treesitter_syntax PRIVATE ssg_core)

    # A separate CTest process ensures no earlier suite has compiled a query,
    # which would make these checks vacuous.
    ssg_add_test_suite(
        NAME test_treesitter_embedded_queries
        ENTRY ${SSG_SOURCE_DIR}/tests/test_treesitter_embedded_queries.cpp
        SYMBOL test_treesitter_embedded_queries)
    ssg_test_include_directories(test_treesitter_embedded_queries PRIVATE
        ${SSG_SOURCE_DIR}/tests
        ${SSG_SOURCE_DIR}/src
    )
    ssg_test_compile_definitions(test_treesitter_embedded_queries PRIVATE
        SSG_TREESITTER_VENDOR_DIR="${_SSG_TREESITTER_VENDOR_DIR}"
    )
    ssg_test_link_libraries(test_treesitter_embedded_queries PRIVATE ssg_core)

    set_tests_properties(test_treesitter_embedded_queries PROPERTIES
        RUN_SERIAL TRUE)

    # The public registration seam.  Deliberately does NOT get the vendored
    # include directories: it must compile against <ssg/TreeSitterGrammars.h>
    # alone, which is what proves a host is not forced to have tree-sitter's
    # headers.
    ssg_add_test_suite(
        NAME test_treesitter_grammar_registration
        ENTRY ${SSG_SOURCE_DIR}/tests/test_treesitter_grammar_registration.cpp
        SYMBOL test_treesitter_grammar_registration)
    ssg_test_include_directories(test_treesitter_grammar_registration PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_treesitter_grammar_registration PRIVATE ssg_core)

endif()

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_tui_fixture
        ENTRY ${SSG_SOURCE_DIR}/tests/test_tui_session.cpp
        SYMBOL test_tui_fixture
        SOURCES
            ${SSG_SOURCE_DIR}/tests/tui_fixture.cpp)
    ssg_test_include_directories(test_tui_fixture PRIVATE
        ${SSG_SOURCE_DIR}/tests
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_tui_fixture PRIVATE ssg_tui_objects)

endif()

# Milestone 11 — The semantic library API drives the TUI contract.
# Proves the TUI screen is a pure function of the production Editor's
# GridPresentation: render(snapshot).canonical() == checked-in goldens for the
# normal, prompt, and too-small screens.

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_tui_contract
        ENTRY ${SSG_SOURCE_DIR}/tests/test_tui_contract.cpp
        SYMBOL test_tui_contract)
    ssg_test_include_directories(test_tui_contract PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_tui_contract PRIVATE ssg_tui_objects)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Widget.cpp
    ${SSG_SOURCE_DIR}/src/UiTree.cpp
    ${SSG_SOURCE_DIR}/src/ScreenState.cpp
)
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_widget
        ENTRY ${SSG_SOURCE_DIR}/tests/test_widget.cpp
        SYMBOL test_widget)
    ssg_test_link_libraries(test_widget PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_widget_layout
        ENTRY ${SSG_SOURCE_DIR}/tests/test_widget_layout.cpp
        SYMBOL test_widget_layout)
    ssg_test_link_libraries(test_widget_layout PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_ui_tree
        ENTRY ${SSG_SOURCE_DIR}/tests/test_ui_tree.cpp
        SYMBOL test_ui_tree)
    ssg_test_link_libraries(test_ui_tree PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_ui_node_state
        ENTRY ${SSG_SOURCE_DIR}/tests/test_ui_node_state.cpp
        SYMBOL test_ui_node_state)
    ssg_test_link_libraries(test_ui_node_state PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_keyboard_focus
        ENTRY ${SSG_SOURCE_DIR}/tests/test_keyboard_focus.cpp
        SYMBOL test_keyboard_focus)
    ssg_test_link_libraries(test_keyboard_focus PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_ui_view_surface
        ENTRY ${SSG_SOURCE_DIR}/tests/test_ui_view_surface.cpp
        SYMBOL test_ui_view_surface)
    ssg_test_link_libraries(test_ui_view_surface PRIVATE ssg_tui_objects)

    ssg_add_test_suite(
        NAME test_screen_state
        ENTRY ${SSG_SOURCE_DIR}/tests/test_screen_state.cpp
        SYMBOL test_screen_state)
    ssg_test_link_libraries(test_screen_state PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/ScreenLayout.cpp
)
if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_screen_layout
        ENTRY ${SSG_SOURCE_DIR}/tests/test_screen_layout.cpp
        SYMBOL test_screen_layout)
    ssg_test_link_libraries(test_screen_layout PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/DocumentHistory.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_history
        ENTRY ${SSG_SOURCE_DIR}/tests/test_history.cpp
        SYMBOL test_history
        SOURCES
            ${SSG_SOURCE_DIR}/tests/reference_editor.cpp)
    ssg_test_include_directories(test_history PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_history PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/GraphemeLayout.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_cell_layout
        ENTRY ${SSG_SOURCE_DIR}/tests/test_cell_layout.cpp
        SYMBOL test_cell_layout)
    ssg_test_link_libraries(test_cell_layout PRIVATE ssg_core)

    ssg_add_test_suite(
        NAME test_gcb_oracle
        ENTRY ${SSG_SOURCE_DIR}/tests/test_gcb_oracle.cpp
        SYMBOL test_gcb_oracle)
    ssg_test_link_libraries(test_gcb_oracle PRIVATE ssg_core)
    ssg_test_compile_definitions(test_gcb_oracle PRIVATE
        UNICODE_DATA_DIR="${SSG_SOURCE_DIR}/data/unicode"
    )

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/Viewport.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_viewport
        ENTRY ${SSG_SOURCE_DIR}/tests/test_viewport.cpp
        SYMBOL test_viewport)
    ssg_test_link_libraries(test_viewport PRIVATE ssg_core)
    ssg_test_compile_definitions(test_viewport PRIVATE
        VIEWPORT_FIXTURE_DIR="${SSG_SOURCE_DIR}/tests/fixtures/layout/viewports"
    )

    ssg_add_test_suite(
        NAME test_scroll_offset
        ENTRY ${SSG_SOURCE_DIR}/tests/test_scroll_offset.cpp
        SYMBOL test_scroll_offset)
    ssg_test_include_directories(test_scroll_offset PRIVATE ${SSG_SOURCE_DIR}/tests)
    ssg_test_link_libraries(test_scroll_offset PRIVATE ssg_core)

endif()

target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/WorkspaceFileIndex.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    ssg_add_test_suite(
        NAME test_workspace_file_index
        ENTRY ${SSG_SOURCE_DIR}/tests/test_workspace_file_index.cpp
        SYMBOL test_workspace_file_index)
    ssg_test_include_directories(test_workspace_file_index PRIVATE
        ${SSG_SOURCE_DIR}/src)
    ssg_test_include_directories(test_workspace_file_index PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    ssg_test_link_libraries(test_workspace_file_index PRIVATE ssg_core)

endif()
