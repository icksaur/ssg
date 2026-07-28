// Generates doc/commands.md from the command catalog.
//
// A build step, not a runtime one: CMake cannot read a C++ table at configure
// time, so this executable links the catalog, walks it, and writes the
// reference.  It holds no command data of its own -- only formatting -- so the
// catalog stays the single source (doc/spec-commands.md).
//
// Usage: ssg_command_docs <output-path>

#include <ssg/Commands.h>

#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string_view argumentName(ssg::ArgumentKind kind) {
    switch (kind) {
        case ssg::ArgumentKind::None: return "none";
        case ssg::ArgumentKind::TextInput: return "text";
        case ssg::ArgumentKind::SelectionCommand: return "selection";
        case ssg::ArgumentKind::ScrollLines: return "scroll lines";
        case ssg::ArgumentKind::ScrollPages: return "scroll pages";
        case ssg::ArgumentKind::ScrollFraction: return "scroll fraction";
        case ssg::ArgumentKind::DroppedContent: return "dropped content";
        case ssg::ArgumentKind::ReopenWithEncoding: return "encoding";
        case ssg::ArgumentKind::SetEncoding: return "encoding";
        case ssg::ArgumentKind::SetLineEnding: return "line ending";
        case ssg::ArgumentKind::SetFinalNewline: return "final newline";
        case ssg::ArgumentKind::SettingSet: return "setting";
        case ssg::ArgumentKind::SettingReset: return "setting key";
        case ssg::ArgumentKind::SettingResetScope: return "setting scope";
        case ssg::ArgumentKind::WorkspaceReplace: return "workspace replace";
        case ssg::ArgumentKind::WorkspaceApply: return "workspace apply";
        case ssg::ArgumentKind::PaletteExecute: return "palette selection";
        case ssg::ArgumentKind::TreeSelect: return "tree node";
        case ssg::ArgumentKind::FindQuery: return "query";
        case ssg::ArgumentKind::PromptValue: return "prompt value";
    }
    return "none";
}

std::string surfaces(ssg::CommandSurfaces const& value) {
    // init.lua implies the Lua API, so naming both would be noise.
    if (value.initScript) return "lua, init.lua";
    if (value.luaApi) return "lua";
    return "—";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: ssg_command_docs <output-path>\n";
        return 2;
    }

    std::map<std::string_view, std::vector<ssg::CommandSpec const*>> byOwner;
    for (auto const& command : ssg::commandCatalog()) {
        byOwner[command.owner].push_back(&command);
    }

    std::ofstream out{argv[1], std::ios::binary};
    if (!out) {
        std::cerr << "cannot write " << argv[1] << "\n";
        return 1;
    }

    out << "# Commands\n\n"
        << "Generated from the command catalog in `src/Commands.cpp` by\n"
        << "`ssg_command_docs`. Do not edit: change the catalog instead.\n\n"
        << "`init.lua` may call the commands marked `init.lua`; the rest are\n"
        << "available to the Lua API when a host grants them\n"
        << "(see `doc/spec-commands.md`).\n\n"
        << "There are " << ssg::commandCatalog().size() << " commands.\n";

    for (auto const& [owner, commands] : byOwner) {
        out << "\n## " << owner << "\n\n"
            << "| Command | Summary | Arguments | Surfaces |\n"
            << "|---|---|---|---|\n";
        for (auto const* command : commands) {
            out << "| `" << command->id << "` | " << command->summary << " | "
                << argumentName(command->argument) << " | "
                << surfaces(command->surfaces) << " |\n";
        }
    }
    return out ? 0 : 1;
}
