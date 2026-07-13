#include <ssg/focus.h>
#include <ssg/input.h>

#include "test_helpers.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
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

TEST(keymap_contexts_are_star_plus_focus_names) {
    const auto contexts = ssg::keymap_contexts();
    std::set<std::string_view> actual{contexts.begin(), contexts.end()};
    const std::set<std::string_view> expected{"*", "editor", "panel", "prompt"};
    ASSERT_TRUE(actual == expected);
    ASSERT_EQ(ssg::focus_target_name(ssg::FocusTarget::editor),
              std::string_view{"editor"});
    ASSERT_EQ(ssg::focus_target_name(ssg::FocusTarget::panel),
              std::string_view{"panel"});
    ASSERT_EQ(ssg::focus_target_name(ssg::FocusTarget::prompt),
              std::string_view{"prompt"});
}

namespace {

bool has_error(const std::vector<ssg::KeymapError>& errors,
               ssg::KeymapErrorCode code) {
    return std::ranges::any_of(
        errors, [&](const auto& error) { return error.code == code; });
}

}  // namespace

TEST(validate_keymap_rejects_unknown_context) {
    const auto seq = *ssg::parse_key_sequence({"ArrowDown"});
    ssg::KeymapViewState bad{"bad", {{seq, "cursor.line_down", "sidebar"}}};
    ASSERT_TRUE(
        has_error(ssg::validate_keymap(bad, {}), ssg::KeymapErrorCode::unknown_context));

    for (const auto context : {"*", "editor", "panel", "prompt"}) {
        ssg::KeymapViewState good{"ok", {{seq, "cursor.line_down", context}}};
        ASSERT_FALSE(has_error(ssg::validate_keymap(good, {}),
                               ssg::KeymapErrorCode::unknown_context));
    }
}

TEST(validate_keymap_rejects_ambiguous_prefix_order_independently) {
    const auto esc_f = *ssg::parse_key_sequence({"Escape", "KeyF"});
    const auto esc_f_t = *ssg::parse_key_sequence({"Escape", "KeyF", "KeyT"});

    // Same context (both "*"): a strict prefix pair is ambiguous, in either order.
    ssg::KeymapViewState forward{
        "m", {{esc_f, "a", "*"}, {esc_f_t, "b", "*"}}};
    ssg::KeymapViewState reversed{
        "m", {{esc_f_t, "b", "*"}, {esc_f, "a", "*"}}};
    ASSERT_TRUE(has_error(ssg::validate_keymap(forward, {}),
                          ssg::KeymapErrorCode::ambiguous_prefix));
    ASSERT_TRUE(has_error(ssg::validate_keymap(reversed, {}),
                          ssg::KeymapErrorCode::ambiguous_prefix));

    // "*"/focus overlap: a global prefix and a focus continuation collide.
    ssg::KeymapViewState star_focus{
        "m", {{esc_f, "a", "*"}, {esc_f_t, "b", "editor"}}};
    ASSERT_TRUE(has_error(ssg::validate_keymap(star_focus, {}),
                          ssg::KeymapErrorCode::ambiguous_prefix));

    // focus/focus in the SAME context collide.
    ssg::KeymapViewState focus_focus{
        "m", {{esc_f, "a", "editor"}, {esc_f_t, "b", "editor"}}};
    ASSERT_TRUE(has_error(ssg::validate_keymap(focus_focus, {}),
                          ssg::KeymapErrorCode::ambiguous_prefix));

    // DIFFERENT focus contexts do not overlap, so a prefix pair is allowed.
    ssg::KeymapViewState disjoint{
        "m", {{esc_f, "a", "editor"}, {esc_f_t, "b", "panel"}}};
    ASSERT_FALSE(has_error(ssg::validate_keymap(disjoint, {}),
                           ssg::KeymapErrorCode::ambiguous_prefix));
}

TEST(resolve_key_sequence_maps_same_key_per_context) {
    const auto down = *ssg::parse_key_sequence({"ArrowDown"});
    ssg::KeymapViewState keymap{
        "default",
        {{down, "cursor.line_down", "editor"},
         {down, "tree.select_next", "panel"}}};

    const auto in_editor = ssg::resolve_key_sequence(keymap, down, "editor");
    ASSERT_EQ(in_editor.kind, ssg::KeymapMatchKind::resolved);
    ASSERT_EQ(in_editor.command_id, std::string{"cursor.line_down"});

    const auto in_panel = ssg::resolve_key_sequence(keymap, down, "panel");
    ASSERT_EQ(in_panel.kind, ssg::KeymapMatchKind::resolved);
    ASSERT_EQ(in_panel.command_id, std::string{"tree.select_next"});

    // No eligible binding in prompt context.
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, down, "prompt").kind,
              ssg::KeymapMatchKind::none);
}

TEST(resolve_key_sequence_star_beats_focus_and_resolves_everywhere) {
    const auto save = *ssg::parse_key_sequence({"Escape", "KeyS"});
    // A "*" binding and a same-sequence focus binding; "*" must win regardless of
    // which is listed first, and resolve in every context.
    ssg::KeymapViewState focus_first{
        "m", {{save, "focus.only", "editor"}, {save, "file.save", "*"}}};
    ssg::KeymapViewState star_first{
        "m", {{save, "file.save", "*"}, {save, "focus.only", "editor"}}};
    for (const auto* keymap : {&focus_first, &star_first}) {
        for (const auto context : {"editor", "panel", "prompt"}) {
            const auto r = ssg::resolve_key_sequence(*keymap, save, context);
            ASSERT_EQ(r.kind, ssg::KeymapMatchKind::resolved);
            ASSERT_EQ(r.command_id, std::string{"file.save"});
        }
    }
}

TEST(resolve_key_sequence_reports_pending_and_none) {
    const auto esc = *ssg::parse_key_sequence({"Escape"});
    const auto esc_s = *ssg::parse_key_sequence({"Escape", "KeyS"});
    const auto esc_x = *ssg::parse_key_sequence({"Escape", "KeyX"});
    ssg::KeymapViewState keymap{"m", {{esc_s, "file.save", "*"}}};

    ASSERT_EQ(ssg::resolve_key_sequence(keymap, esc, "editor").kind,
              ssg::KeymapMatchKind::pending);
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, esc_s, "editor").kind,
              ssg::KeymapMatchKind::resolved);
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, esc_x, "editor").kind,
              ssg::KeymapMatchKind::none);
    ASSERT_EQ(ssg::resolve_key_sequence(keymap, {}, "editor").kind,
              ssg::KeymapMatchKind::none);
}

TEST(text_routing_is_per_context) {
    ASSERT_EQ(ssg::text_routing("editor"), ssg::TextRouting::insert);
    ASSERT_EQ(ssg::text_routing("prompt"), ssg::TextRouting::prompt_query);
    ASSERT_EQ(ssg::text_routing("panel"), ssg::TextRouting::ignore);
    ASSERT_EQ(ssg::text_routing("*"), ssg::TextRouting::ignore);
}

TEST(has_global_binding_requires_unreserved_unshadowed_star) {
    const auto seq = *ssg::parse_key_sequence({"Escape", "KeyF", "KeyT"});

    ssg::KeymapViewState present{"m", {{seq, "settings.open", "*"}}};
    ASSERT_TRUE(ssg::has_global_binding(present, "settings.open", {}));

    // Focus-context (not global) does not count.
    ssg::KeymapViewState contextual{"m", {{seq, "settings.open", "editor"}}};
    ASSERT_FALSE(ssg::has_global_binding(contextual, "settings.open", {}));

    // Reserved sequence does not count.
    ASSERT_FALSE(ssg::has_global_binding(present, "settings.open", {&seq, 1}));

    // Shadowed by an earlier "*" binding of the same sequence does not count.
    ssg::KeymapViewState shadowed{
        "m", {{seq, "other.command", "*"}, {seq, "settings.open", "*"}}};
    ASSERT_FALSE(ssg::has_global_binding(shadowed, "settings.open", {}));

    // Absent command.
    ASSERT_FALSE(ssg::has_global_binding(present, "file.save", {}));
}

TEST(validate_keymap_flags_global_shadow_regardless_of_order) {
    const auto seq = *ssg::parse_key_sequence({"Escape", "KeyS"});
    // Global-then-focus and focus-then-global must both flag the focus binding.
    ssg::KeymapViewState global_first{
        "m", {{seq, "file.save", "*"}, {seq, "focus.only", "editor"}}};
    ssg::KeymapViewState focus_first{
        "m", {{seq, "focus.only", "editor"}, {seq, "file.save", "*"}}};
    ASSERT_TRUE(has_error(ssg::validate_keymap(global_first, {}),
                          ssg::KeymapErrorCode::unreachable_binding));
    ASSERT_TRUE(has_error(ssg::validate_keymap(focus_first, {}),
                          ssg::KeymapErrorCode::unreachable_binding));
}

TEST(resolver_and_has_global_binding_agree_on_duplicate_globals) {
    const auto seq = *ssg::parse_key_sequence({"Escape", "KeyF", "KeyT"});
    // An invalid map with two "*" bindings for one sequence: the resolver's
    // winner must be the same command has_global_binding calls authoritative.
    for (const auto& first : {std::string{"settings.open"}, std::string{"other.cmd"}}) {
        const std::string second =
            first == "settings.open" ? "other.cmd" : "settings.open";
        ssg::KeymapViewState keymap{
            "m", {{seq, first, "*"}, {seq, second, "*"}}};
        const auto resolved = ssg::resolve_key_sequence(keymap, seq, "editor");
        ASSERT_EQ(resolved.kind, ssg::KeymapMatchKind::resolved);
        // First "*" binding wins in both functions.
        ASSERT_EQ(resolved.command_id, first);
        ASSERT_EQ(ssg::has_global_binding(keymap, first, {}), true);
        ASSERT_EQ(ssg::has_global_binding(keymap, second, {}), false);
    }
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
    RUN(keymap_contexts_are_star_plus_focus_names);
    RUN(validate_keymap_rejects_unknown_context);
    RUN(validate_keymap_rejects_ambiguous_prefix_order_independently);
    RUN(resolve_key_sequence_maps_same_key_per_context);
    RUN(resolve_key_sequence_star_beats_focus_and_resolves_everywhere);
    RUN(resolve_key_sequence_reports_pending_and_none);
    RUN(text_routing_is_per_context);
    RUN(has_global_binding_requires_unreserved_unshadowed_star);
    RUN(validate_keymap_flags_global_shadow_regardless_of_order);
    RUN(resolver_and_has_global_binding_agree_on_duplicate_globals);
    RUN(ime_accepts_only_committed_utf8_text);
    RUN(hit_targets_round_trip_typed_semantic_arguments);
    RUN(backend_has_no_platform_input_capture_dependency);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
