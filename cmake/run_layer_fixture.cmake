execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${SSG_LAYER_BINARY_DIR}"
        --target "${SSG_LAYER_TARGET}"
    RESULT_VARIABLE _build
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _output)

if(SSG_LAYER_EXPECT_SUCCESS)
    if(NOT _build EQUAL 0)
        message(FATAL_ERROR
            "${SSG_LAYER_TARGET} should build successfully:\n${_output}")
    endif()
elseif(_build EQUAL 0)
    message(FATAL_ERROR
        "${SSG_LAYER_TARGET} unexpectedly compiled ${SSG_LAYER_HEADER}")
elseif(NOT _output MATCHES "${SSG_LAYER_HEADER}")
    message(FATAL_ERROR
        "${SSG_LAYER_TARGET} failed for the wrong reason:\n${_output}")
endif()
