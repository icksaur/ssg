target_sources(ssg_core PRIVATE
    ${SSG_SOURCE_DIR}/src/GitDiffSource.cpp
    ${SSG_SOURCE_DIR}/src/platform/git_repository.cpp
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
    add_executable(test_git_diff_source
        ${SSG_SOURCE_DIR}/tests/test_git_diff_source.cpp
    )
    target_include_directories(test_git_diff_source PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_git_diff_source PRIVATE ssg_core)
    add_test(NAME test_git_diff_source COMMAND test_git_diff_source)

    add_executable(test_git_repository
        ${SSG_SOURCE_DIR}/tests/test_git_repository.cpp
    )
    target_include_directories(test_git_repository PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_git_repository PRIVATE ssg_core)
    add_test(NAME test_git_repository COMMAND test_git_repository)

    add_executable(test_git_diff_host
        ${SSG_SOURCE_DIR}/tests/test_git_diff_host.cpp
    )
    target_include_directories(test_git_diff_host PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_link_libraries(test_git_diff_host PRIVATE ssg_core)
    add_test(NAME test_git_diff_host COMMAND test_git_diff_host)
endif()
