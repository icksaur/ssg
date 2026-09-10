#pragma once

// Every command the editor offers, for fixtures that stand in for the real
// runtime.
//
// A fake session needs the same command SET as the real one so a parity test
// can dispatch anything the real editor would accept.  That set used to be
// readable from the static table, but commands are migrating into the
// components that implement them, so the table
// is no longer the whole story and no single file is.
//
// Asking a real runtime is therefore the only truthful answer -- and it stays
// truthful as the migration proceeds, which a maintained list would not.  The
// runtime is built once, in a temporary workspace, and only its command
// declarations are used; fixtures supply their own stub handlers.

#include <ssg/CommandCatalog.h>
#include <ssg/Editor.h>
#include <optional>
#include <string_view>
#include <typeindex>
#include <unordered_map>

#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

namespace ssg::testing {

// A command's declaration, without its implementation: enough for a fixture to
// register a faithful stand-in.
struct CommandFacts {
    std::string id;
    std::string label;
    std::string owner;
    bool luaApi = false;
    bool initScript = false;
    bool mutates = true;
    // The real argument type, so a stand-in's wire codec matches the real one.
    std::optional<std::type_index> argument;
    // Whether that argument crosses the wire.  An in-process-only payload has
    // no codec, so a stand-in that claimed one would make the command
    // unencodable.
    bool wire = false;
    bool required = false;
};

// Reviewed migration classification for user-surface commands.
// Mechanical default: initScript→config, required→remove, else→keep.
// This table records deliberate overrides per spec-command-system.md §Migration.
inline std::string_view classifyCommand(CommandFacts const& f) {
    static std::unordered_map<std::string, std::string_view> const kOverrides = {
        // remove: explicit file operations (path/id always required for meaning)
        {"file.open_recent",      "remove"},
        // remove: tab activation by id
        {"tab.activate",          "remove"},
        {"goto.file",               "remove"},
        {"goto.symbol",             "remove"},
        {"tree.invoke_node_command", "remove"},
        // split: optional target with useful no-arg behavior retained as command
        {"workspace.open_directory",     "split"},
        {"find.update_query",            "split"},
        {"replace.update_replacement",   "split"},
        {"replace.workspace_preview",    "split"},
        {"replace.workspace_apply",      "split"},
        {"file.new",                     "split"},
        {"file.open",                    "split"},
        {"file.save_as",                 "split"},
        {"file.rename",                  "split"},
        {"file.new_directory",           "split"},
        {"tab.close",                    "split"},
        {"tab.close_others",             "split"},
        {"tab.move_left",                "split"},
        {"tab.move_right",               "split"},
        {"palette.close",                "split"},
        {"search.workspace",             "split"},
        {"goto.line",                    "split"},
    };
    if (f.initScript) return "config";
    auto it = kOverrides.find(f.id);
    if (it != kOverrides.end()) return it->second;
    if (f.required) return "remove";
    return "keep";
}

inline std::vector<CommandFacts> const& allCommandFacts() {
    static std::vector<CommandFacts> const facts = [] {
        auto const root = std::filesystem::temp_directory_path() /
                          ("ssg-all-command-ids-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::vector<CommandFacts> collected;
        auto created = ssg::createEditor({root});
        if (created.session) {
            for (auto const* command :
                 created.session->commandCatalog().commands()) {
                collected.push_back(
                    {command->id, command->displayLabel(), command->owner,
                     command->luaApi,
                     command->initScript,
                     command->effect == ssg::CommandEffect::Mutation,
                     command->argument.type, command->argument.wire,
                     command->argument.required});
            }
        }
        created.session.reset();
        std::filesystem::remove_all(root);
        return collected;
    }();
    return facts;
}

}  // namespace ssg::testing
