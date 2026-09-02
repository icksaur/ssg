target_sources(ssg_core PRIVATE
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
target_include_directories(ssg_core PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
ssg_allow_private_roots(
    ssg_core "${CMAKE_CURRENT_BINARY_DIR}/generated")

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
