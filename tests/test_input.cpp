#include <ssg/input.h>

#include "test_helpers.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef SSG_SOURCE_PATH
#error "SSG_SOURCE_PATH must name the backend source directory"
#endif

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input{path};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
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

TEST(validate_keymap_flags_duplicate_unreachable_and_reserved_bindings) {
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
    RUN(validate_keymap_flags_duplicate_unreachable_and_reserved_bindings);
    RUN(ime_accepts_only_committed_utf8_text);
    RUN(hit_targets_round_trip_typed_semantic_arguments);
    RUN(backend_has_no_platform_input_capture_dependency);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
