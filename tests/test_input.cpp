#include <ssg/input.h>

#include "test_helpers.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef SSG_DEFAULT_KEYMAP_PATH
#error "SSG_DEFAULT_KEYMAP_PATH must name the authoritative keymap"
#endif
#ifndef SSG_REQUIRED_COMMANDS_PATH
#error "SSG_REQUIRED_COMMANDS_PATH must name the accepted command catalog"
#endif
#ifndef SSG_RESERVED_CHORDS_PATH
#error "SSG_RESERVED_CHORDS_PATH must name the accepted reserved fixture"
#endif
#ifndef SSG_SOURCE_PATH
#error "SSG_SOURCE_PATH must name the backend source directory"
#endif

namespace {

struct Binding {
    ssg::KeySequence sequence;
    std::string command;
    std::string context;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input{path};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::vector<std::string> quoted_values(std::string_view array) {
    std::vector<std::string> result;
    const std::regex value{R"regex("([^"]+)")regex"};
    const std::string input{array};
    for (auto it = std::sregex_iterator(input.begin(), input.end(), value);
         it != std::sregex_iterator(); ++it) {
        result.push_back((*it)[1].str());
    }
    return result;
}

std::optional<std::string> string_field(const std::string& object,
                                        std::string_view name) {
    const std::regex expression{"\"" + std::string{name} +
                                R"regex("\s*:\s*"([^"]+)")regex"};
    std::smatch match;
    if (!std::regex_search(object, match, expression)) {
        return std::nullopt;
    }
    return match[1].str();
}

std::optional<std::vector<std::string>> array_field(
    const std::string& object, std::string_view name) {
    const std::regex expression{"\"" + std::string{name} +
                                R"("\s*:\s*\[([^\]]*)\])"};
    std::smatch match;
    if (!std::regex_search(object, match, expression)) {
        return std::nullopt;
    }
    return quoted_values(match[1].str());
}

std::vector<Binding> load_bindings() {
    const auto json = read_file(SSG_DEFAULT_KEYMAP_PATH);
    const auto prefix = string_field(json, "prefix");
    const auto alphabet = string_field(json, "alphabet");
    const auto context = string_field(json, "when");
    const auto commands = array_field(json, "commands");
    ASSERT_TRUE(prefix.has_value());
    ASSERT_TRUE(alphabet.has_value());
    ASSERT_TRUE(context.has_value());
    ASSERT_TRUE(commands.has_value());
    std::vector<Binding> result;
    if (!prefix || !alphabet || !context || !commands ||
        alphabet->empty() ||
        commands->size() > alphabet->size() * alphabet->size()) {
        return result;
    }
    const auto prefix_stroke = ssg::parse_key_stroke(*prefix);
    ASSERT_TRUE(prefix_stroke.has_value());
    if (!prefix_stroke) {
        return result;
    }
    for (std::size_t index = 0; index < commands->size(); ++index) {
        const std::string first{"Key" +
                                std::string(1, (*alphabet)[index /
                                                          alphabet->size()])};
        const std::string second{"Key" +
                                 std::string(1, (*alphabet)[index %
                                                           alphabet->size()])};
        const auto first_stroke = ssg::parse_key_stroke(first);
        const auto second_stroke = ssg::parse_key_stroke(second);
        ASSERT_TRUE(first_stroke.has_value());
        ASSERT_TRUE(second_stroke.has_value());
        if (first_stroke && second_stroke) {
            result.push_back(
                {{*prefix_stroke, *first_stroke, *second_stroke},
                 (*commands)[index], *context});
        }
    }
    return result;
}

std::set<std::string> eligible_commands() {
    const auto json = read_file(SSG_REQUIRED_COMMANDS_PATH);
    std::set<std::string> result;
    const std::regex object{R"(\{([^{}]*"id"[^{}]*)\})"};
    for (auto it = std::sregex_iterator(json.begin(), json.end(), object);
         it != std::sregex_iterator(); ++it) {
        const auto text = (*it)[1].str();
        const auto id = string_field(text, "id");
        ASSERT_TRUE(id.has_value());
        const bool eligible =
            std::regex_search(text, std::regex{R"("keymap"\s*:\s*true)"});
        if (id && eligible) {
            result.insert(*id);
        }
    }
    return result;
}

std::vector<ssg::KeySequence> reserved_sequences() {
    const auto json = read_file(SSG_RESERVED_CHORDS_PATH);
    std::vector<ssg::KeySequence> result;
    const std::regex object{R"(\{([^{}]*"browser"[^{}]*)\})"};
    for (auto it = std::sregex_iterator(json.begin(), json.end(), object);
         it != std::sregex_iterator(); ++it) {
        const auto strokes = array_field((*it)[1].str(), "sequence");
        ASSERT_TRUE(strokes.has_value());
        if (!strokes) {
            continue;
        }
        ssg::KeySequence sequence;
        for (const auto& encoded : *strokes) {
            const auto stroke = ssg::parse_key_stroke(encoded);
            ASSERT_TRUE(stroke.has_value());
            if (stroke) {
                sequence.push_back(*stroke);
            }
        }
        result.push_back(std::move(sequence));
    }
    return result;
}

TEST(key_strokes_have_a_canonical_round_trip) {
    for (const auto text : {"KeyA", "Ctrl+Shift+KeyM", "Meta+BracketLeft",
                            "Alt+ArrowRight", "F5"}) {
        const auto parsed = ssg::parse_key_stroke(text);
        ASSERT_TRUE(parsed.has_value());
        if (parsed) {
            ASSERT_EQ(ssg::format_key_stroke(*parsed), std::string{text});
        }
    }
    ASSERT_FALSE(ssg::parse_key_stroke("").has_value());
    ASSERT_FALSE(ssg::parse_key_stroke("Ctrl+Ctrl+KeyA").has_value());
    ASSERT_FALSE(ssg::parse_key_stroke("Hyper+KeyA").has_value());
    ASSERT_FALSE(ssg::parse_key_stroke("Ctrl+").has_value());
}

TEST(default_keymap_exactly_covers_keymap_eligible_commands) {
    const auto bindings = load_bindings();
    const auto expected = eligible_commands();
    std::set<std::string> actual;
    for (const auto& binding : bindings) {
        ASSERT_TRUE(actual.insert(binding.command).second);
    }
    ASSERT_EQ(expected.size(), std::size_t{160});
    ASSERT_EQ(actual, expected);
    ASSERT_FALSE(actual.contains("file.open_dropped_content"));
}

TEST(default_keymap_rejects_duplicate_unreachable_and_reserved_bindings) {
    const auto bindings = load_bindings();
    ssg::KeymapViewState keymap{"default", {}};
    for (const auto& binding : bindings) {
        keymap.bindings.push_back(
            {binding.sequence, binding.command, binding.context});
    }
    ASSERT_TRUE(ssg::validate_keymap(keymap, reserved_sequences()).empty());

    const auto sequence = *ssg::parse_key_sequence(
        {"Ctrl+Shift+KeyM", "KeyA", "KeyA"});
    ssg::KeymapViewState duplicate{
        "bad", {{sequence, "cursor.left", "editor"},
                {sequence, "cursor.right", "editor"}}};
    const auto duplicate_errors = ssg::validate_keymap(duplicate, {});
    ASSERT_TRUE(std::ranges::any_of(duplicate_errors, [](const auto& error) {
        return error.code == ssg::KeymapErrorCode::duplicate_binding;
    }));

    ssg::KeymapViewState unreachable{
        "bad", {{sequence, "cursor.left", "*"},
                {sequence, "cursor.right", "editor"}}};
    const auto unreachable_errors = ssg::validate_keymap(unreachable, {});
    ASSERT_TRUE(std::ranges::any_of(unreachable_errors, [](const auto& error) {
        return error.code == ssg::KeymapErrorCode::unreachable_binding;
    }));

    const auto reserved = *ssg::parse_key_sequence({"Ctrl+KeyL"});
    ssg::KeymapViewState reserved_map{
        "bad", {{reserved, "cursor.left", "*"}}};
    const auto reserved_errors =
        ssg::validate_keymap(reserved_map, {&reserved, 1});
    ASSERT_TRUE(std::ranges::any_of(reserved_errors, [](const auto& error) {
        return error.code == ssg::KeymapErrorCode::reserved_binding;
    }));
    const auto longer = *ssg::parse_key_sequence({"Ctrl+KeyL", "KeyA"});
    ssg::KeymapViewState reserved_prefix_map{
        "bad", {{longer, "cursor.left", "*"}}};
    const auto prefix_errors =
        ssg::validate_keymap(reserved_prefix_map, {&reserved, 1});
    ASSERT_TRUE(std::ranges::any_of(prefix_errors, [](const auto& error) {
        return error.code == ssg::KeymapErrorCode::reserved_binding;
    }));
}

TEST(ime_accepts_only_committed_utf8_text) {
    const auto committed =
        ssg::CommittedText::from_utf8("e\xCC\x81 \xF0\x9F\x98\x80");
    ASSERT_TRUE(committed.has_value());
    if (committed) {
        const auto semantic = ssg::semantic_input(*committed);
        ASSERT_EQ(semantic.command_id, std::string{"text.insert"});
        ASSERT_EQ(std::get<ssg::TextInputArguments>(semantic.arguments).text,
                  committed->utf8());
    }
    ASSERT_FALSE(ssg::CommittedText::from_utf8(std::string{"\xC0\xAF", 2})
                     .has_value());
    ASSERT_FALSE(ssg::CommittedText::from_utf8(std::string{"a\0b", 3})
                     .has_value());
    ASSERT_FALSE(ssg::CommittedText::from_utf8("").has_value());
}

TEST(hit_targets_round_trip_typed_semantic_arguments) {
    ssg::SemanticCommand command{
        "cursor.set_position",
        ssg::SelectionCommandArguments{ssg::DocumentPosition{
                                           ssg::ByteOffset{7},
                                           ssg::LineIndex{2},
                                           ssg::CellIndex{4}},
                                       std::nullopt}};
    const ssg::SemanticHitTarget target{
        42, ssg::HitTargetKind::editor_cell, "document cell", command};
    ASSERT_EQ(ssg::activate_hit_target(target), command);

    const auto& arguments =
        std::get<ssg::SelectionCommandArguments>(target.command.arguments);
    ASSERT_TRUE(arguments.position.has_value());
    if (arguments.position) {
        ASSERT_EQ(arguments.position->byte_offset, ssg::ByteOffset{7});
        ASSERT_EQ(arguments.position->line, ssg::LineIndex{2});
        ASSERT_EQ(arguments.position->cell, ssg::CellIndex{4});
    }

    const ssg::SemanticCommand scroll{
        "view.scroll_to_fraction", ssg::ScrollFractionArguments{3, 7}};
    const ssg::SemanticHitTarget scrollbar{
        43, ssg::HitTargetKind::scrollbar, "scrollbar", scroll};
    ASSERT_EQ(ssg::activate_hit_target(scrollbar), scroll);
}

TEST(backend_has_no_platform_input_capture_dependency) {
    const std::vector<std::string> forbidden{
        "KeyboardEvent", "keydown", "compositionstart", "compositionupdate",
        "navigator.clipboard", "addEventListener", "GetAsyncKeyState",
        "ReadConsoleInput"};
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator{SSG_SOURCE_PATH}) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
            continue;
        }
        const auto source = read_file(entry.path());
        for (const auto& token : forbidden) {
            ASSERT_TRUE(source.find(token) == std::string::npos);
        }
    }
}

} // namespace

int main() {
    RUN(key_strokes_have_a_canonical_round_trip);
    RUN(default_keymap_exactly_covers_keymap_eligible_commands);
    RUN(default_keymap_rejects_duplicate_unreachable_and_reserved_bindings);
    RUN(ime_accepts_only_committed_utf8_text);
    RUN(hit_targets_round_trip_typed_semantic_arguments);
    RUN(backend_has_no_platform_input_capture_dependency);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
