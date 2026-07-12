#include "test_helpers.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef SSG_REQUIRED_COMMANDS_PATH
#error "SSG_REQUIRED_COMMANDS_PATH must name the accepted catalog"
#endif

namespace {

struct ExpectedCommand {
    std::string_view id;
    std::string_view owner;
};

struct CatalogCommand {
    std::string id;
    std::string owner;
    std::vector<std::string> required_capabilities;
    bool lua{};
    bool keymap{};
    bool palette{};
};

constexpr auto expected_commands = std::to_array<ExpectedCommand>({
    {"text.insert", "text-input-commands"},
    {"text.newline", "text-input-commands"},
    {"text.delete_backward", "text-input-commands"},
    {"text.delete_forward", "text-input-commands"},
    {"text.delete_word_backward", "text-input-commands"},
    {"text.delete_word_forward", "text-input-commands"},
    {"cursor.set_position", "selection-navigation"},
    {"cursor.left", "selection-navigation"},
    {"cursor.right", "selection-navigation"},
    {"cursor.word_left", "selection-navigation"},
    {"cursor.word_right", "selection-navigation"},
    {"cursor.line_up", "selection-navigation"},
    {"cursor.line_down", "selection-navigation"},
    {"cursor.line_start", "selection-navigation"},
    {"cursor.line_end", "selection-navigation"},
    {"cursor.page_up", "selection-navigation"},
    {"cursor.page_down", "selection-navigation"},
    {"cursor.document_start", "selection-navigation"},
    {"cursor.document_end", "selection-navigation"},
    {"select.set_range", "selection-navigation"},
    {"select.add_range", "selection-navigation"},
    {"select.left", "selection-navigation"},
    {"select.right", "selection-navigation"},
    {"select.word_left", "selection-navigation"},
    {"select.word_right", "selection-navigation"},
    {"select.line_up", "selection-navigation"},
    {"select.line_down", "selection-navigation"},
    {"select.line_start", "selection-navigation"},
    {"select.line_end", "selection-navigation"},
    {"select.page_up", "selection-navigation"},
    {"select.page_down", "selection-navigation"},
    {"select.document_start", "selection-navigation"},
    {"select.document_end", "selection-navigation"},
    {"select.all", "selection-navigation"},
    {"select.add_next_occurrence", "selection-navigation"},
    {"select.add_cursor_up", "selection-navigation"},
    {"select.add_cursor_down", "selection-navigation"},
    {"select.split_into_lines", "selection-navigation"},
    {"select.to_matching_bracket", "selection-navigation"},
    {"edit.undo", "undo-redo-history"},
    {"edit.redo", "undo-redo-history"},
    {"edit.indent", "edit-command-suite"},
    {"edit.outdent", "edit-command-suite"},
    {"edit.duplicate_line", "edit-command-suite"},
    {"edit.move_line_up", "edit-command-suite"},
    {"edit.move_line_down", "edit-command-suite"},
    {"edit.delete_line", "edit-command-suite"},
    {"edit.join_lines", "edit-command-suite"},
    {"edit.uppercase", "edit-command-suite"},
    {"edit.lowercase", "edit-command-suite"},
    {"edit.swap_case", "edit-command-suite"},
    {"edit.sort_lines", "edit-command-suite"},
    {"edit.transpose", "edit-command-suite"},
    {"edit.toggle_comment", "edit-command-suite"},
    {"clipboard.copy", "clipboard-register"},
    {"clipboard.cut", "clipboard-register"},
    {"clipboard.paste", "clipboard-register"},
    {"view.toggle_word_wrap", "viewport-wrap-scrollbar"},
    {"view.scroll_lines", "viewport-wrap-scrollbar"},
    {"view.scroll_pages", "viewport-wrap-scrollbar"},
    {"view.scroll_to_fraction", "viewport-wrap-scrollbar"},
    {"view.reveal_caret", "selection-navigation"},
    {"view.center_caret", "selection-navigation"},
    {"palette.open", "search-palette"},
    {"palette.close", "search-palette"},
    {"palette.next", "search-palette"},
    {"palette.previous", "search-palette"},
    {"palette.execute", "search-palette"},
    {"goto.file", "search-palette"},
    {"goto.line", "search-palette"},
    {"goto.symbol", "search-palette"},
    {"goto.definition", "lsp-language-features"},
    {"goto.reference", "lsp-language-features"},
    {"goto.matching_bracket", "selection-navigation"},
    {"goto.back", "search-palette"},
    {"goto.forward", "search-palette"},
    {"find.open", "find-replace"},
    {"find.close", "find-replace"},
    {"find.next", "find-replace"},
    {"find.previous", "find-replace"},
    {"find.toggle_case", "find-replace"},
    {"find.toggle_whole_word", "find-replace"},
    {"find.toggle_regex", "find-replace"},
    {"find.toggle_selection", "find-replace"},
    {"replace.open", "find-replace"},
    {"replace.current", "find-replace"},
    {"replace.all", "find-replace"},
    {"replace.workspace_preview", "find-replace"},
    {"replace.workspace_apply", "find-replace"},
    {"search.workspace", "search-palette"},
    {"search.results_next", "search-palette"},
    {"search.results_previous", "search-palette"},
    {"completion.open", "lsp-language-features"},
    {"completion.next", "lsp-language-features"},
    {"completion.previous", "lsp-language-features"},
    {"completion.accept", "lsp-language-features"},
    {"completion.dismiss", "lsp-language-features"},
    {"hover.show", "lsp-language-features"},
    {"hover.dismiss", "lsp-language-features"},
    {"rename.symbol", "lsp-workspace-edits"},
    {"pane.split_horizontal", "shell-layout"},
    {"pane.split_vertical", "shell-layout"},
    {"pane.close", "shell-layout"},
    {"pane.next", "shell-layout"},
    {"pane.previous", "shell-layout"},
    {"pane.focus_left", "shell-layout"},
    {"pane.focus_right", "shell-layout"},
    {"pane.focus_up", "shell-layout"},
    {"pane.focus_down", "shell-layout"},
    {"panel.toggle", "shell-layout"},
    {"panel.focus", "shell-layout"},
    {"panel.next_provider", "shell-layout"},
    {"panel.previous_provider", "shell-layout"},
    {"tree.toggle_expanded", "tree-providers"},
    {"tree.invoke_node_command", "tree-providers"},
    {"view.toggle_distraction_free", "shell-layout"},
    {"prompt.submit", "prompt-status-surface"},
    {"prompt.cancel", "prompt-status-surface"},
    {"status.next", "prompt-status-surface"},
    {"status.previous", "prompt-status-surface"},
    {"status.dismiss", "prompt-status-surface"},
    {"status.invoke_action", "prompt-status-surface"},
    {"workspace.open_directory", "file-commands"},
    {"file.new", "file-commands"},
    {"file.open", "file-commands"},
    {"file.open_recent", "file-commands"},
    {"file.open_dropped_content", "file-commands"},
    {"file.save", "file-commands"},
    {"file.save_all", "file-commands"},
    {"file.save_as", "file-commands"},
    {"file.reload", "file-commands"},
    {"file.rename", "file-commands"},
    {"file.delete", "file-commands"},
    {"file.new_directory", "file-commands"},
    {"file.reopen_with_encoding", "encoding-eol"},
    {"file.set_encoding", "encoding-eol"},
    {"file.set_line_ending", "encoding-eol"},
    {"file.set_final_newline", "encoding-eol"},
    {"tab.close", "tab-management"},
    {"tab.close_others", "tab-management"},
    {"tab.close_all", "tab-management"},
    {"tab.reopen_closed", "tab-management"},
    {"tab.next", "tab-management"},
    {"tab.previous", "tab-management"},
    {"tab.activate", "tab-management"},
    {"tab.move_left", "tab-management"},
    {"tab.move_right", "tab-management"},
    {"external.reload", "external-modification-flow"},
    {"external.keep_buffer", "external-modification-flow"},
    {"external.open_diff", "external-modification-flow"},
    {"settings.open", "settings-model"},
    {"settings.set", "settings-model"},
    {"settings.reset", "settings-model"},
    {"settings.reset_scope", "settings-model"},
    {"settings.export_workspace", "settings-model"},
    {"settings.import_workspace", "settings-model"},
    {"follow_edits.resume", "follow-edits"},
    {"follow_edits.pause", "follow-edits"},
    {"diff.next_hunk", "diff-model"},
    {"diff.previous_hunk", "diff-model"},
    {"diff.open_file", "diff-model"},
});

constexpr auto expected_category_counts =
    std::to_array<std::pair<std::string_view, std::size_t>>({
        {"text", 6},       {"cursor", 13},   {"select", 20},
        {"edit", 15},      {"clipboard", 3}, {"view", 7},
        {"palette", 5},    {"goto", 8},      {"find", 8},
        {"replace", 5},    {"search", 3},    {"completion", 5},
        {"hover", 2},      {"rename", 1},    {"pane", 9},
        {"panel", 4},
        {"tree", 2},
        {"prompt", 2},     {"status", 4},    {"workspace", 1},
        {"file", 15},      {"tab", 9},       {"external", 3},
        {"settings", 6},   {"follow_edits", 2}, {"diff", 3},
    });

static_assert(expected_commands.size() == 161);

std::optional<std::string> field(const std::string& object,
                                 const std::string& name) {
    const std::regex expression{
        "\"" + name + R"regex("\s*:\s*"([^"]*)")regex"};
    std::smatch match;
    if (!std::regex_search(object, match, expression)) {
        return std::nullopt;
    }
    return match[1].str();
}

std::optional<bool> boolean_field(const std::string& object,
                                  const std::string& name) {
    const std::regex expression{"\"" + name + R"("\s*:\s*(true|false))"};
    std::smatch match;
    if (!std::regex_search(object, match, expression)) {
        return std::nullopt;
    }
    return match[1].str() == "true";
}

std::optional<std::vector<std::string>> capabilities_field(
    const std::string& object) {
    const std::regex expression{R"("required_capabilities"\s*:\s*\[([^\]]*)\])"};
    std::smatch match;
    if (!std::regex_search(object, match, expression)) {
        return std::nullopt;
    }
    std::vector<std::string> capabilities;
    const std::string contents = match[1].str();
    const std::regex value_expression{R"regex("([^"]+)")regex"};
    for (auto it = std::sregex_iterator(contents.begin(), contents.end(),
                                        value_expression);
         it != std::sregex_iterator(); ++it) {
        capabilities.push_back((*it)[1].str());
    }
    return capabilities;
}

std::optional<std::vector<CatalogCommand>> load_catalog() {
    std::ifstream input{SSG_REQUIRED_COMMANDS_PATH};
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto json = buffer.str();

    std::vector<CatalogCommand> commands;
    const std::regex object_expression{R"(\{([^{}]*)\})"};
    for (auto it = std::sregex_iterator(json.begin(), json.end(),
                                        object_expression);
         it != std::sregex_iterator(); ++it) {
        const auto object = (*it)[1].str();
        const auto id = field(object, "id");
        if (!id) {
            continue;
        }
        const auto owner = field(object, "owner");
        const auto capabilities = capabilities_field(object);
        const auto lua = boolean_field(object, "lua");
        const auto keymap = boolean_field(object, "keymap");
        const auto palette = boolean_field(object, "palette");
        if (!owner || !capabilities || !lua || !keymap || !palette) {
            return std::nullopt;
        }
        commands.push_back(
            {*id, *owner, *capabilities, *lua, *keymap, *palette});
    }

    const std::regex id_token{R"("id"\s*:)"};
    const auto id_count = static_cast<std::size_t>(
        std::distance(std::sregex_iterator(json.begin(), json.end(), id_token),
                      std::sregex_iterator()));
    if (id_count != commands.size()) {
        return std::nullopt;
    }
    return commands;
}

std::map<std::string, ExpectedCommand> expected_by_id() {
    std::map<std::string, ExpectedCommand> expected;
    for (const auto& command : expected_commands) {
        expected.emplace(std::string{command.id}, command);
    }
    return expected;
}

std::set<std::string> feature_spec_commands() {
    const std::regex command_id{
        R"(^[a-z][a-z0-9_]*\.[a-z][a-z0-9_]*$)"};
    const std::regex backticked{
        R"(`([^`]+)`)"};
    std::set<std::string> result;

    for (const auto& entry :
         std::filesystem::directory_iterator{SSG_FEATURE_DOCS_PATH}) {
        if (entry.path().extension() != ".md") {
            continue;
        }
        std::ifstream input{entry.path()};
        const std::string document{std::istreambuf_iterator<char>{input},
                                   std::istreambuf_iterator<char>{}};
        for (auto match = std::sregex_iterator{document.begin(), document.end(),
                                               backticked};
             match != std::sregex_iterator{}; ++match) {
            const auto token = (*match)[1].str();
            if (std::regex_match(token, command_id) && !token.ends_with(".h")) {
                result.insert(token);
            }
        }
    }
    return result;
}

TEST(catalog_exactly_matches_independent_id_and_owner_oracle) {
    const auto catalog = load_catalog();
    ASSERT_TRUE(catalog.has_value());
    if (!catalog) {
        return;
    }

    const auto expected = expected_by_id();
    ASSERT_EQ(expected.size(), expected_commands.size());
    ASSERT_EQ(catalog->size(), expected_commands.size());

    std::set<std::string> seen;
    for (const auto& command : *catalog) {
        const auto expected_command = expected.find(command.id);
        ASSERT_TRUE(expected_command != expected.end());
        if (expected_command != expected.end()) {
            ASSERT_EQ(command.owner,
                      std::string{expected_command->second.owner});
        }
        ASSERT_TRUE(seen.insert(command.id).second);
        ASSERT_FALSE(command.owner.empty());
    }
    ASSERT_EQ(seen.size(), expected_commands.size());
}

TEST(feature_spec_command_union_exactly_matches_catalog) {
    const auto catalog = load_catalog();
    ASSERT_TRUE(catalog.has_value());
    if (!catalog) {
        return;
    }

    std::set<std::string> catalog_ids;
    for (const auto& command : *catalog) {
        catalog_ids.insert(command.id);
    }
    ASSERT_EQ(feature_spec_commands(), catalog_ids);
}

TEST(category_counts_are_independently_fixed) {
    const auto catalog = load_catalog();
    ASSERT_TRUE(catalog.has_value());
    if (!catalog) {
        return;
    }

    std::map<std::string, std::size_t> actual;
    for (const auto& command : *catalog) {
        const auto separator = command.id.find('.');
        ASSERT_TRUE(separator != std::string::npos);
        if (separator != std::string::npos) {
            ++actual[command.id.substr(0, separator)];
        }
    }

    ASSERT_EQ(actual.size(), expected_category_counts.size());
    for (const auto& [category, count] : expected_category_counts) {
        ASSERT_EQ(actual[std::string{category}], count);
    }
}

TEST(ids_are_exact_not_fuzzy_and_all_fields_are_owned) {
    const auto catalog = load_catalog();
    ASSERT_TRUE(catalog.has_value());
    if (!catalog) {
        return;
    }

    const auto expected = expected_by_id();
    const std::regex valid_id{
        R"(^[a-z][a-z0-9_]*\.[a-z][a-z0-9_]*$)"};
    for (const auto& command : *catalog) {
        ASSERT_TRUE(std::regex_match(command.id, valid_id));
        ASSERT_TRUE(expected.contains(command.id));
        ASSERT_FALSE(command.owner.empty());
    }
}

TEST(capability_and_surface_exclusions_are_exact) {
    const auto catalog = load_catalog();
    ASSERT_TRUE(catalog.has_value());
    if (!catalog) {
        return;
    }

    for (const auto& command : *catalog) {
        if (command.id == "file.open_dropped_content") {
            ASSERT_EQ(command.required_capabilities,
                      std::vector<std::string>{"local_file_drop"});
            ASSERT_FALSE(command.lua);
            ASSERT_FALSE(command.keymap);
            ASSERT_FALSE(command.palette);
        } else {
            ASSERT_TRUE(command.required_capabilities.empty());
            ASSERT_TRUE(command.lua);
            ASSERT_TRUE(command.keymap);
            ASSERT_TRUE(command.palette);
        }
    }
}

} // namespace

int main() {
    RUN(catalog_exactly_matches_independent_id_and_owner_oracle);
    RUN(feature_spec_command_union_exactly_matches_catalog);
    RUN(category_counts_are_independently_fixed);
    RUN(ids_are_exact_not_fuzzy_and_all_fields_are_owned);
    RUN(capability_and_surface_exclusions_are_exact);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
