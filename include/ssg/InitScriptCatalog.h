#pragma once

#include <array>
#include <string_view>

namespace ssg {

// The complete registry of commands callable from init.lua via
// ssg.command(id, args) (see doc/spec-config.md). THIS is the single
// source of truth: apps/ssg_main.cpp's initScriptCommandCatalog() builds
// its LuaCommandHostOptions.commands list from this array, and
// tests/test_config_doc.cpp asserts every entry has a backticked mention
// in doc/config.md -- so a newly Lua-exposed command can't ship without
// updating the user-facing doc, and the doc can't silently drift stale
// relative to the actual Lua surface.
struct InitScriptCommandDescriptor {
    std::string_view id;
};

inline constexpr std::array<InitScriptCommandDescriptor, 4> kInitScriptCommands{{
    {"theme.define"},
    {"theme.background"},
    {"keymap.bind"},
    {"keymap.unbind"},
}};

}  // namespace ssg
