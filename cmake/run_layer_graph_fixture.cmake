file(REMOVE_RECURSE "${SSG_LAYER_WORK_DIR}")

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -S "${SSG_LAYER_FIXTURE_DIR}"
        -B "${SSG_LAYER_WORK_DIR}"
        -G "${SSG_LAYER_GENERATOR}"
        -DSSG_SOURCE_DIR=${SSG_SOURCE_DIR}
        -DSSG_LAYER_FIXTURE_MODE=${SSG_LAYER_FIXTURE_MODE}
    RESULT_VARIABLE _configure
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _output)

if(_configure EQUAL 0)
    message(FATAL_ERROR
        "forbidden ${SSG_LAYER_FIXTURE_MODE} configured successfully")
endif()

if(NOT _output MATCHES "SSG layer violation")
    message(FATAL_ERROR
        "fixture failed for the wrong reason:\n${_output}")
endif()

file(REMOVE_RECURSE "${SSG_LAYER_WORK_DIR}")
