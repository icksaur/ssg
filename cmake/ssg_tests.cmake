include_guard(GLOBAL)

function(ssg_initialize_tests)
    if(NOT SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
        return()
    endif()

    set(_generatedDir "${CMAKE_CURRENT_BINARY_DIR}/generated/tests")
    file(MAKE_DIRECTORY "${_generatedDir}")
    file(WRITE "${_generatedDir}/suite_declarations.inc" "")
    file(WRITE "${_generatedDir}/suite_cases.inc" "")

    add_executable(ssg_tests "${SSG_SOURCE_DIR}/tests/test_dispatcher.cpp")
    target_include_directories(ssg_tests PRIVATE
        "${SSG_SOURCE_DIR}/tests"
        "${_generatedDir}")
    target_precompile_headers(ssg_tests PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${SSG_SOURCE_DIR}/tests/test_helpers.h>")
    target_link_libraries(ssg_tests PRIVATE ssg_core)

    set_property(GLOBAL PROPERTY SSG_TESTS_INITIALIZED TRUE)
    set_property(GLOBAL PROPERTY SSG_TEST_SUITE_NAMES "")
    set_property(GLOBAL PROPERTY SSG_TEST_SUITE_SYMBOLS "")
    set_property(GLOBAL PROPERTY SSG_TEST_SOURCES "")
    set_property(GLOBAL PROPERTY SSG_TEST_GENERATED_DIR "${_generatedDir}")
endfunction()

function(_ssg_require_test_suite suite output)
    get_property(_source GLOBAL PROPERTY "SSG_TEST_ENTRY_${suite}")
    if(NOT _source)
        message(FATAL_ERROR "SSG test suite is not registered: ${suite}")
    endif()
    set(${output} "${_source}" PARENT_SCOPE)
endfunction()

function(_ssg_add_test_sources)
    get_property(_known GLOBAL PROPERTY SSG_TEST_SOURCES)
    foreach(_source IN LISTS ARGN)
        get_filename_component(_absolute "${_source}" ABSOLUTE
            BASE_DIR "${SSG_SOURCE_DIR}")
        if(NOT EXISTS "${_absolute}")
            message(FATAL_ERROR "SSG test source does not exist: ${_absolute}")
        endif()
        if(NOT _absolute IN_LIST _known)
            target_sources(ssg_tests PRIVATE "${_absolute}")
            list(APPEND _known "${_absolute}")
        endif()
    endforeach()
    set_property(GLOBAL PROPERTY SSG_TEST_SOURCES "${_known}")
endfunction()

function(ssg_add_test_suite)
    # CONTRACT: Ordinary runtime tests register here rather than creating
    # executables; CTest provides process isolation through suite selection.
    set(_options ARGS)
    set(_oneValue NAME ENTRY SYMBOL)
    set(_multiValue SOURCES)
    cmake_parse_arguments(SUITE
        "${_options}" "${_oneValue}" "${_multiValue}" ${ARGN})

    get_property(_initialized GLOBAL PROPERTY SSG_TESTS_INITIALIZED)
    if(NOT _initialized)
        message(FATAL_ERROR "ssg_initialize_tests must run before suite registration")
    endif()
    if(NOT SUITE_NAME OR NOT SUITE_ENTRY OR NOT SUITE_SYMBOL)
        message(FATAL_ERROR "SSG test suite requires NAME, ENTRY, and SYMBOL")
    endif()
    if(NOT SUITE_SYMBOL MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR
            "SSG test suite has invalid symbol: ${SUITE_SYMBOL}")
    endif()

    get_property(_names GLOBAL PROPERTY SSG_TEST_SUITE_NAMES)
    get_property(_symbols GLOBAL PROPERTY SSG_TEST_SUITE_SYMBOLS)
    if(SUITE_NAME IN_LIST _names)
        message(FATAL_ERROR "Duplicate SSG test suite name: ${SUITE_NAME}")
    endif()
    if(SUITE_SYMBOL IN_LIST _symbols)
        message(FATAL_ERROR "Duplicate SSG test suite symbol: ${SUITE_SYMBOL}")
    endif()

    get_filename_component(_entry "${SUITE_ENTRY}" ABSOLUTE
        BASE_DIR "${SSG_SOURCE_DIR}")
    _ssg_add_test_sources("${_entry}" ${SUITE_SOURCES})
    set_property(GLOBAL PROPERTY "SSG_TEST_ENTRY_${SUITE_NAME}" "${_entry}")
    list(APPEND _names "${SUITE_NAME}")
    list(APPEND _symbols "${SUITE_SYMBOL}")
    set_property(GLOBAL PROPERTY SSG_TEST_SUITE_NAMES "${_names}")
    set_property(GLOBAL PROPERTY SSG_TEST_SUITE_SYMBOLS "${_symbols}")

    get_property(_generatedDir GLOBAL PROPERTY SSG_TEST_GENERATED_DIR)
    if(SUITE_ARGS)
        file(APPEND "${_generatedDir}/suite_declarations.inc"
            "int ssg_test_entry_${SUITE_SYMBOL}(int, char**);\n")
        file(APPEND "${_generatedDir}/suite_cases.inc"
            "    if (suite == \"${SUITE_NAME}\") return "
            "ssg_test_entry_${SUITE_SYMBOL}("
            "static_cast<int>(suiteArguments.size()) - 1, "
            "suiteArguments.data());\n")
    else()
        file(APPEND "${_generatedDir}/suite_declarations.inc"
            "int ssg_test_entry_${SUITE_SYMBOL}();\n")
        file(APPEND "${_generatedDir}/suite_cases.inc"
            "    if (suite == \"${SUITE_NAME}\") {\n"
            "        if (argc != 3) return unexpectedSuiteArguments(suite);\n"
            "        return ssg_test_entry_${SUITE_SYMBOL}();\n"
            "    }\n")
    endif()

    add_test(NAME "${SUITE_NAME}"
        COMMAND ssg_tests --suite "${SUITE_NAME}")
endfunction()

function(ssg_test_compile_definitions suite visibility)
    _ssg_require_test_suite("${suite}" _entry)
    set_property(SOURCE "${_entry}" APPEND PROPERTY
        COMPILE_DEFINITIONS ${ARGN})
endfunction()

function(ssg_test_labels suite)
    _ssg_require_test_suite("${suite}" _entry)
    set_tests_properties("${suite}" PROPERTIES LABELS "${ARGN}")
endfunction()

function(ssg_test_include_directories suite visibility)
    _ssg_require_test_suite("${suite}" _entry)
    target_include_directories(ssg_tests PRIVATE ${ARGN})
endfunction()

function(ssg_test_link_libraries suite visibility)
    _ssg_require_test_suite("${suite}" _entry)
    target_link_libraries(ssg_tests PRIVATE ${ARGN})
endfunction()

function(ssg_test_add_dependencies suite)
    _ssg_require_test_suite("${suite}" _entry)
    add_dependencies(ssg_tests ${ARGN})
endfunction()

function(ssg_test_sources suite visibility)
    _ssg_require_test_suite("${suite}" _entry)
    _ssg_add_test_sources(${ARGN})
endfunction()
