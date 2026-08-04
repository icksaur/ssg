#include "editor_runtime_internal.h"

#include <ssg/CommandCatalog.h>
#include <ssg/CommandSpecBuilder.h>
#include <ssg/Keymap.h>
#include <ssg/Style.h>
#include <ssg/SyntaxModel.h>
#include <ssg/command_metadata.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace ssg {
namespace {

// The compiled-in prose. Kept as plain UTF-8 Markdown; the help tab is opened
// with the Markdown language so tree-sitter highlights the headings, emphasis,
// and links. No Markdown TABLES are used -- SSG renders text, not rendered
// tables, so a pipe table would show as raw pipes. URLs still linkify through
// the document's URL-run detection.
constexpr std::string_view kHelpPreamble =
    "# SSG Help\n"
    "\n"
    "Close this tab like any other with Alt+w.\n"
    "\n"
    "## Moving around\n"
    "\n"
    "- Arrow keys, PageUp/PageDown, Home/End move the cursor.\n"
    "- Type to filter in a picker; Escape cancels a prompt.\n"
    "\n"
    "## Mouse\n"
    "\n"
    "- Click to place the cursor; drag to select text.\n"
    "- Double-click a word to select it.\n"
    "- Click a tab to switch to it; middle-click a tab to close it.\n"
    "- Click a URL to open it.\n"
    "- Click the working-directory path in the header to show the file tree.\n"
    "- Click the scrollbar to jump; drag its thumb to scroll.\n"
    "\n"
    "## Multiple cursors\n"
    "\n"
    "Edit in several places at once; every cursor types, deletes, and moves\n"
    "together.\n"
    "\n"
    "- Alt+d adds a cursor at the next occurrence of the current selection, so you\n"
    "  can select a word and press it repeatedly to edit each match.\n"
    "- Alt+j and Alt+k add a cursor on the line below or above.\n"
    "- Alt+i splits a multi-line selection into one cursor per line.\n"
    "- Alt+a selects everything.\n"
    "- Click anywhere to collapse back to a single cursor.\n"
    "\n"
    "## Line numbers\n"
    "\n"
    "The command \"view.toggle_line_numbers\" shows or hides a left gutter with\n"
    "1-indexed line numbers; the current line's number is highlighted. It is off\n"
    "by default. Run it from the command palette.\n"
    "\n"
    "## Keybindings\n"
    "\n";

constexpr std::string_view kHelpConfigSection =
    "\n"
    "## Configuring SSG\n"
    "\n"
    "SSG reads an optional init.lua at startup (see doc/config.md). From it you\n"
    "can recolor every UI role and syntax scope with theme.set, rebind keys with\n"
    "keymap.bind / keymap.unbind, and change the glyphs SSG draws its chrome with\n"
    "using style.define. A missing init.lua is not an error.\n"
    "\n"
    "Example:\n"
    "\n"
    "    ssg.command(\"keymap.bind\", { sequence = \"Alt+KeyH\", command = \"help.open\" })\n"
    "\n"
    "### Chrome glyphs\n"
    "\n"
    "style.define replaces any of these glyphs; the value shown is what is drawn\n"
    "now. Most must keep their current width, but the tab edge and separator\n"
    "glyphs may be any width. Example:\n"
    "\n"
    "    ssg.command(\"style.define\", { tab_separator = \" | \" })\n"
    "\n";

// The chrome-glyph listing, one Markdown item per style.define glyph key with
// its current value quoted so spaces and empties are visible. Generated from
// styleGlyphValues so a newly added glyph appears here without a second list.
// The value is escaped so a glyph containing a quote or backslash stays a valid,
// copy-pasteable Lua string literal.
std::string renderGlyphList(Style const& style) {
    std::string out;
    for (auto const& [key, value] : styleGlyphValues(style)) {
        out += "- `";
        out += key;
        out += "` = \"";
        for (char c : value) {
            if (c == '\\' || c == '"') out += '\\';
            out += c;
        }
        out += "\"\n";
    }
    return out;
}

std::string humanBindingLabel(CommandCatalog const& catalog,
                              std::string const& commandId) {
    if (auto const* entry = catalog.find(commandId)) {
        return commandLabel(*entry);
    }
    return commandId;
}

// The live keybinding table, one row per binding, formatted as
// "<keys>\t<command label>". Reads the runtime's current keymap, so a user's
// keymap.bind customizations appear here.
std::string renderKeybindings(KeymapViewState const& keymap,
                              CommandCatalog const& catalog) {
    std::vector<std::pair<std::string, std::string>> rows;
    rows.reserve(keymap.bindings.size());
    for (auto const& binding : keymap.bindings) {
        rows.emplace_back(KeyCodec{}.formatSequence(binding.sequence),
                          humanBindingLabel(catalog, binding.commandId));
    }
    std::sort(rows.begin(), rows.end());
    std::string out;
    for (auto const& [keys, label] : rows) {
        out += "  ";
        out += keys;
        out += "  ";
        out += label;
        out += '\n';
    }
    return out;
}

// The full command list as plain Markdown list items grouped by owner -- NOT a
// table, because SSG shows text and a pipe table would render as raw pipes. Each
// row is "- `command.id` -- summary".
std::string renderCommandList(CommandCatalog const& catalog) {
    std::map<std::string_view, std::vector<CommandEntry const*>> byOwner;
    for (auto const* command : catalog.commands()) {
        byOwner[command->owner].push_back(command);
    }
    std::string out;
    for (auto const& [owner, owned] : byOwner) {
        out += "\n### ";
        out += owner;
        out += "\n\n";
        for (auto const* command : owned) {
            out += "- `";
            out += command->id;
            out += '`';
            if (!command->summary.empty()) {
                out += " -- ";
                out += command->summary;
            }
            out += '\n';
        }
    }
    return out;
}

} // namespace

// Assembles the read-only help document: compiled-in prose, then the live
// keybinding table, then configuration help and the full command list.
// Reassembled on every help.open so the generated sections always reflect the
// current keymap and catalog.
std::string buildHelpDocument(EditorRuntime::Impl const& runtime) {
    auto const& catalog = *runtime.session->catalog();
    std::string document{kHelpPreamble};
    document += renderKeybindings(runtime.keymap, catalog);
    document += kHelpConfigSection;
    document += renderGlyphList(runtime.style);
    document += "\n## All commands\n";
    document += renderCommandList(catalog);
    return document;
}

void bindRuntimeHelp(EditorSessionBuilder& builder,
                     EditorRuntime::Impl& runtime) {
    builder.add(
        CommandSpecBuilder{"help.open"}
            .owner("help-system")
            .summary("Open Help")
            .label("Open Help")
            .mutates()
            .lua()
            .handler([&runtime](CommandContext&) {
                return runtime.runTransaction([&] {
                    return runtime.openReadOnlyTab(
                        TabKind::ReadOnlyOutput, "help:main", "Help",
                        buildHelpDocument(runtime), ssg::LanguageId{"markdown"});
                });
            }));
}

} // namespace ssg
