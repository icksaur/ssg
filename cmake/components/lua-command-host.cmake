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

target_sources(ssg PRIVATE
    ${SSG_SOURCE_DIR}/src/LuaCommandHost.cpp
)
target_include_directories(ssg PRIVATE ${SSG_LUA_INCLUDE_DIR})
target_link_libraries(ssg PRIVATE ${SSG_LUA_LIBRARY})

if(SSG_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_executable(test_lua
        ${SSG_SOURCE_DIR}/tests/test_lua.cpp
    )
    target_include_directories(test_lua PRIVATE
        ${SSG_SOURCE_DIR}/tests
    )
    target_compile_definitions(test_lua PRIVATE
        SSG_TEST_SOURCE_DIR="${SSG_SOURCE_DIR}"
    )
    target_link_libraries(test_lua PRIVATE ssg)
    add_test(NAME test_lua COMMAND test_lua)
endif()
