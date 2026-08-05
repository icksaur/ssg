target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/ChromeComposition.cpp
    ${SSG_SOURCE_DIR}/src/ChromeLowering.cpp
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_chrome_composition
        ${SSG_SOURCE_DIR}/tests/test_chrome_composition.cpp
    )
    target_link_libraries(test_chrome_composition PRIVATE ssg)
    add_test(NAME test_chrome_composition COMMAND test_chrome_composition)

    add_executable(test_chrome_lowering
        ${SSG_SOURCE_DIR}/tests/test_chrome_lowering.cpp
    )
    target_link_libraries(test_chrome_lowering PRIVATE ssg)
    add_test(NAME test_chrome_lowering COMMAND test_chrome_lowering)
endif()
