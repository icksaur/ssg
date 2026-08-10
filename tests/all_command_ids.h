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
#include <ssg/EditorRuntime.h>
#include <ssg/EditorSessionBuilder.h>
#include <optional>
#include <typeindex>

#include <unistd.h>

#include <filesystem>
#include <string>
#include <string>
#include <vector>

namespace ssg::testing {

// A command's declaration, without its implementation: enough for a fixture to
// register a faithful stand-in.
struct CommandFacts {
    std::string id;
    std::string owner;
    bool luaApi = false;
    bool initScript = false;
    bool mutates = true;
    std::vector<ssg::CapabilityId> requiredCapabilities;
    // The real argument type, so a stand-in's wire codec matches the real one.
    std::optional<std::type_index> argument;
    // Whether that argument crosses the wire.  An in-process-only payload has
    // no codec, so a stand-in that claimed one would make the command
    // unencodable.
    bool wire = false;
};

inline std::vector<CommandFacts> const& allCommandFacts() {
    static std::vector<CommandFacts> const facts = [] {
        auto const root = std::filesystem::temp_directory_path() /
                          ("ssg-all-command-ids-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::vector<CommandFacts> collected;
        auto created = ssg::EditorRuntime::create({root});
        if (created.runtime) {
            for (auto const* command :
                 created.runtime->commandCatalog()->commands()) {
                collected.push_back(
                    {command->id, command->owner, command->luaApi,
                     command->initScript,
                     command->effect == ssg::CommandEffect::Mutation,
                     command->requiredCapabilities,
                     command->argument.type, command->argument.wire});
            }
        }
        created.runtime.reset();
        std::filesystem::remove_all(root);
        return collected;
    }();
    return facts;
}

// Registers a stand-in for every command the editor offers, with the caller's
// handler.  Effect and capabilities are copied from the real declaration so a
// fixture's dispatch decisions match the real session's.
template <typename MakeHandler>
void registerStandIns(ssg::EditorSessionBuilder& builder,
                      MakeHandler&& makeHandler) {
    for (auto const& facts : allCommandFacts()) {
        ssg::CommandSpecBuilder spec{facts.id};
        spec.owner("test-stand-in").summary("stand-in");
        if (facts.mutates) {
            spec.mutates();
        } else {
            spec.observes();
        }
        for (auto const& capability : facts.requiredCapabilities) {
            spec.capability(std::string{capability.value()});
        }
        // Untyped by necessity: one stand-in handler serves every command, so
        // it cannot name the argument any single one consumes.  It still
        // declares the real command's WIRE type, so it encodes identically.
        spec.untypedHandler(makeHandler(facts.id),
                            facts.wire ? facts.argument : std::nullopt);
        builder.add(std::move(spec));
    }
}

}  // namespace ssg::testing
