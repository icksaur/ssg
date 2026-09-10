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

#include <ssg/Editor.h>
#include <string_view>

#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

namespace ssg::testing {

struct CommandFacts {
    std::string id;
    std::string label;
};

inline std::vector<CommandFacts> const& allCommandFacts() {
    static std::vector<CommandFacts> const facts = [] {
        auto const root = std::filesystem::temp_directory_path() /
                          ("ssg-all-command-ids-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::vector<CommandFacts> collected;
        auto created = ssg::createEditor({root});
        if (created.session) {
            for (auto const& [id, command] :
                 created.session->commandRegistry().all()) {
                collected.push_back({id, command.label});
            }
        }
        created.session.reset();
        std::filesystem::remove_all(root);
        return collected;
    }();
    return facts;
}

}  // namespace ssg::testing
