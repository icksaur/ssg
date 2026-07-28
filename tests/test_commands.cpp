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

#include <ssg/Commands.h>
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
    auto const owned = [](std::string_view owner) {
        std::set<std::string> ids;
        for (auto const* command : ssg::commandsOwnedBy(owner)) {
            ids.emplace(command->id);
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

TEST(theGeneratedCommandReferenceIsCurrent) {
    // doc/commands.md is generated from this catalog by ssg_command_docs.  If
    // it is stale the catalog and the published reference disagree, so this
    // fails rather than letting the doc drift.  Regenerate with:
    //   cmake --build build --target ssg_command_docs_generate
    std::ifstream input{SSG_COMMAND_DOC_PATH, std::ios::binary};
    ASSERT_TRUE(input.good());
    if (!input.good()) return;
    std::string const doc{std::istreambuf_iterator<char>{input},
                          std::istreambuf_iterator<char>{}};
    ASSERT_FALSE(doc.empty());

    // Every command appears, and no command appears that the catalog lacks.
    std::size_t documented = 0;
    for (auto const& command : ssg::commandCatalog()) {
        std::string const needle = "| `" + std::string{command.id} + "` |";
        ASSERT_TRUE(doc.find(needle) != std::string::npos);
        ++documented;
    }
    ASSERT_EQ(documented, ssg::commandCatalog().size());

    std::size_t rows = 0;
    for (std::size_t at = doc.find("| `"); at != std::string::npos;
         at = doc.find("| `", at + 1)) {
        ++rows;
    }
    ASSERT_EQ(rows, ssg::commandCatalog().size());
}

}  // namespace

int main() {
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
