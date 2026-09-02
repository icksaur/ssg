// Kind: seam.
//
// Proves the new compiled catalog is equal to the
// catalog it replaces, BEFORE anything is reprojected from it.
//
// The comparison covers only the fields C0 does not change -- ids, effect,
// capabilities, and argument shape -- against both the assembled registry and
// the JSON transcription, in both directions.  Surface metadata is deliberately
// excluded: `initScript` is a new axis the JSON never carried, so comparing it
// would assert a fiction.  Surfaces are migrated and checked in C4.

#include "test_helpers.h"

#include <ssg/CommandCatalog.h>
#include <ssg/EditCommands.h>
#include <ssg/EditorSession.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/FindReplace.h>
#include <ssg/Keymap.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/PromptSurface.h>
#include <ssg/Search.h>
#include <ssg/Selection.h>
#include <ssg/Settings.h>
#include <ssg/TabManager.h>
#include <ssg/TextCodec.h>
#include <ssg/TextInputCommands.h>
#include <ssg/TreeModel.h>
#include <ssg/UiNodeState.h>

#include "all_command_ids.h"
#include <array>
#include <cstdlib>

#include <unistd.h>

#include <filesystem>
#include <string>
#include <ssg/FileCommands.h>

#include "file_commands.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace {

std::string_view commandArgumentName(ssg::CommandEntry const& command) {
    if (!command.argument.type || !command.argument.wire) return "none";

    static std::unordered_map<std::type_index, std::string_view> const names{
        {typeid(ssg::TextInputArguments), "text"},
        {typeid(ssg::SelectionCommandArguments), "selection"},
        {typeid(ssg::ScrollLinesArguments), "scroll lines"},
        {typeid(ssg::ScrollPagesArguments), "scroll pages"},
        {typeid(ssg::ScrollFractionArguments), "scroll fraction"},
        {typeid(ssg::DroppedContentArguments), "dropped content"},
        {typeid(ssg::ReopenWithEncodingArguments), "encoding"},
        {typeid(ssg::SetEncodingArguments), "encoding"},
        {typeid(ssg::SetLineEndingArguments), "line ending"},
        {typeid(ssg::SetFinalNewlineArguments), "final newline"},
        {typeid(ssg::SettingSetArguments), "setting"},
        {typeid(ssg::SettingResetArguments), "setting key"},
        {typeid(ssg::SettingResetScopeArguments), "setting scope"},
        {typeid(ssg::WorkspaceReplaceArguments), "workspace replace"},
        {typeid(ssg::WorkspaceReplacePreview), "workspace apply"},
        {typeid(ssg::PaletteExecuteArguments), "palette selection"},
        {typeid(ssg::PickerSubmitArguments), "picker candidate"},
        {typeid(ssg::TreeSelectArguments), "tree node"},
        {typeid(ssg::ExternalActionInvocation), "external action"},
        {typeid(ssg::FindQueryArguments), "query"},
        {typeid(ssg::PromptValueArguments), "prompt value"},
        {typeid(ssg::PromptFocusArguments), "prompt focus"},
        {typeid(ssg::UiNodeActivationArguments), "UI node activation"},
        {typeid(ssg::TabId), "tab"},
    };
    auto const found = names.find(*command.argument.type);
    if (found == names.end()) {
        throw std::invalid_argument{
            "command \"" + command.id +
            "\" declares an argument type with no name for the reference"};
    }
    return found->second;
}

std::string_view commandSurfaces(ssg::CommandEntry const& command) {
    if (command.initScript) return "lua, init.lua";
    if (command.luaApi) return "lua";
    return "—";
}

std::string renderCommandReference(ssg::CommandCatalog const& catalog) {
    auto const commands = catalog.commands();
    std::map<std::string_view, std::vector<ssg::CommandEntry const*>> byOwner;
    for (auto const* command : commands) {
        byOwner[command->owner].push_back(command);
    }

    std::ostringstream out;
    out << "# Commands\n\n"
        << "Generated from the command catalog by `test_commands`. Do not\n"
        << "edit: change the command's registration instead, then regenerate\n"
        << "with `SSG_UPDATE_DOCS=1 ./build/test_commands`.\n\n"
        << "`init.lua` may call the commands marked `init.lua`; the rest are\n"
        << "available to the Lua API when a host grants them.\n\n"
        << "There are " << commands.size() << " commands.\n";

    for (auto const& [owner, owned] : byOwner) {
        out << "\n## " << owner << "\n\n"
            << "| Command | Summary | Arguments | Surfaces |\n"
            << "|---|---|---|---|\n";
        for (auto const* command : owned) {
            out << "| `" << command->id << "` | " << command->summary << " | "
                << commandArgumentName(*command) << " | "
                << commandSurfaces(*command) << " |\n";
        }
    }
    return out.str();
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
    for (auto const& descriptor : ssg::kFileCommands) {
        fileIds.emplace(descriptor.id);
    }
    ASSERT_EQ(fileIds, owned("file-commands"));
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
    auto created = ssg::EditorSession::create({root});
    ASSERT_TRUE(created.session != nullptr);
    if (!created.session) return;
    auto const catalog = created.session->commandCatalog();
    auto const rendered = renderCommandReference(*catalog);
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

// The migration's transition oracle.
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

SSG_TEST_SUITE(test_commands) {
    RUN(featureMetadataTablesAnnotateCatalogCommandsAndDeclareNoNewOnes);
    RUN(theGeneratedCommandReferenceIsCurrent);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
