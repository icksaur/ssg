target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/ChromeDecode.cpp
    ${SSG_SOURCE_DIR}/src/ChromeLowering.cpp
    ${SSG_SOURCE_DIR}/src/ChromeRegionShape.cpp
    ${SSG_SOURCE_DIR}/src/WholeScreenAssembly.cpp
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

    add_executable(test_whole_screen_assembly
        ${SSG_SOURCE_DIR}/tests/test_whole_screen_assembly.cpp
    )
    target_link_libraries(test_whole_screen_assembly PRIVATE ssg)
    add_test(NAME test_whole_screen_assembly COMMAND test_whole_screen_assembly)
endif()
