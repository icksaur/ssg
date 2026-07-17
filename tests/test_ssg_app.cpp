#include "pointer_routing.h"
#include "ssg_terminal.h"

#include <ssg/hit_test.h>
#include <ssg/selection.h>

#include "test_helpers.h"

#include <any>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

TEST(resolve_launch_no_argument_opens_cwd) {
    auto target = ssg::app::resolve_launch({});
    ASSERT_EQ(target.cwd, fs::current_path());
    ASSERT_FALSE(target.file.has_value());
}

TEST(resolve_launch_directory_opens_that_directory) {
    auto dir = fs::temp_directory_path() / "ssg-app-dir-case";
    fs::create_directories(dir);
    auto target = ssg::app::resolve_launch(dir);
    ASSERT_EQ(target.cwd, fs::absolute(dir));
    ASSERT_FALSE(target.file.has_value());
}

TEST(resolve_launch_file_opens_parent_directory_and_file) {
    auto dir = fs::temp_directory_path() / "ssg-app-file-case";
    fs::create_directories(dir);
    auto file = dir / "hello.txt";
    std::ofstream{file} << "hi";
    auto target = ssg::app::resolve_launch(file);
    ASSERT_EQ(target.cwd, fs::absolute(dir));
    ASSERT_TRUE(target.file.has_value());
    ASSERT_EQ(*target.file, std::string{"hello.txt"});
}

TEST(encode_ansi_frame_addresses_rows_and_emits_palette_colors) {
    ssg::CellGrid screen;
    screen.size = {2, 1};
    screen.palette[0] = {10, 20, 30};
    screen.palette[1] = {200, 100, 50};
    ssg::CellGridCell left;
    left.text = "X";
    left.foreground = 1;
    left.background = 0;
    ssg::CellGridCell right;
    right.text = "Y";
    right.foreground = 1;
    right.background = 0;
    screen.cells = {left, right};

    auto frame = ssg::app::encode_ansi_frame(screen);
    ASSERT_TRUE(frame.find("\x1b[1;1H") != std::string::npos);
    ASSERT_TRUE(frame.find("\x1b[38;2;200;100;50m") != std::string::npos);
    ASSERT_TRUE(frame.find("\x1b[48;2;10;20;30m") != std::string::npos);
    ASSERT_TRUE(frame.find('X') != std::string::npos);
    ASSERT_TRUE(frame.find('Y') != std::string::npos);
    // The two cells share fg/bg, so the color escape is emitted once.
    auto first = frame.find("\x1b[38;2;200;100;50m");
    ASSERT_TRUE(frame.find("\x1b[38;2;200;100;50m", first + 1) ==
                std::string::npos);
}

TEST(encode_ansi_frame_skips_wide_glyph_continuation) {
    ssg::CellGrid screen;
    screen.size = {2, 1};
    screen.palette[0] = {0, 0, 0};
    ssg::CellGridCell wide;
    wide.text = "\xe4\xb8\xad";  // U+4E2D, a double-width glyph.
    ssg::CellGridCell continuation;
    continuation.continuation = true;
    continuation.text = " ";
    screen.cells = {wide, continuation};

    auto frame = ssg::app::encode_ansi_frame(screen);
    ASSERT_TRUE(frame.find("\xe4\xb8\xad") != std::string::npos);
    // The wide glyph occupies both columns; the continuation adds no glyph.
    auto row_start = frame.find("\x1b[1;1H");
    ASSERT_TRUE(row_start != std::string::npos);
    auto glyph = frame.find("\xe4\xb8\xad", row_start);
    ASSERT_TRUE(frame.find(' ', glyph + 3) == std::string::npos ||
                frame.find("\x1b[0m", glyph) < frame.find(' ', glyph + 3));
}

TEST(decode_input_maps_printables_and_named_keys) {
    std::size_t consumed = 0;
    // Lowercase letter: KeyA stroke (no shift) plus committed text.
    auto a = ssg::app::decode_input("a", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_TRUE(a.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(a.stroke.code, std::string{"KeyA"});
    ASSERT_FALSE(a.stroke.shift);
    ASSERT_EQ(a.text, std::string{"a"});

    // Uppercase: Shift+KeyZ plus committed text "Z".
    auto z = ssg::app::decode_input("Z", true, consumed);
    ASSERT_EQ(z.stroke.code, std::string{"KeyZ"});
    ASSERT_TRUE(z.stroke.shift);
    ASSERT_EQ(z.text, std::string{"Z"});

    // Bracket punctuation used by tab chords.
    auto bracket = ssg::app::decode_input("]", true, consumed);
    ASSERT_EQ(bracket.stroke.code, std::string{"BracketRight"});
    ASSERT_EQ(bracket.text, std::string{"]"});

    // Enter and Backspace are strokes without committed text.
    auto enter = ssg::app::decode_input("\r", true, consumed);
    ASSERT_EQ(enter.stroke.code, std::string{"Enter"});
    ASSERT_TRUE(enter.text.empty());
    auto back = ssg::app::decode_input("\x7f", true, consumed);
    ASSERT_EQ(back.stroke.code, std::string{"Backspace"});
}

TEST(decode_input_modified_arrows) {
    std::size_t consumed = 0;
    // Shift+ArrowUp: ESC [ 1 ; 2 A (modifier 2 -> bitmask 1 = Shift).
    auto shift_up = ssg::app::decode_input("\x1b[1;2A", true, consumed);
    ASSERT_EQ(consumed, std::size_t{6});
    ASSERT_TRUE(shift_up.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(shift_up.stroke.code, std::string{"ArrowUp"});
    ASSERT_TRUE(shift_up.stroke.shift);
    ASSERT_FALSE(shift_up.stroke.alt);
    ASSERT_FALSE(shift_up.stroke.control);

    // Ctrl+ArrowRight: modifier 5 -> bitmask 4 = Ctrl.
    auto ctrl_right = ssg::app::decode_input("\x1b[1;5C", true, consumed);
    ASSERT_EQ(ctrl_right.stroke.code, std::string{"ArrowRight"});
    ASSERT_TRUE(ctrl_right.stroke.control);
    ASSERT_FALSE(ctrl_right.stroke.shift);

    // Alt+ArrowLeft: modifier 3 -> bitmask 2 = Alt.
    auto alt_left = ssg::app::decode_input("\x1b[1;3D", true, consumed);
    ASSERT_EQ(alt_left.stroke.code, std::string{"ArrowLeft"});
    ASSERT_TRUE(alt_left.stroke.alt);

    // Ctrl+Shift+ArrowDown: modifier 6 -> bitmask 5 = Shift|Ctrl.
    auto cs_down = ssg::app::decode_input("\x1b[1;6B", true, consumed);
    ASSERT_EQ(cs_down.stroke.code, std::string{"ArrowDown"});
    ASSERT_TRUE(cs_down.stroke.shift);
    ASSERT_TRUE(cs_down.stroke.control);
    ASSERT_FALSE(cs_down.stroke.alt);

    // Shift+Home / Shift+End.
    auto shift_home = ssg::app::decode_input("\x1b[1;2H", true, consumed);
    ASSERT_EQ(shift_home.stroke.code, std::string{"Home"});
    ASSERT_TRUE(shift_home.stroke.shift);
    auto shift_end = ssg::app::decode_input("\x1b[1;2F", true, consumed);
    ASSERT_EQ(shift_end.stroke.code, std::string{"End"});
    ASSERT_TRUE(shift_end.stroke.shift);

    // Plain arrow still decodes unmodified.
    auto plain = ssg::app::decode_input("\x1b[A", true, consumed);
    ASSERT_EQ(plain.stroke.code, std::string{"ArrowUp"});
    ASSERT_FALSE(plain.stroke.shift);

    // An unsupported modifier (m=9 -> bitmask 8, a Meta bit) falls back to the
    // plain, unmodified arrow, consuming the whole sequence.
    auto meta = ssg::app::decode_input("\x1b[1;9A", true, consumed);
    ASSERT_EQ(consumed, std::size_t{6});
    ASSERT_EQ(meta.stroke.code, std::string{"ArrowUp"});
    ASSERT_FALSE(meta.stroke.shift);
    ASSERT_FALSE(meta.stroke.alt);
    ASSERT_FALSE(meta.stroke.control);

    // Ctrl+Alt+ArrowRight: modifier 7 -> bitmask 6 = Alt|Ctrl.
    auto ctrl_alt = ssg::app::decode_input("\x1b[1;7C", true, consumed);
    ASSERT_EQ(ctrl_alt.stroke.code, std::string{"ArrowRight"});
    ASSERT_TRUE(ctrl_alt.stroke.alt);
    ASSERT_TRUE(ctrl_alt.stroke.control);
    ASSERT_FALSE(ctrl_alt.stroke.shift);

    // A multi-digit modifier parses; m=16 -> bitmask 15 includes the unsupported
    // Meta bit, so it falls back to the plain arrow (consuming all 7 bytes).
    auto multi = ssg::app::decode_input("\x1b[1;16C", true, consumed);
    ASSERT_EQ(consumed, std::size_t{7});
    ASSERT_EQ(multi.stroke.code, std::string{"ArrowRight"});
    ASSERT_FALSE(multi.stroke.alt);
    ASSERT_FALSE(multi.stroke.control);
    ASSERT_FALSE(multi.stroke.shift);
}

TEST(decode_input_modified_arrow_split_reads_are_incomplete) {
    std::size_t consumed = 0;
    // Every partial-parameter prefix is incomplete and consumes nothing until the
    // final letter arrives.
    for (auto const* partial : {"\x1b[1", "\x1b[1;", "\x1b[1;2", "\x1b[1;16"}) {
        consumed = 99;
        auto decoded = ssg::app::decode_input(partial, true, consumed);
        ASSERT_TRUE(decoded.status == ssg::app::DecodeStatus::incomplete);
        ASSERT_EQ(consumed, std::size_t{0});
    }
    // The completing bytes finish the sequence.
    auto done = ssg::app::decode_input("\x1b[1;16D", true, consumed);
    ASSERT_EQ(done.stroke.code, std::string{"ArrowLeft"});
    ASSERT_EQ(consumed, std::size_t{7});

    // A pathologically long modifier parameter must not overflow the decimal
    // accumulator; it saturates, falls back to the plain arrow, and consumes the
    // whole sequence.
    std::string huge = "\x1b[1;";
    huge.append(40, '9');
    huge += "A";
    auto overflow = ssg::app::decode_input(huge, true, consumed);
    ASSERT_EQ(consumed, huge.size());
    ASSERT_EQ(overflow.stroke.code, std::string{"ArrowUp"});
    ASSERT_FALSE(overflow.stroke.shift);
    ASSERT_FALSE(overflow.stroke.alt);
    ASSERT_FALSE(overflow.stroke.control);
}

TEST(decode_input_arrows_and_mouse) {
    std::size_t consumed = 0;
    auto up = ssg::app::decode_input("\x1b[A", true, consumed);
    ASSERT_EQ(consumed, std::size_t{3});
    ASSERT_EQ(up.stroke.code, std::string{"ArrowUp"});
    auto down = ssg::app::decode_input("\x1b[B", true, consumed);
    ASSERT_EQ(down.stroke.code, std::string{"ArrowDown"});

    auto wheel = ssg::app::decode_input("\x1b[<65;10;5M", true, consumed);
    ASSERT_TRUE(wheel.status == ssg::app::DecodeStatus::scroll);
    ASSERT_EQ(wheel.scroll, std::int64_t{3});
    // The wheel carries its 0-based grid position (SGR 1-based 10,5 -> 9,4) so
    // the app can route it to the region under the pointer.
    ASSERT_EQ(wheel.pointer.column, 9);
    ASSERT_EQ(wheel.pointer.row, 4);
}

TEST(decode_input_escape_boundary_is_bounded) {
    std::size_t consumed = 0;
    // A buffered CSI introducer disambiguates to an arrow, not an Escape stroke.
    auto arrow = ssg::app::decode_input("\x1b[A", false, consumed);
    ASSERT_EQ(arrow.stroke.code, std::string{"ArrowUp"});

    // ESC followed by a non-CSI byte: ESC is a standalone Escape stroke consuming
    // only itself, so the next byte (the chord continuation) decodes separately.
    auto esc_then = ssg::app::decode_input("\x1bs", false, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_EQ(esc_then.stroke.code, std::string{"Escape"});

    // A lone ESC with more input possibly coming: incomplete, consume nothing.
    auto pending = ssg::app::decode_input("\x1b", false, consumed);
    ASSERT_TRUE(pending.status == ssg::app::DecodeStatus::incomplete);
    ASSERT_EQ(consumed, std::size_t{0});

    // A lone ESC with input exhausted (bounded read returned nothing): the
    // Escape stroke.
    auto exhausted = ssg::app::decode_input("\x1b", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_EQ(exhausted.stroke.code, std::string{"Escape"});

    // A truncated CSI is always incomplete regardless of exhaustion (the final
    // byte has not arrived).
    auto partial = ssg::app::decode_input("\x1b[", true, consumed);
    ASSERT_TRUE(partial.status == ssg::app::DecodeStatus::incomplete);
}

TEST(decode_input_pointer_press_release_drag) {
    std::size_t consumed = 0;

    // Left press at SGR (1,1) -> grid (0,0). Button bits 0, final 'M'.
    auto press = ssg::app::decode_input("\x1b[<0;1;1M", true, consumed);
    ASSERT_TRUE(press.status == ssg::app::DecodeStatus::pointer);
    ASSERT_EQ(consumed, std::size_t{9});
    ASSERT_EQ(press.pointer.column, 0);
    ASSERT_EQ(press.pointer.row, 0);
    ASSERT_TRUE(press.pointer.button == ssg::app::PointerButton::left);
    ASSERT_TRUE(press.pointer.kind == ssg::app::PointerKind::press);

    // Left release (final 'm') at (10,5) -> grid (9,4).
    auto release = ssg::app::decode_input("\x1b[<0;10;5m", true, consumed);
    ASSERT_TRUE(release.status == ssg::app::DecodeStatus::pointer);
    ASSERT_EQ(release.pointer.column, 9);
    ASSERT_EQ(release.pointer.row, 4);
    ASSERT_TRUE(release.pointer.button == ssg::app::PointerButton::left);
    ASSERT_TRUE(release.pointer.kind == ssg::app::PointerKind::release);

    // Left drag: motion bit 32 set (Cb 32), final 'M', at (3,7) -> grid (2,6).
    auto drag = ssg::app::decode_input("\x1b[<32;3;7M", true, consumed);
    ASSERT_TRUE(drag.status == ssg::app::DecodeStatus::pointer);
    ASSERT_EQ(drag.pointer.column, 2);
    ASSERT_EQ(drag.pointer.row, 6);
    ASSERT_TRUE(drag.pointer.button == ssg::app::PointerButton::left);
    ASSERT_TRUE(drag.pointer.kind == ssg::app::PointerKind::drag);

    // Middle press (button bits 1) and right press (button bits 2).
    auto middle = ssg::app::decode_input("\x1b[<1;2;2M", true, consumed);
    ASSERT_TRUE(middle.pointer.button == ssg::app::PointerButton::middle);
    ASSERT_TRUE(middle.pointer.kind == ssg::app::PointerKind::press);
    auto right = ssg::app::decode_input("\x1b[<2;2;2M", true, consumed);
    ASSERT_TRUE(right.pointer.button == ssg::app::PointerButton::right);

    // The wheel stays a scroll event, not a pointer event.
    auto wheel_up = ssg::app::decode_input("\x1b[<64;10;5M", true, consumed);
    ASSERT_TRUE(wheel_up.status == ssg::app::DecodeStatus::scroll);
    ASSERT_EQ(wheel_up.scroll, std::int64_t{-3});
    ASSERT_EQ(wheel_up.pointer.column, 9);
    ASSERT_EQ(wheel_up.pointer.row, 4);
}

TEST(decode_input_pointer_split_reads_are_incomplete) {
    std::size_t consumed = 0;
    // Every truncation before the final M/m byte is incomplete and consumes
    // nothing, so the loop waits for more bytes.
    for (std::string_view partial :
         {"\x1b[<", "\x1b[<0", "\x1b[<0;", "\x1b[<0;10", "\x1b[<0;10;",
          "\x1b[<0;10;5"}) {
        consumed = 0;
        auto decoded = ssg::app::decode_input(partial, true, consumed);
        ASSERT_TRUE(decoded.status == ssg::app::DecodeStatus::incomplete);
        ASSERT_EQ(consumed, std::size_t{0});
    }
    // The complete sequence then decodes.
    auto complete = ssg::app::decode_input("\x1b[<0;10;5M", true, consumed);
    ASSERT_TRUE(complete.status == ssg::app::DecodeStatus::pointer);
}

TEST(route_pointer_left_press_on_editor_places_caret) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::editor;
    hit.byte_offset = 3;
    ssg::app::PointerTargets targets;
    targets.document_position =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};

    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::press, false,
                                        std::nullopt, targets);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"cursor.set_position"});
        auto const* args = std::any_cast<ssg::SelectionCommandArguments>(
            &plan.commands[0].payload);
        ASSERT_TRUE(args != nullptr);
        if (args) {
            ASSERT_TRUE(args->position.has_value());
            if (args->position) ASSERT_EQ(*args->position, *targets.document_position);
            ASSERT_FALSE(args->selection.has_value());
        }
    }
    // The press begins a potential selection drag.
    ASSERT_TRUE(plan.begins_drag);
    ASSERT_FALSE(plan.ends_drag);
}

TEST(route_pointer_ignores_non_editor_and_non_left) {
    ssg::app::PointerTargets const empty;

    // A press on nothing (out of bounds / chrome) dispatches no command.
    ssg::RegionHit none_hit;  // region defaults to HitRegion::none
    auto none_plan = ssg::app::route_pointer(
        none_hit, ssg::app::PointerButton::left, ssg::app::PointerKind::press,
        false, std::nullopt, empty);
    ASSERT_TRUE(none_plan.commands.empty());
    ASSERT_FALSE(none_plan.begins_drag);

    // A left press on a non-editor region (e.g. the panel) is not handled by
    // M8-C: no editor command, no drag.
    ssg::RegionHit panel_hit;
    panel_hit.region = ssg::HitRegion::panel;
    auto panel_plan = ssg::app::route_pointer(
        panel_hit, ssg::app::PointerButton::left, ssg::app::PointerKind::press,
        false, std::nullopt, empty);
    ASSERT_TRUE(panel_plan.commands.empty());

    // A right/middle press on the editor is a no-op in M8.
    ssg::RegionHit editor_hit;
    editor_hit.region = ssg::HitRegion::editor;
    ssg::app::PointerTargets targets;
    targets.document_position =
        ssg::DocumentPosition{ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
    auto right_plan = ssg::app::route_pointer(
        editor_hit, ssg::app::PointerButton::right, ssg::app::PointerKind::press,
        false, std::nullopt, targets);
    ASSERT_TRUE(right_plan.commands.empty());

    // A left press on the editor with no resolved position (e.g. a blank cell)
    // dispatches nothing.
    auto unresolved = ssg::app::route_pointer(
        editor_hit, ssg::app::PointerButton::left, ssg::app::PointerKind::press,
        false, std::nullopt, empty);
    ASSERT_TRUE(unresolved.commands.empty());
    ASSERT_FALSE(unresolved.begins_drag);
}

TEST(decode_input_pointer_rejects_malformed_but_terminated_payloads) {
    std::size_t consumed = 0;
    // Empty Cb, empty Cx, empty Cy, and non-digit Cy each terminate with M/m but
    // are malformed: they are consumed and dropped (none), never dispatched as a
    // real pointer event at (0,0).
    for (std::string_view malformed :
         {"\x1b[<;1;1M", "\x1b[<0;;1M", "\x1b[<0;1;M", "\x1b[<0;1;xM",
          "\x1b[<0;1M", "\x1b[<0;1;5;9M"}) {
        consumed = 0;
        auto decoded = ssg::app::decode_input(malformed, true, consumed);
        ASSERT_TRUE(decoded.status == ssg::app::DecodeStatus::none);
        ASSERT_EQ(consumed, malformed.size());  // consumed so the loop advances
    }
}

TEST(route_pointer_drag_extends_selection_from_anchor) {
    auto const anchor =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::editor;
    hit.byte_offset = 10;
    ssg::app::PointerTargets targets;
    auto const active =
        ssg::DocumentPosition{ssg::ByteOffset{10}, ssg::LineIndex{1}, ssg::CellIndex{2}};
    targets.document_position = active;

    // A drag while dragging with an anchor -> select.set_range spanning the two.
    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::drag, true,
                                        anchor, targets);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"select.set_range"});
        auto const* args = std::any_cast<ssg::SelectionCommandArguments>(
            &plan.commands[0].payload);
        ASSERT_TRUE(args != nullptr);
        if (args) {
            ASSERT_FALSE(args->position.has_value());
            ASSERT_TRUE(args->selection.has_value());
            if (args->selection) {
                ASSERT_EQ(args->selection->anchor, anchor);
                ASSERT_EQ(args->selection->active, active);
            }
        }
    }
    ASSERT_FALSE(plan.begins_drag);
    ASSERT_FALSE(plan.ends_drag);
}

TEST(route_pointer_drag_without_anchor_or_target_is_a_no_op) {
    auto const anchor =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    ssg::RegionHit editor_hit;
    editor_hit.region = ssg::HitRegion::editor;
    ssg::app::PointerTargets targets;
    targets.document_position =
        ssg::DocumentPosition{ssg::ByteOffset{5}, ssg::LineIndex{0}, ssg::CellIndex{5}};

    // Not dragging (no prior press) -> no command even over the editor.
    auto not_dragging = ssg::app::route_pointer(
        editor_hit, ssg::app::PointerButton::left, ssg::app::PointerKind::drag,
        false, anchor, targets);
    ASSERT_TRUE(not_dragging.commands.empty());

    // Dragging but the pointer is over a cell with no document target (past a
    // short line's end / beyond the viewport edge) -> no command, selection holds.
    ssg::app::PointerTargets const no_target;
    auto off_content = ssg::app::route_pointer(
        editor_hit, ssg::app::PointerButton::left, ssg::app::PointerKind::drag,
        true, anchor, no_target);
    ASSERT_TRUE(off_content.commands.empty());

    // Dragging over a non-editor region (e.g. the panel) -> no command.
    ssg::RegionHit panel_hit;
    panel_hit.region = ssg::HitRegion::panel;
    auto off_editor = ssg::app::route_pointer(
        panel_hit, ssg::app::PointerButton::left, ssg::app::PointerKind::drag,
        true, anchor, targets);
    ASSERT_TRUE(off_editor.commands.empty());
}

TEST(route_pointer_release_ends_drag_without_a_command) {
    ssg::RegionHit editor_hit;
    editor_hit.region = ssg::HitRegion::editor;
    ssg::app::PointerTargets targets;
    targets.document_position =
        ssg::DocumentPosition{ssg::ByteOffset{5}, ssg::LineIndex{0}, ssg::CellIndex{5}};

    // Release while dragging ends the drag and dispatches nothing.
    auto ending = ssg::app::route_pointer(
        editor_hit, ssg::app::PointerButton::left, ssg::app::PointerKind::release,
        true, targets.document_position, targets);
    ASSERT_TRUE(ending.commands.empty());
    ASSERT_TRUE(ending.ends_drag);

    // A release when not dragging is inert.
    auto stray = ssg::app::route_pointer(
        editor_hit, ssg::app::PointerButton::left, ssg::app::PointerKind::release,
        false, std::nullopt, targets);
    ASSERT_TRUE(stray.commands.empty());
    ASSERT_FALSE(stray.ends_drag);
}

TEST(route_pointer_editor_scrollbar_scrolls_to_fraction) {
    // A press or drag on the editor gutter scrolls to the fraction hit_test
    // reported, independent of the selection drag state. The bottom of the
    // gutter reports numerator == denominator (-> maximum_first_row); the top
    // reports numerator 0 (-> first_row 0).
    auto scroll_args = [](ssg::app::PointerDispatch const& plan)
        -> ssg::ScrollFractionArguments const* {
        if (plan.commands.size() != 1) return nullptr;
        if (plan.commands[0].command_id != "view.scroll_to_fraction") return nullptr;
        return std::any_cast<ssg::ScrollFractionArguments>(&plan.commands[0].payload);
    };

    ssg::RegionHit bottom;
    bottom.region = ssg::HitRegion::editor_scrollbar;
    bottom.scroll_numerator = 7;
    bottom.scroll_denominator = 7;
    ssg::app::PointerTargets const empty;

    for (auto kind : {ssg::app::PointerKind::press, ssg::app::PointerKind::drag}) {
        auto plan = ssg::app::route_pointer(
            bottom, ssg::app::PointerButton::left, kind, false, std::nullopt, empty);
        auto const* args = scroll_args(plan);
        ASSERT_TRUE(args != nullptr);
        if (args) {
            ASSERT_EQ(args->numerator, std::uint32_t{7});
            ASSERT_EQ(args->denominator, std::uint32_t{7});
        }
        ASSERT_FALSE(plan.begins_drag);
        ASSERT_FALSE(plan.ends_drag);
    }

    ssg::RegionHit top;
    top.region = ssg::HitRegion::editor_scrollbar;
    top.scroll_numerator = 0;
    top.scroll_denominator = 7;
    auto top_plan = ssg::app::route_pointer(
        top, ssg::app::PointerButton::left, ssg::app::PointerKind::press, false,
        std::nullopt, empty);
    auto const* top_args = scroll_args(top_plan);
    ASSERT_TRUE(top_args != nullptr);
    if (top_args) {
        ASSERT_EQ(top_args->numerator, std::uint32_t{0});
        ASSERT_EQ(top_args->denominator, std::uint32_t{7});
    }

    // A mid-drag onto the editor gutter scrolls even while a selection drag is
    // active; the scrollbar path does not consult the drag anchor.
    auto const anchor =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    auto mid = ssg::app::route_pointer(bottom, ssg::app::PointerButton::left,
                                       ssg::app::PointerKind::drag, true, anchor,
                                       empty);
    ASSERT_TRUE(scroll_args(mid) != nullptr);
}

TEST(route_pointer_panel_and_palette_scrollbars_are_no_ops) {
    // Panel/palette gutter drag is not wired in M8; those hits dispatch nothing.
    ssg::app::PointerTargets const empty;
    for (auto region : {ssg::HitRegion::panel_scrollbar,
                        ssg::HitRegion::palette_scrollbar}) {
        ssg::RegionHit hit;
        hit.region = region;
        hit.scroll_numerator = 3;
        hit.scroll_denominator = 5;
        for (auto kind : {ssg::app::PointerKind::press, ssg::app::PointerKind::drag}) {
            auto plan = ssg::app::route_pointer(
                hit, ssg::app::PointerButton::left, kind, false, std::nullopt, empty);
            ASSERT_TRUE(plan.commands.empty());
        }
    }
}

TEST(route_pointer_tab_press_activates_the_tab) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::tab;
    hit.tab_index = 2;
    ssg::app::PointerTargets targets;
    targets.tab_id = ssg::TabId{7};

    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::press, false,
                                        std::nullopt, targets);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"tab.activate"});
        auto const* id = std::any_cast<ssg::TabId>(&plan.commands[0].payload);
        ASSERT_TRUE(id != nullptr);
        if (id) ASSERT_TRUE(*id == ssg::TabId{7});
    }
    ASSERT_FALSE(plan.begins_drag);
    ASSERT_FALSE(plan.ends_drag);

    // A tab hit the caller could not resolve to a TabId dispatches nothing.
    ssg::app::PointerTargets const empty;
    auto unresolved = ssg::app::route_pointer(
        hit, ssg::app::PointerButton::left, ssg::app::PointerKind::press, false,
        std::nullopt, empty);
    ASSERT_TRUE(unresolved.commands.empty());
}

TEST(route_pointer_palette_press_executes_the_candidate) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::palette;
    hit.item_index = 4;
    ssg::app::PointerTargets targets;
    targets.palette_command_id = std::string{"view.split"};

    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::press, false,
                                        std::nullopt, targets);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"palette.execute"});
        auto const* args = std::any_cast<ssg::PaletteExecuteArguments>(
            &plan.commands[0].payload);
        ASSERT_TRUE(args != nullptr);
        if (args) ASSERT_EQ(args->command_id, std::string{"view.split"});
    }
    ASSERT_FALSE(plan.begins_drag);

    // A palette hit with no resolved candidate id dispatches nothing.
    ssg::app::PointerTargets const empty;
    auto unresolved = ssg::app::route_pointer(
        hit, ssg::app::PointerButton::left, ssg::app::PointerKind::press, false,
        std::nullopt, empty);
    ASSERT_TRUE(unresolved.commands.empty());
}

TEST(route_pointer_panel_press_selects_and_activates_the_node) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::panel;
    hit.node_id = ssg::TreeNodeId{"files:src/main.cpp"};
    ssg::app::PointerTargets const empty;

    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::press, false,
                                        std::nullopt, empty);
    // A tree-row click selects the node, then activates it (matching keyboard
    // select-then-Enter), in that order.
    ASSERT_EQ(plan.commands.size(), std::size_t{2});
    if (plan.commands.size() == 2) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"tree.select"});
        auto const* args = std::any_cast<ssg::TreeSelectArguments>(
            &plan.commands[0].payload);
        ASSERT_TRUE(args != nullptr);
        if (args) ASSERT_EQ(args->node_id, *hit.node_id);
        ASSERT_EQ(plan.commands[1].command_id, std::string{"tree.activate"});
    }
    ASSERT_FALSE(plan.begins_drag);
    ASSERT_FALSE(plan.ends_drag);

    // A panel hit with no node id (the provider-label row / empty area) is inert.
    ssg::RegionHit no_node;
    no_node.region = ssg::HitRegion::panel;
    auto inert = ssg::app::route_pointer(
        no_node, ssg::app::PointerButton::left, ssg::app::PointerKind::press,
        false, std::nullopt, empty);
    ASSERT_TRUE(inert.commands.empty());
}

TEST(route_wheel_maps_region_to_scroll_target) {
    using ssg::app::WheelTarget;
    // The side panel and its gutter scroll the tree.
    ASSERT_TRUE(ssg::app::route_wheel(ssg::HitRegion::panel) == WheelTarget::tree);
    ASSERT_TRUE(ssg::app::route_wheel(ssg::HitRegion::panel_scrollbar) ==
                WheelTarget::tree);
    // The palette and its gutter scroll the client-owned palette window.
    ASSERT_TRUE(ssg::app::route_wheel(ssg::HitRegion::palette) ==
                WheelTarget::palette);
    ASSERT_TRUE(ssg::app::route_wheel(ssg::HitRegion::palette_scrollbar) ==
                WheelTarget::palette);
    // The editor, its gutter, a tab, and no region all scroll the document.
    for (auto region : {ssg::HitRegion::editor, ssg::HitRegion::editor_scrollbar,
                        ssg::HitRegion::tab, ssg::HitRegion::none}) {
        ASSERT_TRUE(ssg::app::route_wheel(region) == WheelTarget::editor);
    }
}

int main() {
    RUN(resolve_launch_no_argument_opens_cwd);
    RUN(resolve_launch_directory_opens_that_directory);
    RUN(resolve_launch_file_opens_parent_directory_and_file);
    RUN(encode_ansi_frame_addresses_rows_and_emits_palette_colors);
    RUN(encode_ansi_frame_skips_wide_glyph_continuation);
    RUN(decode_input_maps_printables_and_named_keys);
    RUN(decode_input_modified_arrows);
    RUN(decode_input_modified_arrow_split_reads_are_incomplete);
    RUN(decode_input_arrows_and_mouse);
    RUN(decode_input_pointer_press_release_drag);
    RUN(decode_input_pointer_split_reads_are_incomplete);
    RUN(decode_input_pointer_rejects_malformed_but_terminated_payloads);
    RUN(route_pointer_left_press_on_editor_places_caret);
    RUN(route_pointer_ignores_non_editor_and_non_left);
    RUN(route_pointer_drag_extends_selection_from_anchor);
    RUN(route_pointer_drag_without_anchor_or_target_is_a_no_op);
    RUN(route_pointer_release_ends_drag_without_a_command);
    RUN(route_pointer_editor_scrollbar_scrolls_to_fraction);
    RUN(route_pointer_panel_and_palette_scrollbars_are_no_ops);
    RUN(route_pointer_tab_press_activates_the_tab);
    RUN(route_pointer_palette_press_executes_the_candidate);
    RUN(route_pointer_panel_press_selects_and_activates_the_node);
    RUN(route_wheel_maps_region_to_scroll_target);
    RUN(decode_input_escape_boundary_is_bounded);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
