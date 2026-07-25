# Drives the add_subdirectory consumer build for test_embed_consumer.
# Fails loudly on either configure or build failure, printing the output that
# explains why -- a silent success here would defeat the point of the oracle.

file(REMOVE_RECURSE "${SSG_EMBED_WORK_DIR}")
file(MAKE_DIRECTORY "${SSG_EMBED_WORK_DIR}")

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -S "${SSG_EMBED_FIXTURE_DIR}"
        -B "${SSG_EMBED_WORK_DIR}"
        -G "${SSG_EMBED_GENERATOR}"
        -DSSG_EMBED_SOURCE_DIR=${SSG_EMBED_SOURCE_DIR}
    RESULT_VARIABLE _configure
    OUTPUT_VARIABLE _configureOut
    ERROR_VARIABLE _configureOut)
if(NOT _configure EQUAL 0)
    message(FATAL_ERROR
        "embed consumer failed to configure:\n${_configureOut}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${SSG_EMBED_WORK_DIR}" --target embed_consumer
    RESULT_VARIABLE _build
    OUTPUT_VARIABLE _buildOut
    ERROR_VARIABLE _buildOut)
if(NOT _build EQUAL 0)
    message(FATAL_ERROR "embed consumer failed to build:\n${_buildOut}")
endif()

# Linking is the point, but running it proves the library is actually usable
# from an embedder rather than merely linkable.
find_program(_consumer embed_consumer PATHS "${SSG_EMBED_WORK_DIR}" NO_DEFAULT_PATH)
if(NOT _consumer)
    message(FATAL_ERROR "embed consumer binary not found in ${SSG_EMBED_WORK_DIR}")
endif()
execute_process(COMMAND "${_consumer}"
    RESULT_VARIABLE _run OUTPUT_VARIABLE _runOut ERROR_VARIABLE _runOut)
if(NOT _run EQUAL 0)
    message(FATAL_ERROR "embed consumer failed to run:\n${_runOut}")
endif()

file(REMOVE_RECURSE "${SSG_EMBED_WORK_DIR}")
message(STATUS "embed consumer ok")
