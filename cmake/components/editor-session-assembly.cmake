target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/EditorSessionBuilder.cpp
    ${SSG_SOURCE_DIR}/src/StatusFields.cpp
    ${SSG_SOURCE_DIR}/src/session_snapshot.cpp
)

file(READ "${SSG_SOURCE_DIR}/data/ui/status_fields.json" SSG_STATUS_FIELDS_JSON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SSG_SOURCE_DIR}/data/ui/status_fields.json"
)
configure_file(
    "${SSG_SOURCE_DIR}/src/status_fields_catalog_json.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/status_fields_catalog_json.h"
    @ONLY
)
target_include_directories(ssg PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_editor_session_assembly
        ${SSG_SOURCE_DIR}/tests/test_editor_session_assembly.cpp
    )
    target_include_directories(test_editor_session_assembly PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
        target_link_libraries(test_editor_session_assembly PRIVATE ssg)
    add_test(NAME test_editor_session_assembly
             COMMAND test_editor_session_assembly)
endif()
