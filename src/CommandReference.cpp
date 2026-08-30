#include <ssg/CommandReference.h>

#include <ssg/CommandCatalog.h>
#include <ssg/EditCommands.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/FileCommands.h>
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

#include <map>
#include <sstream>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ssg {

namespace {

// What a reader should call each argument type.  Keyed by the type itself, so
// this cannot disagree with what a handler consumes -- and a type with no name
// here is refused rather than rendered as an empty column.
std::unordered_map<std::type_index, std::string_view> const& argumentNames() {
    static std::unordered_map<std::type_index, std::string_view> const names{
        {typeid(TextInputArguments), "text"},
        {typeid(SelectionCommandArguments), "selection"},
        {typeid(ScrollLinesArguments), "scroll lines"},
        {typeid(ScrollPagesArguments), "scroll pages"},
        {typeid(ScrollFractionArguments), "scroll fraction"},
        {typeid(DroppedContentArguments), "dropped content"},
        {typeid(ReopenWithEncodingArguments), "encoding"},
        {typeid(SetEncodingArguments), "encoding"},
        {typeid(SetLineEndingArguments), "line ending"},
        {typeid(SetFinalNewlineArguments), "final newline"},
        {typeid(SettingSetArguments), "setting"},
        {typeid(SettingResetArguments), "setting key"},
        {typeid(SettingResetScopeArguments), "setting scope"},
        {typeid(WorkspaceReplaceArguments), "workspace replace"},
        {typeid(WorkspaceReplacePreview), "workspace apply"},
        {typeid(PaletteExecuteArguments), "palette selection"},
        {typeid(PickerSubmitArguments), "picker candidate"},
        {typeid(TreeSelectArguments), "tree node"},
        {typeid(ExternalActionInvocation), "external action"},
        {typeid(FindQueryArguments), "query"},
        {typeid(PromptValueArguments), "prompt value"},
        {typeid(PromptFocusArguments), "prompt focus"},
        {typeid(TabId), "tab"},
    };
    return names;
}

}  // namespace

namespace {

std::string_view commandArgumentName(CommandEntry const& command) {
    // The reference documents the WIRE surface, so an in-process-only payload
    // reads as no argument: a remote client cannot send one.
    if (!command.argument.type.has_value() || !command.argument.wire) {
        return "none";
    }
    auto const& names = argumentNames();
    auto const found = names.find(*command.argument.type);
    if (found == names.end()) {
        throw std::invalid_argument{
            "command \"" + command.id +
            "\" declares an argument type with no name for the reference"};
    }
    return found->second;
}

std::string_view surfaces(CommandEntry const& command) {
    // init.lua implies the Lua API, so naming both would be noise.
    if (command.initScript) return "lua, init.lua";
    if (command.luaApi) return "lua";
    return "—";
}

}  // namespace

std::string CommandReferenceRenderer::render(CommandCatalog const& catalog) const {
    auto const commands = catalog.commands();
    std::map<std::string_view, std::vector<CommandEntry const*>> byOwner;
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
                << commandArgumentName(*command) << " | " << surfaces(*command)
                << " |\n";
        }
    }
    return out.str();
}

}  // namespace ssg
