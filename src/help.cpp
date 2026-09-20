#include <ssg/Editor.h>

#include <ssg/Keymap.h>
#include <ssg/Style.h>
#include <ssg/SyntaxModel.h>

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
    "## How keys work\n"
    "\n"
    "SSG has one chord modifier, written Mod. **Mod is Ctrl or Alt** -- press\n"
    "whichever your terminal passes through, and both do the same thing. Ctrl and\n"
    "Alt together is never a Mod chord; that combination is left to your window\n"
    "manager.\n"
    "\n"
    "A few chords are reachable only from Alt: a terminal sends Ctrl+I, Ctrl+M,\n"
    "Ctrl+H and Ctrl+[ as Tab, Enter, Backspace and Escape, so no Ctrl-ness\n"
    "survives for SSG to read.\n"
    "\n"
    "Close this tab like any other with Mod+w.\n"
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
    "- Mod+d adds a cursor at the next occurrence of the current selection, so you\n"
    "  can select a word and press it repeatedly to edit each match.\n"
    "- Mod+j and Mod+k add a cursor on the line below or above.\n"
    "- Mod+i splits a multi-line selection into one cursor per line.\n"
    "- Alt+click adds a cursor where you click; Alt+drag adds a selection. Both\n"
    "  keep the cursors you already have. These are Alt specifically -- a mouse\n"
    "  chord, not a Mod chord.\n"
    "- Mod+a selects everything.\n"
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
    "SSG has a compiled-in configuration hook: edit src/UserConfig.cpp,\n"
    "implement applyUserConfig(Editor&), and rebuild. That hook can call\n"
    "applyThemeSet, applyStyleDefine, applyKeymapBind, and applyKeymapUnbind.\n"
    "\n"
    "Example:\n"
    "\n"
    "    if (auto result = applyKeymapBind(editor, {\"Mod+KeyH\", \"help.open\", \"*\"}); !result.accepted) {\n"
    "        throw std::runtime_error{result.message};\n"
    "    }\n"
    "\n"
    "### Chrome glyphs\n"
    "\n"
    "style.define replaces any of these glyphs; the value shown is what is drawn\n"
    "now. Most must keep their current width, but the tab edge and separator\n"
    "glyphs may be any width. Example:\n"
    "\n"
    "    if (auto result = applyStyleDefine(editor, {{{\"tab_separator\", \" | \"}}}); !result.accepted) {\n"
    "        throw std::runtime_error{result.message};\n"
    "    }\n"
    "\n";

// The chrome-glyph listing, one Markdown item per style.define glyph key with
// its current value quoted so spaces and empties are visible. Generated from
// Style::glyphValues so a newly added glyph appears here without a second list.
// The value is escaped so a glyph containing a quote or backslash stays a valid,
// copy-pasteable C++ string literal.
std::string renderGlyphList(Style const& style) {
    std::string out;
    for (auto const& [key, value] : style.glyphValues()) {
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

std::string humanBindingLabel(Commands const& commands,
                              std::string const& commandId) {
    if (auto const* command = commands.find(commandId)) {
        return command->label;
    }
    return commandId;
}

// The live keybinding table, one row per binding, formatted as
// "<keys>\t<command label>". Reads the runtime's current keymap, so a user's
// keymap.bind customizations appear here.
std::string renderKeybindings(KeymapViewState const& keymap,
                              Commands const& commands) {
    std::vector<std::pair<std::string, std::string>> rows;
    rows.reserve(keymap.bindings.size());
    for (auto const& binding : keymap.bindings) {
        rows.emplace_back(formatKeySequence(binding.sequence),
                          humanBindingLabel(commands, binding.commandId));
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

// The full command list as plain Markdown list items. The ordered registry is
// the user-command source of truth.
std::string renderCommandList(Commands const& commands) {
    std::string out;
    for (auto const& [id, command] : commands.all()) {
        out += "- `";
        out += id;
        out += "` -- ";
        out += command.label;
        out += '\n';
    }
    return out;
}

} // namespace

// Assembles the read-only help document: compiled-in prose, then the live
// keybinding table, then configuration help and the full command list.
// Reassembled on every help.open so the generated sections always reflect the
// current keymap and registry.
std::string buildHelpDocument(Editor const& runtime) {
    std::string document{kHelpPreamble};
    document += renderKeybindings(runtime.keymap, runtime.commands);
    document += kHelpConfigSection;
    document += renderGlyphList(runtime.style);
    document += "\n## All commands\n";
    document += renderCommandList(runtime.commands);
    return document;
}

void bindRuntimeHelp(Commands& commands, Editor& runtime) {
    commands.add("help.open", "Open Help", [&runtime] {
            return runtime.openReadOnlyTab(
                TabKind::ReadOnlyOutput, "help:main", "help",
                buildHelpDocument(runtime), ssg::LanguageId{"markdown"});
    });
}

} // namespace ssg
