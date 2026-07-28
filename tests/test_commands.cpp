// Kind: seam.
//
// C0 of doc/spec-commands.md: proves the new compiled catalog is equal to the
// catalog it replaces, BEFORE anything is reprojected from it.
//
// The comparison covers only the fields C0 does not change -- ids, effect,
// capabilities, and argument shape -- against both the assembled registry and
// the JSON transcription, in both directions.  Surface metadata is deliberately
// excluded: `initScript` is a new axis the JSON never carried, so comparing it
// would assert a fiction.  Surfaces are migrated and checked in C4.

#include "test_helpers.h"

#include <ssg/CommandCatalog.h>
#include <ssg/Commands.h>
#include <ssg/EditorRuntime.h>

#include <ssg/CommandReference.h>

#include "all_command_ids.h"
#include "frozen_catalog.h"

#include <array>
#include <cstdlib>

#include <unistd.h>

#include <filesystem>
#include <string>
#include <ssg/EditorSessionBuilder.h>
#include <ssg/FileCommands.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

std::set<std::string> catalogIds() {
    std::set<std::string> ids;
    for (auto const& command : ssg::commandCatalog()) {
        ids.emplace(command.id);
    }
    return ids;
}

TEST(compiledCatalogHasExactlyTheAssembledRegistrysCommands) {
    auto const compiled = catalogIds();
    ASSERT_FALSE(compiled.empty());

    std::set<std::string> assembled;
    for (auto const& descriptor : ssg::p0CommandDescriptors()) {
        assembled.emplace(descriptor.id);
    }
    ASSERT_EQ(compiled, assembled);
}

TEST(compiledCatalogAgreesOnEffectAndCapabilities) {
    std::map<std::string, ssg::CommandDescriptor> assembled;
    for (auto& descriptor : ssg::p0CommandDescriptors()) {
        assembled.emplace(descriptor.id, std::move(descriptor));
    }

    for (auto const& command : ssg::commandCatalog()) {
        auto const found = assembled.find(std::string{command.id});
        ASSERT_TRUE(found != assembled.end());
        if (found == assembled.end()) continue;

        ASSERT_TRUE(command.effect == found->second.effect);

        std::vector<std::string> expected;
        for (auto const& capability : found->second.requiredCapabilities) {
            expected.emplace_back(capability.value());
        }
        std::vector<std::string> actual;
        for (auto const& capability : command.requiredCapabilities) {
            actual.emplace_back(capability);
        }
        ASSERT_EQ(actual, expected);
    }
}

TEST(everyCommandDeclaresAnArgumentShapeAndIdsAreUnique) {
    std::set<std::string> seen;
    for (auto const& command : ssg::commandCatalog()) {
        // Uniqueness: a duplicated row would otherwise shadow silently.
        ASSERT_TRUE(seen.emplace(command.id).second);
        ASSERT_FALSE(command.id.empty());
        ASSERT_FALSE(command.owner.empty());
        ASSERT_FALSE(command.summary.empty());
        // initScript is a grant on top of luaApi eligibility, never alone.
        if (command.surfaces.initScript) ASSERT_TRUE(command.surfaces.luaApi);
    }
    ASSERT_EQ(seen.size(), ssg::commandCatalog().size());
}

TEST(findCommandLocatesEveryRowAndRejectsUnknownIds) {
    for (auto const& command : ssg::commandCatalog()) {
        auto const* found = ssg::findCommand(command.id);
        ASSERT_TRUE(found != nullptr);
        if (found) ASSERT_EQ(std::string{found->id}, std::string{command.id});
    }
    ASSERT_TRUE(ssg::findCommand("no.such_command") == nullptr);
}

TEST(featureMetadataTablesAnnotateCatalogCommandsAndDeclareNoNewOnes) {
    // Some features keep their own descriptor table because it carries data the
    // generic catalog should not (FileCommands' path-prompt and live-diff
    // rules, the FileCommand enum).  Those tables ANNOTATE commands; they must
    // not declare one the catalog does not have, or introduce a command id
    // nothing else knows about.
    // Against the runtime's catalog, not the static table: file-commands has
    // migrated out of that table, and the rule being checked is that the
    // feature's own table annotates registered commands rather than declaring
    // any the editor does not offer.
    auto const owned = [](std::string_view owner) {
        std::set<std::string> ids;
        for (auto const& facts : ssg::testing::allCommandFacts()) {
            if (facts.owner == owner) ids.emplace(facts.id);
        }
        return ids;
    };

    std::set<std::string> fileIds;
    for (auto const& descriptor : ssg::fileCommandsCommandSet().descriptors()) {
        fileIds.emplace(descriptor.id);
    }
    ASSERT_EQ(fileIds, owned("file-commands"));
}

TEST(everyOwnerInTheCatalogHasAtLeastOneCommand) {
    // commandsOwnedBy drives the handler-binding loops, so an owner string
    // typo there would silently bind nothing.  Every owner named in the table
    // must resolve to a non-empty set.
    std::set<std::string> owners;
    for (auto const& command : ssg::commandCatalog()) {
        owners.emplace(command.owner);
    }
    ASSERT_FALSE(owners.empty());
    for (auto const& owner : owners) {
        ASSERT_FALSE(ssg::commandsOwnedBy(owner).empty());
    }
}

// doc/commands.md is generated from the catalog, so this compares the WHOLE
// committed file against what a core-registered runtime renders -- not merely
// that every id appears somewhere.  A summary edited in the catalog, a command
// whose arguments changed, or an owner regrouped all fail here.
//
// Generation is a test rather than a build step because populating the catalog
// means running registration, which needs a runtime, and constructing one
// creates directories.  A build must not do filesystem work, and a generated
// file must not appear unbidden in a working tree: it is a reviewed diff.
//
// Regenerate with:  SSG_UPDATE_DOCS=1 ./build/test_commands
TEST(theGeneratedCommandReferenceIsCurrent) {
    auto const root = std::filesystem::temp_directory_path() /
                      ("ssg-command-reference-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    auto created = ssg::EditorRuntime::create({root});
    ASSERT_TRUE(created.runtime != nullptr);
    if (!created.runtime) return;
    auto const catalog = created.runtime->commandCatalog();
    auto const rendered = ssg::renderCommandReference(*catalog);
    std::filesystem::remove_all(root);

    if (std::getenv("SSG_UPDATE_DOCS") != nullptr) {
        std::ofstream out{SSG_COMMAND_DOC_PATH, std::ios::binary};
        out << rendered;
        ASSERT_TRUE(out.good());
        return;
    }

    std::ifstream input{SSG_COMMAND_DOC_PATH, std::ios::binary};
    ASSERT_TRUE(input.good());
    if (!input.good()) return;
    std::string const committed{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
    ASSERT_EQ(committed, rendered);
}

}  // namespace

// The migration's transition oracle (doc/spec-command-registry.md).
//
// A command's declaration is moving from the static table into the component
// that implements it, a few components per commit.  "The gate is green" does
// not prove a move was faithful: a commit can drop a command, rename one,
// smuggle a new one in, or -- worst -- keep every id while flipping a fact that
// governs behaviour or authority.  Pinning ids alone would catch none of the
// last kind.
//
// So this compares each command's BEHAVIOURAL tuple against the frozen
// pre-migration snapshot: effect (which decides whether the stale-revision
// check applies), required capabilities, and the two Lua axes.  Label and
// summary are excluded deliberately -- they are cosmetic, a move is a natural
// moment to improve them, and doc/commands.md shows any change in the same
// diff.
//
// Scoped to CORE registration: a runtime built without plugins, before
// init.lua.  Later dynamic registrations are not migration defects.
//
// Deleted with the static table at D5.
// A migration may deliberately correct a fact the static table got wrong.  Each
// such correction is named HERE, one line per command, rather than by editing
// the frozen snapshot -- so a reviewer sees the deviation and its reason in the
// diff instead of a silently rewritten oracle.  An unlisted change still fails.
struct DeliberateCorrection {
    std::string_view id;
    std::string_view wasArgument;
    std::string_view nowArgument;
};

// text.newline and the four delete commands were declared as taking text, but
// their handler default-constructed the payload and ignored it; only insertion
// ever read one.  Deducing the argument type from the handler makes that
// fiction unwritable, so the declaration now matches what the code does.  A
// client that still sends a payload has it ignored, exactly as before.
constexpr std::array<DeliberateCorrection, 5> kDeliberateCorrections{{
    {"text.newline", "text", "none"},
    {"text.delete_backward", "text", "none"},
    {"text.delete_forward", "text", "none"},
    {"text.delete_word_backward", "text", "none"},
    {"text.delete_word_forward", "text", "none"},
}};

TEST(theCoreCatalogStillMatchesTheFrozenPreMigrationSnapshot) {
    auto const root = std::filesystem::temp_directory_path() /
                      ("ssg-frozen-catalog-oracle-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    auto created = ssg::EditorRuntime::create({root});
    ASSERT_TRUE(created.runtime != nullptr);
    if (!created.runtime) return;
    auto const catalog = created.runtime->commandCatalog();
    ASSERT_TRUE(catalog != nullptr);
    if (!catalog) return;

    auto const frozen = ssg::frozen::preMigrationCatalog();
    ASSERT_EQ(catalog->size(), frozen.size());

    for (auto const& expected : frozen) {
        auto const* actual = catalog->find(expected.id);
        ASSERT_TRUE(actual != nullptr);
        if (actual == nullptr) continue;

        bool const mutates = actual->effect == ssg::CommandEffect::Mutation;
        ASSERT_EQ(mutates, expected.mutates);
        std::string_view expectedArgument = expected.argument;
        for (auto const& correction : kDeliberateCorrections) {
            if (correction.id != expected.id) continue;
            ASSERT_EQ(std::string{correction.wasArgument},
                      std::string{expected.argument});
            expectedArgument = correction.nowArgument;
        }
        ASSERT_EQ(std::string{ssg::commandArgumentName(*actual)},
                  std::string{expectedArgument});
        ASSERT_EQ(actual->luaApi, expected.luaApi);
        ASSERT_EQ(actual->initScript, expected.initScript);

        std::vector<std::string> expectedCapabilities;
        for (auto const& capability : expected.requiredCapabilities) {
            expectedCapabilities.emplace_back(capability);
        }
        ASSERT_EQ(actual->requiredCapabilities, expectedCapabilities);
    }
    std::filesystem::remove_all(root);
}

int main() {
    RUN(theCoreCatalogStillMatchesTheFrozenPreMigrationSnapshot);
    RUN(compiledCatalogHasExactlyTheAssembledRegistrysCommands);
    RUN(compiledCatalogAgreesOnEffectAndCapabilities);
    RUN(everyCommandDeclaresAnArgumentShapeAndIdsAreUnique);
    RUN(findCommandLocatesEveryRowAndRejectsUnknownIds);
    RUN(featureMetadataTablesAnnotateCatalogCommandsAndDeclareNoNewOnes);
    RUN(everyOwnerInTheCatalogHasAtLeastOneCommand);
    RUN(theGeneratedCommandReferenceIsCurrent);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
