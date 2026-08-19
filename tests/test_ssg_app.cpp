#include "pointer_routing.h"
#include "ssg_terminal.h"

#include <ssg/EditorRuntime.h>
#include <ssg/HitTester.h>
#include <ssg/Renderer.h>
#include <ssg/ScriptHost.h>
#include <ssg/Selection.h>

#include "init_script.h"

#include "test_helpers.h"

#include <algorithm>
#include <any>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

std::string readSource(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

std::optional<fs::path> locateFromParents(
    fs::path start, const fs::path& relative) {
    for (int depth = 0; depth < 8; ++depth) {
        auto candidate = start / relative;
        if (fs::exists(candidate)) {
            return candidate;
        }
        if (!start.has_parent_path()) break;
        start = start.parent_path();
    }
    return std::nullopt;
}

TEST(resolveLaunchNoArgumentOpensCwd) {
    auto target = ssg::app::resolve_launch({});
    ASSERT_EQ(target.cwd, fs::current_path());
    ASSERT_FALSE(target.file.has_value());
}

TEST(resolveLaunchDirectoryOpensThatDirectory) {
    auto dir = fs::temp_directory_path() / "ssg-app-dir-case";
    fs::create_directories(dir);
    auto target = ssg::app::resolve_launch(dir);
    ASSERT_EQ(target.cwd, fs::absolute(dir));
    ASSERT_FALSE(target.file.has_value());
}

TEST(resolveLaunchFileOpensParentDirectoryAndFile) {
    auto dir = fs::temp_directory_path() / "ssg-app-file-case";
    fs::create_directories(dir);
    auto file = dir / "hello.txt";
    std::ofstream{file} << "hi";
    auto target = ssg::app::resolve_launch(file);
    ASSERT_EQ(target.cwd, fs::absolute(dir));
    ASSERT_TRUE(target.file.has_value());
    ASSERT_EQ(*target.file, std::string{"hello.txt"});
}

TEST(encodeAnsiFrameAdaptsToColorDepth) {
    ssg::CellGrid screen;
    screen.size = {1, 1};
    // Foreground pure red (theme index 1), background pure black (index 0).
    screen.colors[0] = {0, 0, 0};
    screen.colors[1] = {255, 0, 0};
    ssg::CellGridCell cell;
    cell.text = "X";
    cell.foreground = 1;
    cell.background = 0;
    screen.cells = {cell};

    // Truecolor: exact channels.
    auto truecolor = ssg::app::encode_ansi_frame(screen, ssg::ColorDepth::Truecolor);
    ASSERT_TRUE(truecolor.find("\x1b[38;2;255;0;0m") != std::string::npos);
    ASSERT_TRUE(truecolor.find("\x1b[48;2;0;0;0m") != std::string::npos);

    // Indexed256: pure red is xterm cube index 196; black is index 16.
    auto indexed = ssg::app::encode_ansi_frame(screen, ssg::ColorDepth::Indexed256);
    ASSERT_TRUE(indexed.find("\x1b[38;5;196m") != std::string::npos);
    ASSERT_TRUE(indexed.find("\x1b[48;5;16m") != std::string::npos);
    ASSERT_TRUE(indexed.find(";2;") == std::string::npos);  // no truecolor bytes

    // ANSI16: pure red is base index 9 (bright red) -> fg SGR 91; black is index
    // 0 -> bg SGR 40.
    auto ansi = ssg::app::encode_ansi_frame(screen, ssg::ColorDepth::Ansi16);
    ASSERT_TRUE(ansi.find("\x1b[91m") != std::string::npos);
    ASSERT_TRUE(ansi.find("\x1b[40m") != std::string::npos);
    ASSERT_TRUE(ansi.find(";5;") == std::string::npos);
    ASSERT_TRUE(ansi.find(";2;") == std::string::npos);
}

TEST(statusActionPointerClickRoutesToInvokeActionWithGeneration) {
    ssg::StatusActionInvocation invocation{ssg::StatusId{11}, "apply", 13};
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::StatusAction;
    hit.statusInvocation = invocation;
    ssg::app::PointerTargets targets;
    targets.status_invocation = invocation;
    auto plan = ssg::app::route_pointer(
        hit, ssg::app::PointerButton::left, ssg::app::PointerKind::press,
        false, false, std::nullopt, targets, {});
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    ASSERT_EQ(plan.commands[0].command_id, std::string{"status.invoke_action"});
    ASSERT_EQ(std::any_cast<ssg::StatusActionInvocation>(plan.commands[0].payload),
              invocation);
}


TEST(aClickOnAnExternalActionRoutesThroughPointerTargetsToSelectThenAct) {
    // A click on an external-modification action first selects the runtime-minted
    // file (external.select), then runs the payload-less action on the library-
    // owned selection -- select-then-act, the one behavior path, with live (not
    // dead) hit regions.
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::ExternalAction;
    hit.externalFileId = "external:src/foo:bar.cpp";  // an id that contains ':'
    hit.commandId = "external.reload";
    ssg::app::PointerTargets targets;
    targets.external_file_id = ssg::DiffFileId{*hit.externalFileId};
    targets.external_action_command = hit.commandId;
    auto plan = ssg::app::route_pointer(
        hit, ssg::app::PointerButton::left, ssg::app::PointerKind::press,
        false, false, std::nullopt, targets, {});
    ASSERT_EQ(plan.commands.size(), std::size_t{2});
    ASSERT_EQ(plan.commands[0].command_id, std::string{"external.select"});
    ASSERT_EQ(std::any_cast<ssg::DiffFileId>(plan.commands[0].payload),
              ssg::DiffFileId{"external:src/foo:bar.cpp"});
    ASSERT_EQ(plan.commands[1].command_id, std::string{"external.reload"});
    // The action is gated on the select: a rejected external.select of a stale id
    // must not let the action run against the previously selected file.
    ASSERT_TRUE(plan.commands[1].gate_on_previous);
    ASSERT_TRUE(!plan.commands[0].gate_on_previous);
    ASSERT_TRUE(!plan.begins_drag);
}


TEST(exmdParsesActionAndIdWithoutSplittingTheIdOnColon) {
    // The id is opaque after the first TAB: "external:"+path, colons and all.
    auto const frame = ssg::app::parse_external_pointer_frame(
        "EXMD:keep_buffer\texternal:src/a:b:c.cpp");
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(frame->action == ssg::ExternalAction::KeepBuffer);
    ASSERT_EQ(frame->command, std::string{"external.keep_buffer"});
    ASSERT_EQ(frame->id.value(), std::string{"external:src/a:b:c.cpp"});

    // Malformed frames yield nothing: no TAB, empty id, unknown action token.
    ASSERT_TRUE(!ssg::app::parse_external_pointer_frame("EXMD:reload").has_value());
    ASSERT_TRUE(!ssg::app::parse_external_pointer_frame("EXMD:reload\t").has_value());
    ASSERT_TRUE(!ssg::app::parse_external_pointer_frame("EXMD:nope\texternal:x")
                     .has_value());
    ASSERT_TRUE(!ssg::app::parse_external_pointer_frame("SNAP").has_value());
}

TEST(exmdOnlyDispatchesForAPublishedIdAndOfferedAction) {
    ssg::ExternalDocumentView file{
        ssg::DiffFileId{"external:src/a:b.cpp"}, {},
        ssg::ExternalDocumentStatus::ExternallyModified, {},
        {ssg::ExternalAction::Reload, ssg::ExternalAction::OpenDiff}};
    std::vector<ssg::ExternalDocumentView> files{file};

    auto const reload = ssg::app::parse_external_pointer_frame(
        "EXMD:reload\texternal:src/a:b.cpp");
    ASSERT_TRUE(reload.has_value());
    ASSERT_TRUE(ssg::app::external_pointer_frame_is_offered(*reload, files));

    // An action the file does not offer is a no-op.
    auto const keep = ssg::app::parse_external_pointer_frame(
        "EXMD:keep_buffer\texternal:src/a:b.cpp");
    ASSERT_TRUE(keep.has_value());
    ASSERT_TRUE(!ssg::app::external_pointer_frame_is_offered(*keep, files));

    // An unpublished id is a no-op.
    auto const unknown = ssg::app::parse_external_pointer_frame(
        "EXMD:reload\texternal:src/other.cpp");
    ASSERT_TRUE(unknown.has_value());
    ASSERT_TRUE(!ssg::app::external_pointer_frame_is_offered(*unknown, files));
}


TEST(encodeAnsiFrameEmitsOrthogonalTintBackgrounds) {
    ssg::CellGrid screen;
    screen.size = {2, 1};
    screen.colors[0] = {30, 30, 30};
    screen.colors[1] = {212, 212, 212};
    screen.diffTints.addedRow = {0, 0, 95};
    ssg::CellGridCell plain;
    plain.text = "X";
    plain.foreground = 1;
    ssg::CellGridCell tinted = plain;
    tinted.text = "Y";
    tinted.tint = ssg::DiffTint::AddedRow;
    screen.cells = {plain, tinted};

    const auto truecolor =
        ssg::app::encode_ansi_frame(screen, ssg::ColorDepth::Truecolor);
    ASSERT_TRUE(truecolor.find("\x1b[48;2;30;30;30m") != std::string::npos);
    ASSERT_TRUE(truecolor.find("\x1b[48;2;0;0;95m") != std::string::npos);
    const auto indexed =
        ssg::app::encode_ansi_frame(screen, ssg::ColorDepth::Indexed256);
    const auto tintIndex =
        ssg::ColorResolver{ssg::ColorDepth::Indexed256}.resolve(screen.diffTints.addedRow)
            .index;
    ASSERT_TRUE(indexed.find("\x1b[48;5;" + std::to_string(tintIndex) + "m") !=
                std::string::npos);
}

TEST(detectColorDepthReadsEnvironment) {
    using ssg::ColorDepth;
    ASSERT_TRUE(ssg::app::detect_color_depth("truecolor", nullptr, "dumb", nullptr) ==
                ColorDepth::Truecolor);
    ASSERT_TRUE(ssg::app::detect_color_depth("24BIT", nullptr, "dumb", nullptr) ==
                ColorDepth::Truecolor);
    ASSERT_TRUE(ssg::app::detect_color_depth("256", nullptr, "xterm", nullptr) ==
                ColorDepth::Indexed256);
    ASSERT_TRUE(ssg::app::detect_color_depth("indexed256", nullptr, "xterm", nullptr) ==
                ColorDepth::Indexed256);
    ASSERT_TRUE(ssg::app::detect_color_depth("ansi16", nullptr, "xterm", nullptr) ==
                ColorDepth::Ansi16);
    ASSERT_TRUE(ssg::app::detect_color_depth("bogus", "truecolor", "dumb", nullptr) ==
                ColorDepth::Truecolor);

    ASSERT_TRUE(ssg::app::detect_color_depth(nullptr, "truecolor", "xterm-256color",
                                             nullptr) == ColorDepth::Truecolor);
    ASSERT_TRUE(ssg::app::detect_color_depth(nullptr, "24bit", "xterm", nullptr) ==
                ColorDepth::Truecolor);

    ASSERT_TRUE(ssg::app::detect_color_depth(nullptr, nullptr, "dumb", "wezterm") ==
                ColorDepth::Ansi16);
    ASSERT_TRUE(ssg::app::detect_color_depth(nullptr, nullptr, "", "wezterm") ==
                ColorDepth::Ansi16);
    ASSERT_TRUE(ssg::app::detect_color_depth(nullptr, nullptr, nullptr, "wezterm") ==
                ColorDepth::Ansi16);

    ASSERT_TRUE(ssg::app::detect_color_depth(nullptr, nullptr, "screen",
                                             "iTerm.app") == ColorDepth::Truecolor);
    ASSERT_TRUE(ssg::app::detect_color_depth(nullptr, nullptr, "xterm-kitty",
                                             nullptr) == ColorDepth::Truecolor);

    ASSERT_TRUE(ssg::app::detect_color_depth(nullptr, nullptr, "vt100", nullptr) ==
                ColorDepth::Truecolor);
    ASSERT_TRUE(ssg::app::detect_color_depth(nullptr, "", "xterm-256color", nullptr) ==
                ColorDepth::Truecolor);
}

TEST(encodeAnsiFrameAddressesRowsAndEmitsPaletteColors) {
    ssg::CellGrid screen;
    screen.size = {2, 1};
    screen.colors[0] = {10, 20, 30};
    screen.colors[1] = {200, 100, 50};
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

TEST(encodeAnsiFrameSkipsWideGlyphContinuation) {
    ssg::CellGrid screen;
    screen.size = {2, 1};
    screen.colors[0] = {0, 0, 0};
    ssg::CellGridCell wide;
    wide.text = "\xe4\xb8\xad";  // U+4E2D, a double-width glyph.
    ssg::CellGridCell continuation;
    continuation.continuation = true;
    continuation.text = " ";
    screen.cells = {wide, continuation};

    auto frame = ssg::app::encode_ansi_frame(screen);
    ASSERT_TRUE(frame.find("\xe4\xb8\xad") != std::string::npos);
    // The wide glyph occupies both columns; the continuation adds no glyph.
    auto rowStart = frame.find("\x1b[1;1H");
    ASSERT_TRUE(rowStart != std::string::npos);
    auto glyph = frame.find("\xe4\xb8\xad", rowStart);
    ASSERT_TRUE(frame.find(' ', glyph + 3) == std::string::npos ||
                frame.find("\x1b[0m", glyph) < frame.find(' ', glyph + 3));
}

TEST(unicodeEndToEndGridAndEncoding) {
    // text -> snapshot -> render -> encode, locking the client Unicode path:
    // a wide CJG glyph occupies a cell + continuation, a combining mark folds
    // into its base grapheme (width 1), a ZWJ emoji sequence is one wide cluster,
    // the caret advances by 2 past a wide glyph, and the encoder emits one glyph
    // per cluster and nothing for a continuation cell.
    auto root = fs::temp_directory_path() / "ssg-m9u";
    fs::remove_all(root);
    fs::create_directories(root / "workspace");
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    // a b [U+4E00 wide] e [U+0301 combining] [U+1F468 ZWJ U+1F469]
    const std::string cjk = "\xE4\xB8\x80";                 // U+4E00, wide
    const std::string ecombining = "e\xCC\x81";             // e + U+0301
    const std::string emoji = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9";
    const std::string line = "ab" + cjk + ecombining + emoji;
    std::ofstream{root / "workspace" / "u.txt", std::ios::binary} << line << "\n";

    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    ASSERT_TRUE(runtime.dispatch(ssg::ClientId{1},
                                 {"file.open", runtime.revision(), std::string{"u.txt"}})
                    .accepted());
    auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (!snap.has_value()) return;
    auto grid = ssg::Renderer{}.render(*snap);

    // Locate the content row: the first cell run "a","b".
    int row = -1, startx = -1;
    for (int y = 0; y < grid.size.rows && row < 0; ++y) {
        for (int x = 0; x + 1 < grid.size.columns; ++x) {
            auto const& c0 = grid.cells[static_cast<std::size_t>(y * grid.size.columns + x)];
            auto const& c1 = grid.cells[static_cast<std::size_t>(y * grid.size.columns + x + 1)];
            if (c0.text == "a" && c1.text == "b") { row = y; startx = x; break; }
        }
    }
    ASSERT_TRUE(row >= 0);
    if (row < 0) return;
    auto cell = [&](int k) -> ssg::CellGridCell const& {
        return grid.cells[static_cast<std::size_t>(row * grid.size.columns + startx + k)];
    };

    // Exact cell placement (the golden).
    ASSERT_EQ(cell(0).text, std::string{"a"});
    ASSERT_FALSE(cell(0).continuation);
    ASSERT_EQ(cell(1).text, std::string{"b"});
    ASSERT_EQ(cell(2).text, cjk);          // wide glyph in its first cell
    ASSERT_FALSE(cell(2).continuation);
    ASSERT_TRUE(cell(3).continuation);     // trailing half of the wide glyph
    ASSERT_EQ(cell(4).text, ecombining);   // combining mark folded into the base
    ASSERT_FALSE(cell(4).continuation);
    ASSERT_EQ(cell(5).text, emoji);        // ZWJ sequence is one cluster
    ASSERT_FALSE(cell(5).continuation);
    ASSERT_TRUE(cell(6).continuation);     // the emoji is wide too

    // The caret advances by exactly 2 columns across the wide CJK glyph: byte
    // offset 2 (before the glyph) resolves to column startx+2, and offset 5 (just
    // after it, at 'e') to column startx+4 — a literal +2.
    auto before = ssg::SelectionNavigator::resolvePosition(line, ssg::ByteOffset{2});
    auto after = ssg::SelectionNavigator::resolvePosition(line, ssg::ByteOffset{5});
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(after.has_value());
    auto caretColumnAt = [&](std::optional<ssg::DocumentPosition> pos) -> int {
        (void)runtime.dispatch(ssg::ClientId{1},
                               {"cursor.set_position", runtime.revision(),
                                ssg::SelectionCommandArguments{pos, std::nullopt}});
        auto s = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
        if (!s) return -1;
        auto g = ssg::Renderer{}.render(*s);
        return g.caret ? g.caret->column : -1;
    };
    int const columnBefore = caretColumnAt(before);
    int const columnAfter = caretColumnAt(after);
    ASSERT_EQ(columnBefore, startx + 2);
    ASSERT_EQ(columnAfter, startx + 4);
    ASSERT_EQ(columnAfter - columnBefore, 2);

    // Encoding: each wide cluster emits exactly one glyph and continuation cells
    // emit nothing, so the CJK and emoji byte sequences each appear exactly once.
    auto frame = ssg::app::encode_ansi_frame(grid, ssg::ColorDepth::Truecolor);
    auto count = [&](std::string const& needle) {
        std::size_t n = 0, pos = 0;
        while ((pos = frame.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
        return n;
    };
    ASSERT_EQ(count(cjk), std::size_t{1});
    ASSERT_EQ(count(emoji), std::size_t{1});
    ASSERT_EQ(count(ecombining), std::size_t{1});

    fs::remove_all(root);
}

// One escape sequence, classified by what it does to terminal STATE.
//
// The point of classifying rather than pattern-matching one family: an
// unrecognised sequence is a test failure, so a mode expressed in a form this
// oracle does not understand cannot be added without teaching it. The previous
// version understood only CSI ? N h/l, so dropping the cursor-style reset left
// a real terminal with a blinking bar cursor and failed nothing.
struct TerminalOp {
    enum class Kind { PrivateSet, PrivateReset, CursorStyle, Unrecognised };
    Kind kind = Kind::Unrecognised;
    int value = 0;
    std::string raw;
};

std::vector<TerminalOp> classifyTerminalOps(std::string const& sequence) {
    std::vector<TerminalOp> ops;
    std::size_t i = 0;
    while (i < sequence.size()) {
        if (sequence[i] != '\x1b') {  // Anything not an escape is not a mode op.
            ops.push_back({TerminalOp::Kind::Unrecognised, 0,
                           std::string{sequence[i]}});
            ++i;
            continue;
        }
        std::size_t const begin = i;
        if (i + 1 >= sequence.size() || sequence[i + 1] != '[') {
            ops.push_back({TerminalOp::Kind::Unrecognised, 0,
                           sequence.substr(begin)});
            break;
        }
        std::size_t j = i + 2;
        bool const isPrivate = j < sequence.size() && sequence[j] == '?';
        if (isPrivate) ++j;
        int value = 0;
        bool digits = false;
        while (j < sequence.size() && sequence[j] >= '0' && sequence[j] <= '9') {
            value = value * 10 + (sequence[j] - '0');
            ++j;
            digits = true;
        }
        // An intermediate byte (here only SP) distinguishes DECSCUSR from an
        // ordinary CSI with the same final.
        bool const hasSpace = j < sequence.size() && sequence[j] == ' ';
        if (hasSpace) ++j;
        if (j >= sequence.size() || !digits) {
            ops.push_back({TerminalOp::Kind::Unrecognised, 0,
                           sequence.substr(begin)});
            break;
        }
        char const final = sequence[j];
        auto kind = TerminalOp::Kind::Unrecognised;
        if (isPrivate && !hasSpace && final == 'h') {
            kind = TerminalOp::Kind::PrivateSet;
        } else if (isPrivate && !hasSpace && final == 'l') {
            kind = TerminalOp::Kind::PrivateReset;
        } else if (!isPrivate && hasSpace && final == 'q') {
            kind = TerminalOp::Kind::CursorStyle;
        }
        ops.push_back({kind, value, sequence.substr(begin, j + 1 - begin)});
        i = j + 1;
    }
    return ops;
}

// Today's sequences are known good, so they are the reference the mode stack
// must reproduce byte for byte.
// Refactors that only change structure must not change these bytes; the ONE
// step that changes the wire output -- moving cursor visibility to a frame
// guard -- edits this reference and nothing else does.
constexpr std::string_view kExpectedSetup =
    "\x1b[?1049h\x1b[5 q\x1b[?1000h\x1b[?1002h\x1b[?1006h";
// No `?25h`: the cursor is hidden and shown within each frame, so teardown has
// no cursor debt to settle. This is the one reference change the spec allows,
// and it belongs to the step that moved cursor visibility to a frame guard.
constexpr std::string_view kExpectedRestore =
    "\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[0 q\x1b[?1049l";

TEST(everyDeclaredModeLeavesExactlyWhatItEnters) {
    // Checked against the DECLARATIONS rather than against two assembled
    // strings, because the declaration is now the single source of truth: a
    // mode carries its own exit, so pairing is a property of each mode instead
    // of a property of two lists that had to mirror each other.
    struct Named { char const* name; ssg::app::TerminalMode mode; };
    Named const declared[]{
        {"alternate screen", ssg::app::kAlternateScreen},
        {"cursor style", ssg::app::kCursorStyleBar},
        {"mouse buttons", ssg::app::kMouseButtons},
        {"mouse motion", ssg::app::kMouseMotion},
        {"mouse SGR coordinates", ssg::app::kMouseSgrCoordinates},
        {"cursor hidden", ssg::app::kCursorHidden},
    };

    for (auto const& [name, mode] : declared) {
        auto const entering = classifyTerminalOps(std::string{mode.enter});
        auto const leaving = classifyTerminalOps(std::string{mode.leave});
        // COMPLETENESS: a mode expressed in a form this oracle does not
        // understand fails here rather than escaping the rules below.
        ASSERT_EQ(entering.size(), std::size_t{1});
        ASSERT_EQ(leaving.size(), std::size_t{1});
        ASSERT_TRUE(entering[0].kind != TerminalOp::Kind::Unrecognised);
        ASSERT_TRUE(leaving[0].kind != TerminalOp::Kind::Unrecognised);

        bool const entersPrivate =
            entering[0].kind == TerminalOp::Kind::PrivateSet ||
            entering[0].kind == TerminalOp::Kind::PrivateReset;
        if (entersPrivate) {
            // A private mode is left by applying the OPPOSITE terminator to the
            // same id -- in either direction.  Hiding the cursor is entered by
            // RESETTING DECTCEM (?25l) and left by setting it, the inverse of
            // the alternate screen and the mouse modes, so the rule cannot
            // assume a mode is always entered by "set".
            ASSERT_EQ(entering[0].value, leaving[0].value);
            bool const opposite =
                (entering[0].kind == TerminalOp::Kind::PrivateSet &&
                 leaving[0].kind == TerminalOp::Kind::PrivateReset) ||
                (entering[0].kind == TerminalOp::Kind::PrivateReset &&
                 leaving[0].kind == TerminalOp::Kind::PrivateSet);
            ASSERT_TRUE(opposite);
        } else {
            // Cursor style is a single slot: whatever shape is selected, the
            // exit must return it to the default, or the user's cursor keeps
            // ssg's shape after ssg exits.
            ASSERT_EQ(entering[0].kind, TerminalOp::Kind::CursorStyle);
            ASSERT_EQ(leaving[0].kind, TerminalOp::Kind::CursorStyle);
            ASSERT_TRUE(entering[0].value != 0);
            ASSERT_EQ(leaving[0].value, 0);
        }
    }
}

TEST(modeStackReproducesTheCuratedSetupAndRestoreSequences) {
    // The curated literals are known good, so entering the same modes in the
    // same order must produce exactly those bytes -- and leaving must produce
    // exactly the reverse. This is what lets the refactor claim it changed
    // structure only.
    std::string written;
    ssg::app::TerminalModes modes{
        [&written](std::string_view bytes) { written.append(bytes); }};
    {
        auto alt = modes.enter(ssg::app::kAlternateScreen);
        auto cursor = modes.enter(ssg::app::kCursorStyleBar);
        auto buttons = modes.enter(ssg::app::kMouseButtons);
        auto motion = modes.enter(ssg::app::kMouseMotion);
        auto sgr = modes.enter(ssg::app::kMouseSgrCoordinates);
        ASSERT_EQ(written, std::string{kExpectedSetup});
        written.clear();
    }
    ASSERT_EQ(written, std::string{kExpectedRestore});
    ASSERT_EQ(modes.depth(), std::size_t{0});
}

TEST(everyEnteredModeIsLeftInReverseOrder) {
    std::string written;
    ssg::app::TerminalModes modes{
        [&written](std::string_view bytes) { written.append(bytes); }};
    {
        auto outer = modes.enter(ssg::app::kAlternateScreen);
        auto inner = modes.enter(ssg::app::kMouseButtons);
        ASSERT_EQ(modes.depth(), std::size_t{2});
    }
    ASSERT_EQ(modes.depth(), std::size_t{0});
    // Entered alt-screen then mouse; must leave mouse then alt-screen.
    auto const mousePos = written.find("\x1b[?1000l");
    auto const altPos = written.find("\x1b[?1049l");
    ASSERT_TRUE(mousePos != std::string::npos);
    ASSERT_TRUE(altPos != std::string::npos);
    ASSERT_TRUE(mousePos < altPos);
}

TEST(theCrashUndoLeavesEveryDeclaredModeInReverseOrder) {
    // What a fatal-signal handler writes. A constant rather than a record of
    // what is currently entered: see all_modes_undo_sequence for why precision
    // there cannot be published safely, and why over-approximating is the safe
    // direction.
    //
    // Assembled from the one list, reversed, rather than by asking the function
    // how it built itself.
    std::string expected;
    for (std::size_t i = std::size(ssg::app::kAllModes); i-- > 0;) {
        expected.append(ssg::app::kAllModes[i].leave);
    }
    auto const undo = std::string{ssg::app::all_modes_undo_sequence()};
    ASSERT_EQ(undo, expected);

    // And the list itself must hold every mode DECLARED, or a mode could be
    // declared, entered, and left out of the crash undo. Counted from the
    // header, so this does not just re-read the list it is checking.
    std::ifstream header{std::string{SSG_TEST_SOURCE_DIR} +
                         "/apps/ssg_terminal.h"};
    std::string const source{std::istreambuf_iterator<char>{header},
                             std::istreambuf_iterator<char>{}};
    ASSERT_FALSE(source.empty());
    std::size_t declared = 0;
    for (std::size_t at = source.find("inline constexpr TerminalMode k");
         at != std::string::npos;
         at = source.find("inline constexpr TerminalMode k", at + 1)) {
        // kAllModes is the list, not a mode.
        std::string_view const listMarker{"inline constexpr TerminalMode kAllModes"};
        if (source.compare(at, listMarker.size(), listMarker) == 0) continue;
        // kKeyboardProtocol is the ONE sanctioned exclusion from the constant
        // crash-undo superset: its leave `CSI < u` is a stack pop, not idempotent,
        // so it must never be written when it was not entered.  It is torn down by
        // its Guard on the normal paths instead.
        std::string_view const kittyMarker{
            "inline constexpr TerminalMode kKeyboardProtocol"};
        if (source.compare(at, kittyMarker.size(), kittyMarker) == 0) continue;
        ++declared;
    }
    ASSERT_EQ(declared, std::size(ssg::app::kAllModes));

    // The non-idempotent Kitty pop must NOT appear in the constant crash-undo.
    ASSERT_TRUE(undo.find(std::string{ssg::app::kKeyboardProtocol.leave}) ==
                std::string::npos);

    // Mouse reporting must be disabled BEFORE the alternate screen is left, or
    // reporting stays on in the primary screen.
    ASSERT_TRUE(undo.find("\x1b[?1000l") < undo.find("\x1b[?1049l"));
}

TEST(kittyKeyboardModeRoundTripsThroughAGuard) {
    // Entering the mode writes the flag-1 push; the Guard's destruction writes the
    // matching pop.  Routing through TerminalModes (not a hand-written pair) is
    // what makes the pop matched-push-only on every normal teardown path.
    std::string written;
    {
        ssg::app::TerminalModes modes{
            [&](std::string_view bytes) { written.append(bytes); }};
        auto guard = modes.enter(ssg::app::kKeyboardProtocol);
        ASSERT_EQ(written, std::string{ssg::app::kKeyboardProtocol.enter});
    }
    ASSERT_EQ(written, std::string{ssg::app::kKeyboardProtocol.enter} +
                           std::string{ssg::app::kKeyboardProtocol.leave});
}

TEST(aFrameWithNoCaretLeavesTheCursorVisible) {
    auto balanced = [](std::string const& frame) {
        auto const shown = frame.rfind("\x1b[?25h");
        auto const hidden = frame.rfind("\x1b[?25l");
        return shown != std::string::npos && hidden != std::string::npos &&
               shown > hidden;
    };

    ssg::CellGrid withCaret;
    withCaret.size = {4, 2};
    withCaret.cells.resize(8);
    withCaret.caret = ssg::GridPosition{1, 1};
    ASSERT_TRUE(balanced(ssg::app::encode_frame(withCaret,
                                                ssg::ColorDepth::Truecolor)));

    ssg::CellGrid withoutCaret;
    withoutCaret.size = {4, 2};
    withoutCaret.cells.resize(8);
    withoutCaret.caret.reset();
    ASSERT_TRUE(balanced(ssg::app::encode_frame(withoutCaret,
                                                ssg::ColorDepth::Truecolor)));

    // And the hide still comes first, so the redraw itself is not visible.
    auto const frame =
        ssg::app::encode_frame(withoutCaret, ssg::ColorDepth::Truecolor);
    ASSERT_TRUE(frame.find("\x1b[?25l") == 0);
}

TEST(classifySignalTagsMapsSignalNumbers) {
    // Empty drain: no events.
    auto none = ssg::app::classify_signal_tags({});
    ASSERT_FALSE(none.resize);
    ASSERT_FALSE(none.terminate.has_value());

    // A single SIGWINCH byte sets resize only.
    std::string winch(1, static_cast<char>(SIGWINCH));
    auto resize = ssg::app::classify_signal_tags(winch);
    ASSERT_TRUE(resize.resize);
    ASSERT_FALSE(resize.terminate.has_value());

    // Duplicate resize tags coalesce to a single resize event.
    std::string winches(5, static_cast<char>(SIGWINCH));
    auto coalesced = ssg::app::classify_signal_tags(winches);
    ASSERT_TRUE(coalesced.resize);

    // SIGTERM sets terminate carrying the exact signal for a correct re-raise.
    std::string term(1, static_cast<char>(SIGTERM));
    auto terminate = ssg::app::classify_signal_tags(term);
    ASSERT_TRUE(terminate.terminate.has_value());
    ASSERT_EQ(*terminate.terminate, SIGTERM);

    // Mixed drain: resize is set and the last terminating signal wins.
    std::string mixed;
    mixed.push_back(static_cast<char>(SIGWINCH));
    mixed.push_back(static_cast<char>(SIGTERM));
    mixed.push_back(static_cast<char>(SIGHUP));
    auto both = ssg::app::classify_signal_tags(mixed);
    ASSERT_TRUE(both.resize);
    ASSERT_TRUE(both.terminate.has_value());
    ASSERT_EQ(*both.terminate, SIGHUP);

    // Unknown bytes are ignored (total function).
    std::string junk(1, static_cast<char>(7));
    auto ignored = ssg::app::classify_signal_tags(junk);
    ASSERT_FALSE(ignored.resize);
    ASSERT_FALSE(ignored.terminate.has_value());
}

TEST(decodeInputMapsPrintablesAndNamedKeys) {
    std::size_t consumed = 0;
    // Lowercase letter: KeyA stroke (no shift) plus committed text.
    auto a = ssg::app::decode_input("a", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_TRUE(a.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(std::string{ssg::keyCodeName(a.stroke.code)}, std::string{"KeyA"});
    ASSERT_FALSE(a.stroke.shift);
    ASSERT_EQ(a.text, std::string{"a"});

    // Uppercase: Shift+KeyZ plus committed text "Z".
    auto z = ssg::app::decode_input("Z", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(z.stroke.code)}, std::string{"KeyZ"});
    ASSERT_TRUE(z.stroke.shift);
    ASSERT_EQ(z.text, std::string{"Z"});

    // Bracket punctuation used by tab chords.
    auto bracket = ssg::app::decode_input("]", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(bracket.stroke.code)}, std::string{"BracketRight"});
    ASSERT_EQ(bracket.text, std::string{"]"});

    // Enter and Backspace are strokes without committed text.
    auto enter = ssg::app::decode_input("\r", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(enter.stroke.code)}, std::string{"Enter"});
    ASSERT_TRUE(enter.text.empty());
    auto back = ssg::app::decode_input("\x7f", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(back.stroke.code)}, std::string{"Backspace"});
}

TEST(decodeInputModifiedArrows) {
    std::size_t consumed = 0;
    // Shift+ArrowUp: ESC [ 1 ; 2 A (modifier 2 -> bitmask 1 = Shift).
    auto shiftUp = ssg::app::decode_input("\x1b[1;2A", true, consumed);
    ASSERT_EQ(consumed, std::size_t{6});
    ASSERT_TRUE(shiftUp.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(std::string{ssg::keyCodeName(shiftUp.stroke.code)}, std::string{"ArrowUp"});
    ASSERT_TRUE(shiftUp.stroke.shift);
    ASSERT_FALSE(shiftUp.stroke.alt);
    ASSERT_FALSE(shiftUp.stroke.control);

    // Ctrl+ArrowRight: modifier 5 -> bitmask 4 = Ctrl.
    auto ctrlRight = ssg::app::decode_input("\x1b[1;5C", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(ctrlRight.stroke.code)}, std::string{"ArrowRight"});
    ASSERT_TRUE(ctrlRight.stroke.control);
    ASSERT_FALSE(ctrlRight.stroke.shift);

    // Alt+ArrowLeft: modifier 3 -> bitmask 2 = Alt.
    auto altLeft = ssg::app::decode_input("\x1b[1;3D", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(altLeft.stroke.code)}, std::string{"ArrowLeft"});
    ASSERT_TRUE(altLeft.stroke.alt);

    // Ctrl+Shift+ArrowDown: modifier 6 -> bitmask 5 = Shift|Ctrl.
    auto csDown = ssg::app::decode_input("\x1b[1;6B", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(csDown.stroke.code)}, std::string{"ArrowDown"});
    ASSERT_TRUE(csDown.stroke.shift);
    ASSERT_TRUE(csDown.stroke.control);
    ASSERT_FALSE(csDown.stroke.alt);

    // Shift+Home / Shift+End.
    auto shiftHome = ssg::app::decode_input("\x1b[1;2H", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(shiftHome.stroke.code)}, std::string{"Home"});
    ASSERT_TRUE(shiftHome.stroke.shift);
    auto shiftEnd = ssg::app::decode_input("\x1b[1;2F", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(shiftEnd.stroke.code)}, std::string{"End"});
    ASSERT_TRUE(shiftEnd.stroke.shift);

    // Plain arrow still decodes unmodified.
    auto plain = ssg::app::decode_input("\x1b[A", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(plain.stroke.code)}, std::string{"ArrowUp"});
    ASSERT_FALSE(plain.stroke.shift);

    // An unsupported modifier (m=9 -> bitmask 8, a Meta bit) falls back to the
    // plain, unmodified arrow, consuming the whole sequence.
    auto meta = ssg::app::decode_input("\x1b[1;9A", true, consumed);
    ASSERT_EQ(consumed, std::size_t{6});
    ASSERT_EQ(std::string{ssg::keyCodeName(meta.stroke.code)}, std::string{"ArrowUp"});
    ASSERT_FALSE(meta.stroke.shift);
    ASSERT_FALSE(meta.stroke.alt);
    ASSERT_FALSE(meta.stroke.control);

    // Ctrl+Alt+ArrowRight: modifier 7 -> bitmask 6 = Alt|Ctrl.
    auto ctrlAlt = ssg::app::decode_input("\x1b[1;7C", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(ctrlAlt.stroke.code)}, std::string{"ArrowRight"});
    ASSERT_TRUE(ctrlAlt.stroke.alt);
    ASSERT_TRUE(ctrlAlt.stroke.control);
    ASSERT_FALSE(ctrlAlt.stroke.shift);

    // A multi-digit modifier parses; m=16 -> bitmask 15 includes the unsupported
    // Meta bit, so it falls back to the plain arrow (consuming all 7 bytes).
    auto multi = ssg::app::decode_input("\x1b[1;16C", true, consumed);
    ASSERT_EQ(consumed, std::size_t{7});
    ASSERT_EQ(std::string{ssg::keyCodeName(multi.stroke.code)}, std::string{"ArrowRight"});
    ASSERT_FALSE(multi.stroke.alt);
    ASSERT_FALSE(multi.stroke.control);
    ASSERT_FALSE(multi.stroke.shift);
}

TEST(decodeInputMetaPrefixedCsiFoldsAlt) {
    std::size_t consumed = 0;
    // A terminal transmitting Alt as a leading ESC sends Alt+Home as ESC ESC[H:
    // the inner CSI decodes to Home and Alt is folded in.
    auto altHome = ssg::app::decode_input("\x1b\x1b[H", true, consumed);
    ASSERT_EQ(consumed, std::size_t{4});
    ASSERT_TRUE(altHome.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(std::string{ssg::keyCodeName(altHome.stroke.code)}, std::string{"Home"});
    ASSERT_TRUE(altHome.stroke.alt);
    ASSERT_FALSE(altHome.stroke.control);
    ASSERT_FALSE(altHome.stroke.shift);

    auto altEnd = ssg::app::decode_input("\x1b\x1b[F", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(altEnd.stroke.code)}, std::string{"End"});
    ASSERT_TRUE(altEnd.stroke.alt);

    // Alt+ArrowLeft via the same meta-prefix form.
    auto altLeft = ssg::app::decode_input("\x1b\x1b[D", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(altLeft.stroke.code)}, std::string{"ArrowLeft"});
    ASSERT_TRUE(altLeft.stroke.alt);

    // An already-modified inner sequence keeps both modifiers (Ctrl from the
    // CSI, Alt from the prefix).
    auto altCtrlHome = ssg::app::decode_input("\x1b\x1b[1;5H", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(altCtrlHome.stroke.code)}, std::string{"Home"});
    ASSERT_TRUE(altCtrlHome.stroke.alt);
    ASSERT_TRUE(altCtrlHome.stroke.control);

    // An incomplete inner CSI keeps the whole thing pending rather than
    // surfacing a spurious bare Escape.
    auto pending = ssg::app::decode_input("\x1b\x1b[1;3", false, consumed);
    ASSERT_TRUE(pending.status == ssg::app::DecodeStatus::incomplete);

    // ESC ESC with no CSI introducer following is still a bare Escape.
    auto doubleEsc = ssg::app::decode_input("\x1b\x1bx", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_EQ(std::string{ssg::keyCodeName(doubleEsc.stroke.code)}, std::string{"Escape"});
    ASSERT_FALSE(doubleEsc.stroke.alt);
}

TEST(decodeInputTildeHomeEndPlainAndModified) {
    std::size_t consumed = 0;
    // Terminals that send Home/End in the tilde form rather than the letter form:
    // ESC[1~/ESC[7~ = Home, ESC[4~/ESC[8~ = End.
    auto home1 = ssg::app::decode_input("\x1b[1~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{4});
    ASSERT_EQ(std::string{ssg::keyCodeName(home1.stroke.code)}, std::string{"Home"});
    auto home7 = ssg::app::decode_input("\x1b[7~", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(home7.stroke.code)}, std::string{"Home"});
    auto end4 = ssg::app::decode_input("\x1b[4~", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(end4.stroke.code)}, std::string{"End"});
    auto end8 = ssg::app::decode_input("\x1b[8~", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(end8.stroke.code)}, std::string{"End"});

    // Modified tilde forms fold in the modifier (Alt = m 3, Ctrl = m 5).
    auto altHome = ssg::app::decode_input("\x1b[1;3~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{6});
    ASSERT_EQ(std::string{ssg::keyCodeName(altHome.stroke.code)}, std::string{"Home"});
    ASSERT_TRUE(altHome.stroke.alt);
    ASSERT_FALSE(altHome.stroke.control);
    auto altEnd = ssg::app::decode_input("\x1b[4;3~", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(altEnd.stroke.code)}, std::string{"End"});
    ASSERT_TRUE(altEnd.stroke.alt);
    auto ctrlHome7 = ssg::app::decode_input("\x1b[7;5~", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(ctrlHome7.stroke.code)}, std::string{"Home"});
    ASSERT_TRUE(ctrlHome7.stroke.control);
    ASSERT_FALSE(ctrlHome7.stroke.alt);

    // Delete/Page tilde forms still work after adding the Home/End numbers.
    auto del = ssg::app::decode_input("\x1b[3~", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(del.stroke.code)}, std::string{"Delete"});
    auto pageUp = ssg::app::decode_input("\x1b[5~", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(pageUp.stroke.code)}, std::string{"PageUp"});

    // A split modified tilde form is incomplete until the '~' arrives.
    auto pending = ssg::app::decode_input("\x1b[4;3", false, consumed);
    ASSERT_TRUE(pending.status == ssg::app::DecodeStatus::incomplete);
}

TEST(decodeInputStripsLockModifiersFromFunctionalKeys) {
    std::size_t consumed = 0;
    // A terminal speaking the Kitty protocol reports lock states in the modifier
    // field of the legacy letter/tilde functional-key forms. Captured from a real
    // terminal with NumLock on: Alt+Home = ESC[1;131H, Alt+End = ESC[1;131F
    // (modifier 131 -> bitmask 130 = NumLock(128) | Alt(2)). The lock bit must be
    // stripped so Alt survives and the binding resolves.
    auto altHome = ssg::app::decode_input("\x1b[1;131H", true, consumed);
    ASSERT_EQ(consumed, std::size_t{8});
    ASSERT_EQ(std::string{ssg::keyCodeName(altHome.stroke.code)}, std::string{"Home"});
    ASSERT_TRUE(altHome.stroke.alt);
    ASSERT_FALSE(altHome.stroke.control);
    ASSERT_FALSE(altHome.stroke.shift);
    auto altEnd = ssg::app::decode_input("\x1b[1;131F", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(altEnd.stroke.code)}, std::string{"End"});
    ASSERT_TRUE(altEnd.stroke.alt);

    // Plain Home with NumLock on (modifier 129 = NumLock only) stays unmodified.
    auto plainHome = ssg::app::decode_input("\x1b[1;129H", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(plainHome.stroke.code)}, std::string{"Home"});
    ASSERT_FALSE(plainHome.stroke.alt);
    ASSERT_FALSE(plainHome.stroke.control);
    ASSERT_FALSE(plainHome.stroke.shift);

    // CapsLock (bit 6 = 64, modifier 65) is likewise stripped; Ctrl+Shift+End
    // with CapsLock (modifier 1 + 64 + 1 + 4 = 70) keeps only Ctrl+Shift.
    auto capsCtrlShiftEnd = ssg::app::decode_input("\x1b[1;70F", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(capsCtrlShiftEnd.stroke.code)}, std::string{"End"});
    ASSERT_TRUE(capsCtrlShiftEnd.stroke.control);
    ASSERT_TRUE(capsCtrlShiftEnd.stroke.shift);
    ASSERT_FALSE(capsCtrlShiftEnd.stroke.alt);

    // The tilde form carries lock bits too (Alt+End as ESC[4;131~).
    auto altEndTilde = ssg::app::decode_input("\x1b[4;131~", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(altEndTilde.stroke.code)}, std::string{"End"});
    ASSERT_TRUE(altEndTilde.stroke.alt);

    // A genuine unsupported modifier (Super, bit 3 = 8, modifier 9) still falls
    // back to the plain key -- stripping locks must not weaken that guard.
    auto superArrow = ssg::app::decode_input("\x1b[1;9A", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(superArrow.stroke.code)}, std::string{"ArrowUp"});
    ASSERT_FALSE(superArrow.stroke.alt);
    ASSERT_FALSE(superArrow.stroke.control);
    ASSERT_FALSE(superArrow.stroke.shift);
}

// Encode a KeyStroke as the Kitty `CSI unicode-key ; mods u` bytes, for the
// parity oracle below. Returns nullopt for keys Kitty does not send as a `u`
// event under the disambiguate flag (arrows/Home/End/Page/Delete/function keys
// keep their legacy CSI forms), which the legacy tests already cover.
std::optional<int> kittyCodepointFor(ssg::KeyCode code) {
    using K = ssg::KeyCode;
    if (code >= K::KeyA && code <= K::KeyZ)
        return 'a' + (static_cast<int>(code) - static_cast<int>(K::KeyA));
    if (code >= K::Digit0 && code <= K::Digit9)
        return '0' + (static_cast<int>(code) - static_cast<int>(K::Digit0));
    switch (code) {
    case K::Escape: return 27;
    case K::Enter: return 13;
    case K::Tab: return 9;
    case K::Backspace: return 127;
    case K::Space: return ' ';
    case K::BracketLeft: return '[';
    case K::BracketRight: return ']';
    case K::Backslash: return '\\';
    case K::Semicolon: return ';';
    case K::Quote: return '\'';
    case K::Comma: return ',';
    case K::Period: return '.';
    case K::Slash: return '/';
    case K::Minus: return '-';
    case K::Equal: return '=';
    case K::Backquote: return '`';
    default: return std::nullopt;
    }
}

std::string kittyBytes(int codepoint, int bitmask) {
    std::string out = "\x1b[" + std::to_string(codepoint);
    if (bitmask != 0) out += ";" + std::to_string(1 + bitmask);
    out += "u";
    return out;
}

int kittyBitmask(const ssg::KeyStroke& stroke) {
    return (stroke.shift ? 0b1 : 0) | (stroke.alt ? 0b10 : 0) |
           (stroke.control ? 0b100 : 0) | (stroke.meta ? 0b100000 : 0);
}

// The load-bearing parity oracle: for EVERY binding in the real default keymap,
// feeding the Kitty encoding of its KeyStroke must decode back to exactly that
// KeyStroke. Driven off the live keymap so a new binding cannot be added without
// coverage. Proves decodeKittyKey is complete for the keys SSG actually binds --
// including the non-letter modified keys flag 1 reroutes (Alt+Backspace,
// Alt+Slash, Alt+Digit8, Alt+Period/Comma) that a partial decoder would miss.
TEST(decodeKittyKeyMatchesEveryDefaultBinding) {
    auto root = fs::temp_directory_path() / "ssg-kitty-parity";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "recovery");
    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1}).accepted());
    auto snap = runtime.snapshot(ssg::ClientId{1}, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(snap.has_value());
    if (!snap.has_value()) return;

    int covered = 0;
    for (auto const& binding : snap->sections().keymap.bindings) {
        if (binding.sequence.size() != 1) continue;  // all defaults are single strokes
        auto const& stroke = binding.sequence.front();
        auto const codepoint = kittyCodepointFor(stroke.code);
        if (!codepoint) continue;  // legacy-encoded key; covered by legacy tests
        std::size_t consumed = 0;
        auto const bytes = kittyBytes(*codepoint, kittyBitmask(stroke));
        auto const decoded = ssg::app::decode_input(bytes, true, consumed);
        ASSERT_EQ(consumed, bytes.size());
        ASSERT_TRUE(decoded.status == ssg::app::DecodeStatus::key);
        ASSERT_TRUE(decoded.stroke == stroke);
        ++covered;
    }
    ASSERT_TRUE(covered > 0);  // the oracle actually exercised bindings
}

TEST(decodeKittyKeyHandCasesAndCapsLockImmunity) {
    std::size_t consumed = 0;
    // Alt+Shift+P: unicode key 'p' (112), mods = 1 + (shift|alt) = 4.
    auto altShiftP = ssg::app::decode_input("\x1b[112;4u", true, consumed);
    ASSERT_TRUE(altShiftP.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(std::string{ssg::keyCodeName(altShiftP.stroke.code)}, std::string{"KeyP"});
    ASSERT_TRUE(altShiftP.stroke.alt);
    ASSERT_TRUE(altShiftP.stroke.shift);
    ASSERT_FALSE(altShiftP.stroke.control);
    ASSERT_TRUE(altShiftP.text.empty());  // a modified key commits no text

    // The SAME key with caps-lock ALSO held: mods = 1 + (shift|alt|caps) where
    // caps is bit 6 (64), so 1 + 3 + 64 = 68. The caps bit must NOT perturb the
    // stroke -- this is the whole fix, and why the binding no longer swaps under
    // caps lock.
    auto withCaps = ssg::app::decode_input("\x1b[112;68u", true, consumed);
    ASSERT_TRUE(withCaps.stroke == altShiftP.stroke);

    // Ctrl+C: unicode 'c' (99), mods = 1 + ctrl(4) = 5.
    auto ctrlC = ssg::app::decode_input("\x1b[99;5u", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(ctrlC.stroke.code)}, std::string{"KeyC"});
    ASSERT_TRUE(ctrlC.stroke.control);
    ASSERT_FALSE(ctrlC.stroke.shift);

    // Plain Escape disambiguates to CSI 27 u under flag 1 -- the freed-Escape
    // (prompt cancel) must survive the reroute.
    auto esc = ssg::app::decode_input("\x1b[27u", true, consumed);
    ASSERT_TRUE(esc.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(std::string{ssg::keyCodeName(esc.stroke.code)}, std::string{"Escape"});
    ASSERT_FALSE(esc.stroke.alt);
}

TEST(everyEnterEncodingNormalizesToBareEnter) {
    std::size_t consumed = 0;
    auto isBareEnter = [](ssg::app::Decoded const& d) {
        return d.status == ssg::app::DecodeStatus::key &&
               d.stroke.code == ssg::KeyCode::Enter && !d.stroke.shift &&
               !d.stroke.control && !d.stroke.alt && !d.stroke.meta;
    };

    // Plain CR / LF stay bare Enter (regression guard).
    ASSERT_TRUE(isBareEnter(ssg::app::decode_input("\r", true, consumed)));
    ASSERT_TRUE(isBareEnter(ssg::app::decode_input("\n", true, consumed)));

    // Kitty modified Enter (CSI 13 ; mods u): Shift, Ctrl, Alt, Ctrl+Alt, and
    // Meta (bit5) all fold onto a bare Enter so the newline binding fires.
    ASSERT_TRUE(isBareEnter(ssg::app::decode_input("\x1b[13;2u", true, consumed)));
    ASSERT_TRUE(isBareEnter(ssg::app::decode_input("\x1b[13;5u", true, consumed)));
    ASSERT_TRUE(isBareEnter(ssg::app::decode_input("\x1b[13;3u", true, consumed)));
    ASSERT_TRUE(isBareEnter(ssg::app::decode_input("\x1b[13;8u", true, consumed)));
    ASSERT_TRUE(isBareEnter(ssg::app::decode_input("\x1b[13;33u", true, consumed)));

    // Kitty keypad Enter (KP_ENTER = 57414), plain and modified.
    ASSERT_TRUE(isBareEnter(ssg::app::decode_input("\x1b[57414u", true, consumed)));
    ASSERT_TRUE(
        isBareEnter(ssg::app::decode_input("\x1b[57414;2u", true, consumed)));

    // SS3 numpad Enter (application-keypad mode): ESC O M, consumed whole.
    auto ss3 = ssg::app::decode_input("\x1bOM", true, consumed);
    ASSERT_TRUE(isBareEnter(ss3));
    ASSERT_EQ(consumed, std::size_t{3});

    // Alt-prefixed SS3 numpad Enter (ESC ESC O M) rides the recursive alt-chord
    // path; the outer normalization still strips the folded-in Alt.
    ASSERT_TRUE(isBareEnter(ssg::app::decode_input("\x1b\x1bOM", true, consumed)));

    // ESC [ M (X10 mouse) must NOT be mistaken for Enter -- only the SS3
    // introducer means numpad Enter.
    auto mouse = ssg::app::decode_input("\x1b[M\x20\x21\x21", true, consumed);
    ASSERT_TRUE(mouse.status != ssg::app::DecodeStatus::key ||
                mouse.stroke.code != ssg::KeyCode::Enter);
}

TEST(decodeKittyKeySelfIdentifyingAndMalformed) {
    std::size_t consumed = 0;
    // A private-prefixed `CSI ? ... u` is the KEYBOARD-PROTOCOL capability reply,
    // never a key -- it must classify as a reply so the probe still works.
    auto reply = ssg::app::decode_input("\x1b[?1u", true, consumed);
    ASSERT_TRUE(reply.status == ssg::app::DecodeStatus::reply);

    // A non-private `CSI <n> u` is a key event (the self-identifying shape).
    auto key = ssg::app::decode_input("\x1b[112u", true, consumed);
    ASSERT_TRUE(key.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(std::string{ssg::keyCodeName(key.stroke.code)}, std::string{"KeyP"});

    // Field-1 sub-parameters (shifted-key : base-layout-key) are skipped; the
    // unshifted key code and the modifier field still decode.
    auto subparams = ssg::app::decode_input("\x1b[112:80;4u", true, consumed);
    ASSERT_TRUE(subparams.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(std::string{ssg::keyCodeName(subparams.stroke.code)}, std::string{"KeyP"});
    ASSERT_TRUE(subparams.stroke.alt);
    ASSERT_TRUE(subparams.stroke.shift);

    // An event-type sub-parameter on the modifier field is skipped.
    auto eventType = ssg::app::decode_input("\x1b[99;5:1u", true, consumed);
    ASSERT_TRUE(eventType.status == ssg::app::DecodeStatus::key);
    ASSERT_TRUE(eventType.stroke.control);

    // An empty modifier field decodes as no modifiers, not garbage.
    auto emptyMods = ssg::app::decode_input("\x1b[112;u", true, consumed);
    ASSERT_TRUE(emptyMods.status == ssg::app::DecodeStatus::key);
    ASSERT_FALSE(emptyMods.stroke.alt);
    ASSERT_FALSE(emptyMods.stroke.shift);

    // A key SSG does not name (a Kitty functional PUA code) is consumed whole and
    // emits nothing, rather than leaking bytes into the document.
    auto unknown = ssg::app::decode_input("\x1b[57400u", true, consumed);
    ASSERT_EQ(consumed, std::string{"\x1b[57400u"}.size());
    ASSERT_TRUE(unknown.status == ssg::app::DecodeStatus::none);

    // A truncated `CSI ... u` is incomplete (await more), never a partial key.
    auto partial = ssg::app::decode_input("\x1b[112;4", false, consumed);
    ASSERT_TRUE(partial.status == ssg::app::DecodeStatus::incomplete);
}

TEST(decodeInputModifiedArrowSplitReadsAreIncomplete) {
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
    ASSERT_EQ(std::string{ssg::keyCodeName(done.stroke.code)}, std::string{"ArrowLeft"});
    ASSERT_EQ(consumed, std::size_t{7});

    // A pathologically long modifier parameter must not overflow the decimal
    // accumulator; it saturates, falls back to the plain arrow, and consumes the
    // whole sequence.
    std::string huge = "\x1b[1;";
    huge.append(40, '9');
    huge += "A";
    auto overflow = ssg::app::decode_input(huge, true, consumed);
    ASSERT_EQ(consumed, huge.size());
    ASSERT_EQ(std::string{ssg::keyCodeName(overflow.stroke.code)}, std::string{"ArrowUp"});
    ASSERT_FALSE(overflow.stroke.shift);
    ASSERT_FALSE(overflow.stroke.alt);
    ASSERT_FALSE(overflow.stroke.control);
}

TEST(decodeInputDeleteKeyPlainAndModified) {
    std::size_t consumed = 0;
    // Plain Delete: ESC [ 3 ~. Regression test: this byte sequence used to
    // fall through to the unknown-CSI `default` branch (only consuming the
    // "ESC [ 3" introducer), leaving the trailing '~' to be decoded on the
    // NEXT call as plain printable text -- inserting a literal "~" instead
    // of deleting forward.
    auto del = ssg::app::decode_input("\x1b[3~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{4});
    ASSERT_TRUE(del.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(std::string{ssg::keyCodeName(del.stroke.code)}, std::string{"Delete"});
    ASSERT_TRUE(del.text.empty());

    // Modified form ESC [ 3 ; m ~ (m = 1 + bitmask). Shift+Delete: m=2.
    auto shiftDel = ssg::app::decode_input("\x1b[3;2~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{6});
    ASSERT_EQ(std::string{ssg::keyCodeName(shiftDel.stroke.code)}, std::string{"Delete"});
    ASSERT_TRUE(shiftDel.stroke.shift);

    // Split reads of the plain form are incomplete until the '~' arrives.
    for (auto const* partial : {"\x1b[3"}) {
        consumed = 99;
        auto decoded = ssg::app::decode_input(partial, true, consumed);
        ASSERT_TRUE(decoded.status == ssg::app::DecodeStatus::incomplete);
        ASSERT_EQ(consumed, std::size_t{0});
    }
}

TEST(decodeInputPageKeysPlainAndModified) {
    std::size_t consumed = 0;
    // Plain PageUp / PageDown: ESC [ 5 ~ / ESC [ 6 ~.
    auto pageUp = ssg::app::decode_input("\x1b[5~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{4});
    ASSERT_TRUE(pageUp.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(std::string{ssg::keyCodeName(pageUp.stroke.code)}, std::string{"PageUp"});
    ASSERT_FALSE(pageUp.stroke.shift);
    auto pageDown = ssg::app::decode_input("\x1b[6~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{4});
    ASSERT_EQ(std::string{ssg::keyCodeName(pageDown.stroke.code)}, std::string{"PageDown"});

    // Modified form ESC [ 5 ; m ~ (m = 1 + bitmask). Shift+PageUp: m=2 -> Shift.
    auto shiftPgup = ssg::app::decode_input("\x1b[5;2~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{6});
    ASSERT_EQ(std::string{ssg::keyCodeName(shiftPgup.stroke.code)}, std::string{"PageUp"});
    ASSERT_TRUE(shiftPgup.stroke.shift);
    ASSERT_FALSE(shiftPgup.stroke.control);
    ASSERT_FALSE(shiftPgup.stroke.alt);

    // Shift+PageDown.
    auto shiftPgdn = ssg::app::decode_input("\x1b[6;2~", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(shiftPgdn.stroke.code)}, std::string{"PageDown"});
    ASSERT_TRUE(shiftPgdn.stroke.shift);

    // Ctrl+PageUp: m=5 -> bitmask 4 = Ctrl.
    auto ctrlPgup = ssg::app::decode_input("\x1b[5;5~", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(ctrlPgup.stroke.code)}, std::string{"PageUp"});
    ASSERT_TRUE(ctrlPgup.stroke.control);
    ASSERT_FALSE(ctrlPgup.stroke.shift);

    // Split reads of the modified form are incomplete until the '~' arrives.
    for (auto const* partial : {"\x1b[5", "\x1b[5;", "\x1b[5;2"}) {
        consumed = 99;
        auto decoded = ssg::app::decode_input(partial, true, consumed);
        ASSERT_TRUE(decoded.status == ssg::app::DecodeStatus::incomplete);
        ASSERT_EQ(consumed, std::size_t{0});
    }
    // A '5'-prefixed sequence that is neither '~' nor ';' is skipped, not misread.
    auto junk = ssg::app::decode_input("\x1b[5X", true, consumed);
    ASSERT_EQ(consumed, std::size_t{4});
    ASSERT_TRUE(junk.status == ssg::app::DecodeStatus::none);
}

TEST(decodeInputArrowsAndMouse) {
    std::size_t consumed = 0;
    auto up = ssg::app::decode_input("\x1b[A", true, consumed);
    ASSERT_EQ(consumed, std::size_t{3});
    ASSERT_EQ(std::string{ssg::keyCodeName(up.stroke.code)}, std::string{"ArrowUp"});
    auto down = ssg::app::decode_input("\x1b[B", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(down.stroke.code)}, std::string{"ArrowDown"});

    auto wheel = ssg::app::decode_input("\x1b[<65;10;5M", true, consumed);
    ASSERT_TRUE(wheel.status == ssg::app::DecodeStatus::scroll);
    ASSERT_EQ(wheel.scroll, std::int64_t{3});
    // The wheel carries its 0-based grid position (SGR 1-based 10,5 -> 9,4) so
    // the app can route it to the region under the pointer.
    ASSERT_EQ(wheel.pointer.column, 9);
    ASSERT_EQ(wheel.pointer.row, 4);
}

TEST(decodeInputEscapeBoundaryIsBounded) {
    std::size_t consumed = 0;
    // A buffered CSI introducer disambiguates to an arrow, not an Escape stroke.
    auto arrow = ssg::app::decode_input("\x1b[A", false, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(arrow.stroke.code)}, std::string{"ArrowUp"});

    // ESC followed by a printable coalesces into one Alt stroke (legacy
    // meta-prefix), consuming both bytes.
    auto escThen = ssg::app::decode_input("\x1bs", false, consumed);
    ASSERT_EQ(consumed, std::size_t{2});
    ASSERT_EQ(std::string{ssg::keyCodeName(escThen.stroke.code)}, std::string{"KeyS"});
    ASSERT_TRUE(escThen.stroke.alt);

    // A lone ESC with more input possibly coming: incomplete, consume nothing.
    auto pending = ssg::app::decode_input("\x1b", false, consumed);
    ASSERT_TRUE(pending.status == ssg::app::DecodeStatus::incomplete);
    ASSERT_EQ(consumed, std::size_t{0});

    // A lone ESC with input exhausted (bounded read returned nothing): the
    // Escape stroke.
    auto exhausted = ssg::app::decode_input("\x1b", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_EQ(std::string{ssg::keyCodeName(exhausted.stroke.code)}, std::string{"Escape"});

    // A truncated CSI is always incomplete regardless of exhaustion (the final
    // byte has not arrived).
    auto partial = ssg::app::decode_input("\x1b[", true, consumed);
    ASSERT_TRUE(partial.status == ssg::app::DecodeStatus::incomplete);
}

// Legacy meta-prefix: Alt+<key> transmits as ESC then the key's byte, so the
// decoder coalesces ESC+printable into one Alt stroke.  Case supplies Shift for
// letters (there is no shift+lowercase on the wire); a shifted symbol carries no
// keycode and rides as text.  C0 controls become Ctrl+<letter>.
TEST(decodeInputCoalescesMetaPrefixIntoAltStrokes) {
    std::size_t consumed = 0;

    // ESC s -> Alt+KeyS (lowercase: no shift), consuming both bytes.
    auto altS = ssg::app::decode_input("\x1bs", false, consumed);
    ASSERT_EQ(consumed, std::size_t{2});
    ASSERT_TRUE(altS.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(std::string{ssg::keyCodeName(altS.stroke.code)}, std::string{"KeyS"});
    ASSERT_TRUE(altS.stroke.alt);
    ASSERT_FALSE(altS.stroke.shift);

    // ESC P -> Alt+Shift+KeyP; ESC p -> Alt+KeyP.  Distinct strokes: case is the
    // only shift signal for a letter.
    auto altShiftP = ssg::app::decode_input("\x1bP", false, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(altShiftP.stroke.code)}, std::string{"KeyP"});
    ASSERT_TRUE(altShiftP.stroke.alt);
    ASSERT_TRUE(altShiftP.stroke.shift);
    auto altP = ssg::app::decode_input("\x1bp", false, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(altP.stroke.code)}, std::string{"KeyP"});
    ASSERT_TRUE(altP.stroke.alt);
    ASSERT_FALSE(altP.stroke.shift);

    // ESC * (Shift+8) has no keycode -- a shifted number-row symbol is
    // unbindable and rides as text, never {Digit8, shift}.
    auto altStar = ssg::app::decode_input("\x1b*", false, consumed);
    ASSERT_EQ(consumed, std::size_t{2});
    ASSERT_TRUE(altStar.stroke.code == ssg::KeyCode::None);
    ASSERT_EQ(altStar.text, std::string{"*"});

    // C0 control byte -> Ctrl+<letter>.  0x13 = Ctrl+S.
    auto ctrlS = ssg::app::decode_input("\x13", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_TRUE(ctrlS.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(std::string{ssg::keyCodeName(ctrlS.stroke.code)}, std::string{"KeyS"});
    ASSERT_TRUE(ctrlS.stroke.control);
    ASSERT_TRUE(ctrlS.text.empty());

    // Bytes handled as named keys above never fall into the C0 rule: Tab, Enter,
    // Backspace stay themselves.
    auto tab = ssg::app::decode_input("\x09", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(tab.stroke.code)}, std::string{"Tab"});
    auto enter = ssg::app::decode_input("\x0d", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(enter.stroke.code)}, std::string{"Enter"});
    auto lf = ssg::app::decode_input("\x0a", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(lf.stroke.code)}, std::string{"Enter"});

    // Alt+Backspace (ESC 0x7f) is one Alt+Backspace stroke -- NOT keycode-less
    // text -- so the delete-word-backward binding resolves.  Alt+Enter/Tab the
    // same way.
    auto altBksp = ssg::app::decode_input("\x1b\x7f", false, consumed);
    ASSERT_EQ(consumed, std::size_t{2});
    ASSERT_EQ(std::string{ssg::keyCodeName(altBksp.stroke.code)}, std::string{"Backspace"});
    ASSERT_TRUE(altBksp.stroke.alt);
    ASSERT_TRUE(altBksp.text.empty());
    auto altBksp8 = ssg::app::decode_input("\x1b\x08", false, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(altBksp8.stroke.code)}, std::string{"Backspace"});
    ASSERT_TRUE(altBksp8.stroke.alt);

    // A lone ESC still resolves to Escape once input is exhausted.
    auto esc = ssg::app::decode_input("\x1b", true, consumed);
    ASSERT_EQ(std::string{ssg::keyCodeName(esc.stroke.code)}, std::string{"Escape"});
    // ESC ESC is a bare Escape (consume one), not Alt+Escape.
    auto escEsc = ssg::app::decode_input("\x1b\x1b", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_EQ(std::string{ssg::keyCodeName(escEsc.stroke.code)}, std::string{"Escape"});
}

// Drain `bytes` through the decoder the way the main loop does, returning every
// committed text fragment.  `incomplete` ends the drain, leaving the remainder
// reported as held.
struct DrainResult {
    std::string text;
    std::size_t held = 0;
};

DrainResult drainInput(std::string_view bytes) {
    DrainResult result;
    std::string buffer{bytes};
    while (!buffer.empty()) {
        std::size_t consumed = 0;
        auto const decoded = ssg::app::decode_input(buffer, true, consumed);
        if (decoded.status == ssg::app::DecodeStatus::incomplete || consumed == 0) {
            result.held = buffer.size();
            break;
        }
        result.text += decoded.text;
        buffer.erase(0, consumed);
    }
    return result;
}

// Oracle: a terminal
// reply is a report, not typing.  Whatever the decoder makes of a sequence it
// does not recognize, it must never turn its bytes into document text -- that is
// a capability answer being inserted into the user's file.
TEST(noCapabilityReplyIsEverEmittedAsText) {
    struct Reply {
        char const* name;
        char const* bytes;
    };
    for (auto const& reply : {
             Reply{"DA1", "\x1b[?62;22c"},
             Reply{"DECRQM 2026", "\x1b[?2026;2$y"},
             Reply{"kitty keyboard", "\x1b[?1u"},
             Reply{"cell pixel size", "\x1b[6;17;8t"},
             Reply{"text area size", "\x1b[4;1080;1920t"},
             Reply{"DA1 with OSC 52", "\x1b[?62;4;52c"},
         }) {
        auto const drained = drainInput(reply.bytes);
        ASSERT_EQ(std::string{reply.name} + ":" + drained.text,
                  std::string{reply.name} + ":");
        ASSERT_EQ(std::string{reply.name} + ":" + std::to_string(drained.held),
                  std::string{reply.name} + ":0");
    }
}

// Oracle (INV-decode-terminates): no forward scan may hold an unbounded amount
// of buffered input waiting for a terminator that may never arrive.  A run of
// parameter bytes with no final byte is the case that requires the cap -- any
// byte outside 0x30-0x3F would itself terminate the sequence.
TEST(anUnboundedSequenceScanCannotHoldTheInputBuffer) {
    // Parameter bytes only (digits and ';'), never terminated.
    std::string const runaway =
        std::string{"\x1b["} + std::string(4096, '1') + std::string(4096, ';');
    std::size_t consumed = 0;
    auto const decoded = ssg::app::decode_input(runaway, true, consumed);
    ASSERT_TRUE(decoded.status != ssg::app::DecodeStatus::incomplete);
    ASSERT_TRUE(consumed > 0);
    ASSERT_TRUE(consumed <= ssg::app::kMaxSequenceBytes);

    // A truncated SGR mouse prefix must not swallow arbitrary typed text while
    // hunting for its 'M'/'m'.
    std::string const mouseRunaway = std::string{"\x1b[<"} + std::string(4096, '9');
    consumed = 0;
    auto const mouse = ssg::app::decode_input(mouseRunaway, true, consumed);
    ASSERT_TRUE(mouse.status != ssg::app::DecodeStatus::incomplete);
    ASSERT_TRUE(consumed > 0);
    ASSERT_TRUE(consumed <= ssg::app::kMaxSequenceBytes);

    // A short unterminated prefix is still held, so a sequence split across two
    // reads reassembles rather than being discarded.
    consumed = 99;
    auto const split = ssg::app::decode_input("\x1b[1;2", true, consumed);
    ASSERT_TRUE(split.status == ssg::app::DecodeStatus::incomplete);
    ASSERT_EQ(consumed, std::size_t{0});
}

// Oracle (INV-reply-never-input): a reply arriving in two TCP segments over SSH
// is the highest-risk transition in reply recognition -- every proper prefix must
// be held rather than half-consumed, and the completed sequence must surface as
// one reply carrying its bytes verbatim.
TEST(aReplySplitAcrossReadsIsStillConsumedWhole) {
    for (std::string_view whole : {"\x1b[?62;22c", "\x1b[?2026;2$y", "\x1b[?1u"}) {
        for (std::size_t prefix = 2; prefix < whole.size(); ++prefix) {
            std::size_t consumed = 99;
            auto const partial =
                ssg::app::decode_input(whole.substr(0, prefix), false, consumed);
            ASSERT_TRUE(partial.status == ssg::app::DecodeStatus::incomplete);
            ASSERT_EQ(consumed, std::size_t{0});
            ASSERT_TRUE(partial.text.empty());
        }
        std::size_t consumed = 0;
        auto const complete = ssg::app::decode_input(whole, false, consumed);
        ASSERT_TRUE(complete.status == ssg::app::DecodeStatus::reply);
        ASSERT_EQ(consumed, whole.size());
        ASSERT_EQ(complete.reply, std::string{whole});
        ASSERT_TRUE(complete.text.empty());
    }
}

// The DCS/OSC/APC/PM/SOS string introducers (ESC P/]/X/^/_) are byte-identical
// to the Alt+<key> chords the meta-prefix coalescing produces, so a stray such
// reply would be mistaken for a keystroke.  That is safe only while SSG asks no
// DCS/OSC question, so pin the real invariant: nothing under apps/ emits a
// sequence that could solicit such a reply.  Adding one must fail here.
TEST(noDcsOrOscQueryMaySolicitAnUnparsedReply) {
    // The introducers now coalesce to Alt strokes (keyboard input); only the
    // no-query invariant below keeps a genuine reply from ever reaching here.
    std::size_t consumed = 0;
    auto const osc = ssg::app::decode_input("\x1b]", true, consumed);
    ASSERT_TRUE(osc.status == ssg::app::DecodeStatus::key);
    ASSERT_EQ(std::string{ssg::keyCodeName(osc.stroke.code)}, std::string{"BracketRight"});
    ASSERT_TRUE(osc.stroke.alt);
    // OSC 52 WRITE is emitted (see encode_clipboard_write) and is deliberately
    // allowed: a write carries a payload and asks nothing, so no reply can come
    // back.  The scan below therefore looks for a QUERY -- an OSC or DCS ending
    // in "?" before its terminator, which is the form that solicits an answer.
    // Adding one must fail here until the decoder can parse what it will get
    // back, or the answer lands in the user's document.
    std::vector<std::string> emitters;
    for (auto const& entry : std::filesystem::directory_iterator{
             std::filesystem::path{SSG_TEST_SOURCE_DIR} / "apps"}) {
        if (!entry.is_regular_file()) continue;
        auto const extension = entry.path().extension().string();
        if (extension != ".cpp" && extension != ".h") continue;
        std::ifstream input{entry.path()};
        std::ostringstream contents;
        contents << input.rdbuf();
        auto const text = contents.str();
        for (auto const* const introducer : {"\\x1bP", "\\033P", "\\x1b]", "\\033]"}) {
            for (std::size_t at = text.find(introducer); at != std::string::npos;
                 at = text.find(introducer, at + 1)) {
                // A query ends "?" then its terminator; a write does not.
                auto const line = text.substr(at, text.find('\n', at) - at);
                if (line.find("?\\x1b\\\\") != std::string::npos ||
                    line.find("?\\a") != std::string::npos ||
                    line.find(";?") != std::string::npos) {
                    emitters.push_back(entry.path().filename().string());
                }
            }
        }
    }
    ASSERT_TRUE(emitters.empty());
}

// A fake environment, so capability resolution is testable without touching the
// real one and without a terminal.
ssg::app::TerminalCapabilities::EnvironmentLookup fakeEnvironment(
    std::map<std::string, std::string> variables) {
    return [table = std::move(variables)](std::string_view name) -> char const* {
        auto const found = table.find(std::string{name});
        return found == table.end() ? nullptr : found->second.c_str();
    };
}

// Oracle (the DA1 fence): a speculative question that goes unanswered before the
// fence arrives means the feature is absent.  Without the fence there is no way
// to tell "does not support it" from "has not answered yet", and SSG would wait
// forever on the terminals it most needs to detect.
TEST(onlyADa1ReplyMeansTheFeatureIsAbsent) {
    // A terminal that answers everything.
    ssg::app::TerminalCapabilities rich{fakeEnvironment({})};
    (void)rich.beginProbe();
    rich.observeReply("\x1b[?2026;2$y");
    rich.observeReply("\x1b[?1u");
    rich.observeReply("\x1b[?62;4;52c");
    ASSERT_TRUE(rich.has(ssg::app::Capability::SynchronizedOutput));
    ASSERT_TRUE(rich.has(ssg::app::Capability::KeyboardProtocol));
    ASSERT_TRUE(rich.has(ssg::app::Capability::ClipboardWrite));
    ASSERT_FALSE(rich.probing());  // The fence closed the window.

    // A terminal that answers only the fence: everything else is absent, and
    // nothing waited to find that out.
    ssg::app::TerminalCapabilities plain{fakeEnvironment({})};
    (void)plain.beginProbe();
    plain.observeReply("\x1b[?62;22c");
    for (auto const capability : ssg::app::kAllCapabilities) {
        ASSERT_FALSE(plain.has(capability));
    }
    ASSERT_FALSE(plain.probing());

    // A terminal that answers nothing at all: still absent, still not waiting.
    ssg::app::TerminalCapabilities silent{fakeEnvironment({})};
    (void)silent.beginProbe();
    silent.endProbe();
    for (auto const capability : ssg::app::kAllCapabilities) {
        ASSERT_FALSE(silent.has(capability));
    }

    // A terminal that knows mode 2026 but reports it unrecognized (state 0), and
    // one that recognizes it but can never enable it (state 4, permanently
    // reset), are both absent.  States 1/2/3 are usable.
    for (auto const* const unusable : {"\x1b[?2026;0$y", "\x1b[?2026;4$y"}) {
        ssg::app::TerminalCapabilities capabilities{fakeEnvironment({})};
        (void)capabilities.beginProbe();
        capabilities.observeReply(unusable);
        ASSERT_FALSE(capabilities.has(ssg::app::Capability::SynchronizedOutput));
    }
    for (auto const* const usable :
         {"\x1b[?2026;1$y", "\x1b[?2026;2$y", "\x1b[?2026;3$y"}) {
        ssg::app::TerminalCapabilities capabilities{fakeEnvironment({})};
        (void)capabilities.beginProbe();
        capabilities.observeReply(usable);
        ASSERT_TRUE(capabilities.has(ssg::app::Capability::SynchronizedOutput));
    }
}

// Re-probing asks a terminal that may not be the one that answered last time --
// a resumed session, a reattached multiplexer.  An answer from the old terminal
// must not survive the fence that is supposed to be able to retire it.
TEST(reprobingDoesNotCarryStaleAnswersForward) {
    ssg::app::TerminalCapabilities capabilities{fakeEnvironment({})};
    (void)capabilities.beginProbe();
    capabilities.observeReply("\x1b[?2026;2$y");
    capabilities.observeReply("\x1b[?1u");
    capabilities.observeReply("\x1b[?62;4;52c");
    for (auto const capability : ssg::app::kAllCapabilities) {
        ASSERT_TRUE(capabilities.has(capability));
    }

    // The new terminal answers only the fence: every previous answer is retired.
    (void)capabilities.beginProbe();
    capabilities.observeReply("\x1b[?62;22c");
    for (auto const capability : ssg::app::kAllCapabilities) {
        ASSERT_FALSE(capabilities.has(capability));
    }
}

// Oracle (the probe window): a reply shape arriving after the fence -- pasted by
// the user, or emitted by some protocol adopted later -- must not reconfigure the
// editor.  Consumption is unconditional; belief is not.
TEST(aReplyShapeAfterTheFenceIsNotACapabilityAnswer) {
    ssg::app::TerminalCapabilities capabilities{fakeEnvironment({})};
    (void)capabilities.beginProbe();
    capabilities.observeReply("\x1b[?62;22c");  // The fence closes the window.
    capabilities.observeReply("\x1b[?2026;2$y");
    capabilities.observeReply("\x1b[?1u");
    ASSERT_FALSE(capabilities.has(ssg::app::Capability::SynchronizedOutput));
    ASSERT_FALSE(capabilities.has(ssg::app::Capability::KeyboardProtocol));

    // Nor before the queries were ever written.
    ssg::app::TerminalCapabilities unprobed{fakeEnvironment({})};
    unprobed.observeReply("\x1b[?1u");
    ASSERT_FALSE(unprobed.has(ssg::app::Capability::KeyboardProtocol));
}

// Oracle (precedence): the override exists because terminals lie, so it has to
// beat the terminal's own answer in both directions.
TEST(anOverrideBeatsTheTerminalsOwnAnswer) {
    ssg::app::TerminalCapabilities forcedOff{
        fakeEnvironment({{"SSG_TERM_SYNCHRONIZED_OUTPUT", "off"}})};
    (void)forcedOff.beginProbe();
    forcedOff.observeReply("\x1b[?2026;2$y");  // The terminal says yes.
    ASSERT_FALSE(forcedOff.has(ssg::app::Capability::SynchronizedOutput));

    ssg::app::TerminalCapabilities forcedOn{
        fakeEnvironment({{"SSG_TERM_KEYBOARD_PROTOCOL", "1"}})};
    (void)forcedOn.beginProbe();
    forcedOn.observeReply("\x1b[?62;22c");  // The terminal never answered.
    ASSERT_TRUE(forcedOn.has(ssg::app::Capability::KeyboardProtocol));

    // An unparseable override defers to the terminal rather than forcing a guess.
    ssg::app::TerminalCapabilities garbage{
        fakeEnvironment({{"SSG_TERM_CLIPBOARD_WRITE", "perhaps"}})};
    (void)garbage.beginProbe();
    garbage.observeReply("\x1b[?62;4;52c");
    ASSERT_TRUE(garbage.has(ssg::app::Capability::ClipboardWrite));

    // Color depth is resolved by the same object, from the same environment.
    ssg::app::TerminalCapabilities colored{
        fakeEnvironment({{"COLORTERM", "truecolor"}, {"TERM", "xterm"}})};
    ASSERT_TRUE(colored.colorDepth() == ssg::ColorDepth::Truecolor);
    ssg::app::TerminalCapabilities dumb{fakeEnvironment({{"TERM", "dumb"}})};
    ASSERT_TRUE(dumb.colorDepth() == ssg::ColorDepth::Ansi16);
}

// Every capability must be reachable by an override, or a user hitting a
// rendering bug in one of them has no escape hatch.  Enumerated from the enum so
// a capability added later cannot quietly skip its override.
TEST(everyCapabilityHasAWorkingOverride) {
    for (auto const capability : ssg::app::kAllCapabilities) {
        std::string variable = "SSG_TERM_";
        for (char const ch : ssg::app::capability_name(capability)) {
            variable.push_back(
                static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
        ssg::app::TerminalCapabilities forced{fakeEnvironment({{variable, "on"}})};
        ASSERT_TRUE(forced.has(capability));
        ssg::app::TerminalCapabilities suppressed{
            fakeEnvironment({{variable, "off"}})};
        ASSERT_FALSE(suppressed.has(capability));
    }
}

// The queries must be answerable by the parser that reads their replies, and the
// fence must be written last or it cannot fence anything.
TEST(theProbeAsksOnlyQuestionsItCanUnderstand) {
    ssg::app::TerminalCapabilities capabilities{fakeEnvironment({})};
    ASSERT_FALSE(capabilities.probing());
    auto const queries = capabilities.beginProbe();
    ASSERT_TRUE(capabilities.probing());

    // DA1 is last, so every speculative answer precedes the fence.
    ASSERT_TRUE(queries.ends_with("\x1b[c"));
    // No query may use an introducer whose reply the decoder does not parse.
    ASSERT_TRUE(queries.find("\x1b]") == std::string::npos);
    ASSERT_TRUE(queries.find("\x1bP") == std::string::npos);

    // Each query is a sequence the decoder consumes whole, so writing one cannot
    // make the terminal echo bytes SSG would then treat as typing.
    std::size_t offset = 0;
    while (offset < queries.size()) {
        std::size_t consumed = 0;
        auto const decoded =
            ssg::app::decode_input(std::string_view{queries}.substr(offset), true,
                                   consumed);
        ASSERT_TRUE(decoded.status != ssg::app::DecodeStatus::incomplete);
        ASSERT_TRUE(consumed > 0);
        offset += consumed;
    }
}

// The probe window must close on the clock, at the moment a reply is CONSIDERED.
// Enforcing it from the event loop instead would always be a step behind: the
// loop wakes *because* bytes arrived, so a check at the top of the loop runs
// before the very reply it should have excluded, and a reply arriving long after
// startup would be believed.
TEST(aReplyArrivingAfterTheWindowExpiresIsNotBelieved) {
    auto now = std::chrono::steady_clock::time_point{};
    auto const clock = [&now] { return now; };

    ssg::app::TerminalCapabilities capabilities{fakeEnvironment({}), clock};
    (void)capabilities.beginProbe();
    ASSERT_TRUE(capabilities.probing());

    // Just inside the window: still believed.
    now += ssg::app::TerminalCapabilities::kProbeWindow -
           std::chrono::milliseconds{1};
    ASSERT_TRUE(capabilities.probing());
    capabilities.observeReply("\x1b[?1u");
    ASSERT_TRUE(capabilities.has(ssg::app::Capability::KeyboardProtocol));

    // Past the window, with no loop having run in between: not believed.
    ssg::app::TerminalCapabilities late{fakeEnvironment({}), clock};
    (void)late.beginProbe();
    now += ssg::app::TerminalCapabilities::kProbeWindow +
           std::chrono::milliseconds{1};
    ASSERT_FALSE(late.probing());
    late.observeReply("\x1b[?1u");
    late.observeReply("\x1b[?2026;2$y");
    ASSERT_FALSE(late.has(ssg::app::Capability::KeyboardProtocol));
    ASSERT_FALSE(late.has(ssg::app::Capability::SynchronizedOutput));
}

// An override a user cannot read about is an escape hatch they will never find
// when a terminal renders a capability badly.  Enumerated from the enum, so a
// capability added later cannot ship undocumented.
TEST(configDocDocumentsEveryCapabilityOverride) {
    std::ifstream input{std::filesystem::path{SSG_TEST_SOURCE_DIR} / "doc" /
                        "config.md"};
    std::ostringstream contents;
    contents << input.rdbuf();
    auto const doc = contents.str();
    ASSERT_FALSE(doc.empty());
    for (auto const capability : ssg::app::kAllCapabilities) {
        std::string variable = "SSG_TERM_";
        for (char const ch : ssg::app::capability_name(capability)) {
            variable.push_back(
                static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
        ASSERT_TRUE(doc.find(variable) != std::string::npos);
    }
    // The diagnostic that makes the overrides actionable must be findable too.
    ASSERT_TRUE(doc.find("--capabilities") != std::string::npos);
    ASSERT_TRUE(doc.find("SSG_COLOR_DEPTH") != std::string::npos);
}

// The diagnostic must be able to explain a reported "no".  A reply that arrives
// after the fence is recorded but not believed, and the two must stay
// distinguishable -- they call for different fixes.
TEST(theProbeLogSeparatesWhatWasBelievedFromWhatArrivedLate) {
    ssg::app::TerminalCapabilities capabilities{fakeEnvironment({})};
    (void)capabilities.beginProbe();
    capabilities.observeReply("\x1b[?2026;2$y");
    capabilities.observeReply("\x1b[?62;22c");  // The fence closes the window.
    capabilities.observeReply("\x1b[?1u");      // Too late to count.

    auto const& log = capabilities.probeLog();
    ASSERT_EQ(log.believed.size(), std::size_t{2});
    ASSERT_EQ(log.believed.front(), std::string{"\x1b[?2026;2$y"});
    ASSERT_EQ(log.ignored.size(), std::size_t{1});
    ASSERT_EQ(log.ignored.front(), std::string{"\x1b[?1u"});
    ASSERT_FALSE(capabilities.has(ssg::app::Capability::KeyboardProtocol));

    // A terminal that says nothing leaves an empty log, which is what tells a
    // user "silent" rather than "misparsed".
    ssg::app::TerminalCapabilities silent{fakeEnvironment({})};
    (void)silent.beginProbe();
    ASSERT_TRUE(silent.probeLog().believed.empty());
    ASSERT_TRUE(silent.probeLog().ignored.empty());

    // Re-probing starts a fresh log rather than accumulating across probes.
    (void)capabilities.beginProbe();
    ASSERT_TRUE(capabilities.probeLog().believed.empty());
    ASSERT_TRUE(capabilities.probeLog().ignored.empty());
}

// Field-captured replies, kept verbatim because a hand-written reply would not
// have exercised a thirteen-parameter DA1 whose class code (61) sits where an
// extension could, nor a real terminal's choice of DECRPM state.
TEST(realTerminalRepliesResolveAsObserved) {
    // Windows Terminal 1.24 over ssh.
    ssg::app::TerminalCapabilities windowsTerminal{fakeEnvironment({})};
    (void)windowsTerminal.beginProbe();
    windowsTerminal.observeReply("\x1b[?2026;2$y");
    windowsTerminal.observeReply(
        "\x1b[?61;4;6;7;14;21;22;23;24;28;32;42;52c");
    ASSERT_TRUE(windowsTerminal.has(ssg::app::Capability::ClipboardWrite));
    // DECRPM state 2 is "reset": the terminal knows the mode, which is what was
    // asked.  Reading state 2 as unsupported would report no on a terminal that
    // demonstrably has it.
    ASSERT_TRUE(windowsTerminal.has(ssg::app::Capability::SynchronizedOutput));
    // It answered nothing about the keyboard protocol, so it does not have it
    // (1.24 predates the release that implements it).
    ASSERT_FALSE(windowsTerminal.has(ssg::app::Capability::KeyboardProtocol));
    ASSERT_TRUE(windowsTerminal.probeLog().ignored.empty());
    ASSERT_EQ(windowsTerminal.probeLog().believed.size(), std::size_t{2});

    // xterm.js: a minimal DA1 with no extensions, and no answer to either
    // speculative question.
    ssg::app::TerminalCapabilities xtermJs{fakeEnvironment({})};
    (void)xtermJs.beginProbe();
    xtermJs.observeReply("\x1b[?1;2c");
    for (auto const capability : ssg::app::kAllCapabilities) {
        ASSERT_FALSE(xtermJs.has(capability));
    }

    // The terminal class must not be mistaken for an extension: a terminal whose
    // class happens to be 52 advertises no clipboard by saying so.
    ssg::app::TerminalCapabilities classFiftyTwo{fakeEnvironment({})};
    (void)classFiftyTwo.beginProbe();
    classFiftyTwo.observeReply("\x1b[?52;1c");
    ASSERT_FALSE(classFiftyTwo.has(ssg::app::Capability::ClipboardWrite));
}

// While a scrollbar thumb is dragged the caret is not what the user is looking
// at, and a visible cursor lands wherever painting ended -- it flickers around
// the screen chasing each frame.  A frame asked to hide it must END hidden;
// every other frame must end visible, which is the balance the escape-discipline
// spec pins.
TEST(aDragFrameEndsWithTheCursorHiddenAndEveryOtherFrameShowsIt) {
    ssg::CellGrid grid;
    grid.size = {4, 2};
    grid.cells.resize(8);
    grid.caret = ssg::GridPosition{1, 1};

    auto const shown = ssg::app::encode_frame(grid, ssg::ColorDepth::Ansi16);
    auto const hidden =
        ssg::app::encode_frame(grid, ssg::ColorDepth::Ansi16, false);

    std::string const enter{ssg::app::kCursorHidden.enter};
    std::string const leave{ssg::app::kCursorHidden.leave};

    // A normal frame places the caret and then ends by showing the cursor.
    ASSERT_TRUE(shown.ends_with(leave));
    ASSERT_TRUE(shown.find("\x1b[2;2H") != std::string::npos);
    ASSERT_TRUE(shown.rfind(enter) < shown.rfind(leave));

    // A drag frame ends hidden, and does not place the caret at all -- placing it
    // is what would make the cursor appear at a spot the user is not looking at.
    ASSERT_TRUE(hidden.ends_with(enter));
    ASSERT_TRUE(hidden.find("\x1b[2;2H") == std::string::npos);
    ASSERT_TRUE(hidden.rfind(enter) > hidden.rfind(leave));
}

// Pasted bytes are CONTENT, not keystrokes.  Without bracketed paste a paste is
// indistinguishable from very fast typing: a newline in it fires whatever Enter
// is bound to, and an escape sequence in it is obeyed.
TEST(aBracketedPasteIsContentAndNeverKeys) {
    std::size_t consumed = 0;
    // A paste containing the two things that would otherwise be interpreted: a
    // newline, and an escape sequence.
    std::string const payload = "line one\nline two\x1b[A";
    std::string const wrapped = "\x1b[200~" + payload + "\x1b[201~";
    auto const decoded = ssg::app::decode_input(wrapped, true, consumed);
    ASSERT_TRUE(decoded.status == ssg::app::DecodeStatus::paste);
    ASSERT_EQ(decoded.text, payload);
    ASSERT_EQ(consumed, wrapped.size());
    // It is not a keypress, so nothing can route it to the keymap.
    ASSERT_TRUE(decoded.stroke.code == ssg::KeyCode::None);

    // An empty paste is still a paste, consumed whole.
    consumed = 0;
    auto const empty = ssg::app::decode_input("\x1b[200~\x1b[201~", true, consumed);
    ASSERT_TRUE(empty.status == ssg::app::DecodeStatus::paste);
    ASSERT_TRUE(empty.text.empty());
    ASSERT_EQ(consumed, std::size_t{12});

    // Every proper prefix waits rather than being half-consumed, so a paste
    // split across reads reassembles instead of leaking its head as text.
    for (std::size_t prefix = 2; prefix < wrapped.size(); ++prefix) {
        consumed = 99;
        auto const partial =
            ssg::app::decode_input(std::string_view{wrapped}.substr(0, prefix),
                                   false, consumed);
        ASSERT_TRUE(partial.status == ssg::app::DecodeStatus::incomplete);
        ASSERT_EQ(consumed, std::size_t{0});
    }

    // The mode is in kAllModes, so terminal teardown and the crash undo both
    // turn bracketed paste back off; a terminal left in it would wrap the
    // shell's pastes after ssg exits.
    bool listed = false;
    for (auto const& mode : ssg::app::kAllModes) {
        if (mode.enter == ssg::app::kBracketedPaste.enter) listed = true;
    }
    ASSERT_TRUE(listed);
}

// OSC 52 is how a remote editor reaches the LOCAL clipboard over SSH -- the one
// mechanism that crosses that gap.
TEST(clipboardWriteEncodesOsc52WithBase64) {
    // Base64 checked against known answers rather than against another encoder,
    // including both padding lengths, which is where an encoder goes wrong.
    ASSERT_EQ(ssg::app::encode_clipboard_write("hi"),
              std::string{"\x1b]52;c;aGk=\x1b\\"});
    ASSERT_EQ(ssg::app::encode_clipboard_write("abc"),
              std::string{"\x1b]52;c;YWJj\x1b\\"});
    ASSERT_EQ(ssg::app::encode_clipboard_write("a"),
              std::string{"\x1b]52;c;YQ==\x1b\\"});
    ASSERT_EQ(ssg::app::encode_clipboard_write(""),
              std::string{"\x1b]52;c;\x1b\\"});
    // Bytes above 0x7f survive: the payload is base64 of raw bytes, not of text
    // the encoder has opinions about.
    ASSERT_EQ(ssg::app::encode_clipboard_write("\xc3\xa9"),
              std::string{"\x1b]52;c;w6k=\x1b\\"});
}

// A diagnostic is a DECORATION: it underlines whatever the cell already shows
// without taking its colour, so an error under syntax-highlighted code is still
// readable as code.  Severity ranks, so an error is never hidden by a hint on
// the same cell.
TEST(diagnosticsUnderlineTheirCellsWithoutRecolouringThem) {
    ssg::CellGrid grid;
    grid.size = {6, 1};
    grid.cells.resize(6);
    for (int x = 0; x < 6; ++x) {
        grid.cells[static_cast<std::size_t>(x)].text = "x";
        grid.cells[static_cast<std::size_t>(x)].foreground = 2;
    }
    grid.cells[1].underline = ssg::CellUnderline::Error;
    grid.cells[2].underline = ssg::CellUnderline::Warning;
    grid.cells[3].underline = ssg::CellUnderline::Info;

    auto const frame =
        ssg::app::encode_ansi_frame(grid, ssg::ColorDepth::Truecolor);
    // Curly underline, coloured independently of the text.
    ASSERT_TRUE(frame.find("\x1b[4:3m\x1b[58;5;1m") != std::string::npos);
    ASSERT_TRUE(frame.find("\x1b[4:3m\x1b[58;5;3m") != std::string::npos);
    ASSERT_TRUE(frame.find("\x1b[4:2m\x1b[58;5;4m") != std::string::npos);
    // And turned back off, or the underline would run to the end of the row.
    ASSERT_TRUE(frame.find("\x1b[4:0m\x1b[59m") != std::string::npos);

    // The text's own colour is unchanged: a diagnostic decorates, it does not
    // recolour, so highlighted code stays readable underneath.  Compared as the
    // SET of colours used -- underlining breaks the run, so the encoder re-emits
    // the same colour more often, which is not a change in what is shown.
    ssg::CellGrid undecorated = grid;
    for (auto& cell : undecorated.cells) cell.underline = ssg::CellUnderline::None;
    auto const bare =
        ssg::app::encode_ansi_frame(undecorated, ssg::ColorDepth::Truecolor);
    auto const foregroundColours = [](std::string const& text) {
        std::set<std::string> colours;
        for (std::size_t at = text.find("\x1b[38;2;"); at != std::string::npos;
             at = text.find("\x1b[38;2;", at + 1)) {
            auto const end = text.find('m', at);
            if (end != std::string::npos) {
                colours.insert(text.substr(at, end - at + 1));
            }
        }
        return colours;
    };
    ASSERT_TRUE(foregroundColours(frame) == foregroundColours(bare));
    // And the underline sequences are the whole of the difference.
    ASSERT_TRUE(frame.size() > bare.size());
}

// A hyperlink must be CLOSED, or the terminal keeps making everything after it
// clickable -- worse than not linking at all.
TEST(hyperlinkRunsAreOpenedAndAlwaysClosed) {
    ssg::CellGrid grid;
    grid.size = {8, 1};
    grid.cells.resize(8);
    for (int x = 0; x < 8; ++x) {
        grid.cells[static_cast<std::size_t>(x)].text = "x";
    }
    grid.hyperlinks.push_back({0, 2, 3, "https://example.com"});
    auto const frame =
        ssg::app::encode_ansi_frame(grid, ssg::ColorDepth::Truecolor);
    auto const open = frame.find("\x1b]8;;https://example.com\x1b\\");
    ASSERT_TRUE(open != std::string::npos);
    auto const close = frame.find("\x1b]8;;\x1b\\", open);
    ASSERT_TRUE(close != std::string::npos);
    // Exactly three cells between open and close.
    auto const between = frame.substr(
        open + std::string{"\x1b]8;;https://example.com\x1b\\"}.size(),
        close - open - std::string{"\x1b]8;;https://example.com\x1b\\"}.size());
    std::size_t glyphs = 0;
    for (char const byte : between) {
        if (byte == 'x') ++glyphs;
    }
    ASSERT_EQ(glyphs, std::size_t{3});

    // A run reaching the last column still closes: there is no cell after it to
    // close at, so the row's end must do it.
    ssg::CellGrid edge = grid;
    edge.hyperlinks.clear();
    edge.hyperlinks.push_back({0, 5, 3, "https://example.com"});
    auto const edgeFrame =
        ssg::app::encode_ansi_frame(edge, ssg::ColorDepth::Truecolor);
    auto const edgeOpen = edgeFrame.find("\x1b]8;;https://example.com\x1b\\");
    ASSERT_TRUE(edgeOpen != std::string::npos);
    ASSERT_TRUE(edgeFrame.find("\x1b]8;;\x1b\\", edgeOpen) != std::string::npos);

    // No links, no OSC 8 at all: a document without URLs pays nothing.
    ssg::CellGrid plain = grid;
    plain.hyperlinks.clear();
    auto const plainFrame =
        ssg::app::encode_ansi_frame(plain, ssg::ColorDepth::Truecolor);
    ASSERT_TRUE(plainFrame.find("\x1b]8;") == std::string::npos);
}

TEST(decodeInputPointerPressReleaseDrag) {
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
    auto wheelUp = ssg::app::decode_input("\x1b[<64;10;5M", true, consumed);
    ASSERT_TRUE(wheelUp.status == ssg::app::DecodeStatus::scroll);
    ASSERT_EQ(wheelUp.scroll, std::int64_t{-3});
    ASSERT_EQ(wheelUp.pointer.column, 9);
    ASSERT_EQ(wheelUp.pointer.row, 4);
}

TEST(decodeInputPointerCarriesAltModifier) {
    std::size_t consumed = 0;

    // Alt-left press: Cb = left(0) | Alt(8) = 8, final 'M', at SGR (3,4) ->
    // grid (2,3). Bit3 surfaces as PointerEvent::alt.
    auto altPress = ssg::app::decode_input("\x1b[<8;3;4M", true, consumed);
    ASSERT_TRUE(altPress.status == ssg::app::DecodeStatus::pointer);
    ASSERT_EQ(altPress.pointer.column, 2);
    ASSERT_EQ(altPress.pointer.row, 3);
    ASSERT_TRUE(altPress.pointer.button == ssg::app::PointerButton::left);
    ASSERT_TRUE(altPress.pointer.kind == ssg::app::PointerKind::press);
    ASSERT_TRUE(altPress.pointer.alt);

    // Same press without bit3: alt is false.
    auto plainPress = ssg::app::decode_input("\x1b[<0;3;4M", true, consumed);
    ASSERT_TRUE(plainPress.status == ssg::app::DecodeStatus::pointer);
    ASSERT_FALSE(plainPress.pointer.alt);

    // Alt-drag: Cb = motion(32) | Alt(8) = 40, final 'M'. kind is drag, alt set.
    auto altDrag = ssg::app::decode_input("\x1b[<40;3;4M", true, consumed);
    ASSERT_TRUE(altDrag.status == ssg::app::DecodeStatus::pointer);
    ASSERT_TRUE(altDrag.pointer.kind == ssg::app::PointerKind::drag);
    ASSERT_TRUE(altDrag.pointer.alt);
}

TEST(decodeInputPointerSplitReadsAreIncomplete) {
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

TEST(routePointerLeftPressOnEditorPlacesCaret) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    hit.byteOffset = 3;
    ssg::app::PointerTargets targets;
    targets.document_position =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};

    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::press, false, false,
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

TEST(routePointerIgnoresNonEditorAndNonLeft) {
    ssg::app::PointerTargets const empty;

    // A press on nothing (out of bounds / chrome) dispatches no command.
    ssg::RegionHit noneHit;  // region defaults to HitRegion::none
    auto nonePlan = ssg::app::route_pointer(
        noneHit, ssg::app::PointerButton::left, ssg::app::PointerKind::press, false,
        false, std::nullopt, empty);
    ASSERT_TRUE(nonePlan.commands.empty());
    ASSERT_FALSE(nonePlan.begins_drag);

    // A left press on a non-editor region (e.g. the panel) is not handled by
    // M8-C: no editor command, no drag.
    ssg::RegionHit panelHit;
    panelHit.region = ssg::HitRegion::Panel;
    auto panelPlan = ssg::app::route_pointer(
        panelHit, ssg::app::PointerButton::left, ssg::app::PointerKind::press, false,
        false, std::nullopt, empty);
    ASSERT_TRUE(panelPlan.commands.empty());

    // A right/middle press on the editor is a no-op in M8.
    ssg::RegionHit editorHit;
    editorHit.region = ssg::HitRegion::Editor;
    ssg::app::PointerTargets targets;
    targets.document_position =
        ssg::DocumentPosition{ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
    auto rightPlan = ssg::app::route_pointer(
        editorHit, ssg::app::PointerButton::right, ssg::app::PointerKind::press, false,
        false, std::nullopt, targets);
    ASSERT_TRUE(rightPlan.commands.empty());

    // A left press on the editor with no resolved position (e.g. a blank cell)
    // dispatches nothing.
    auto unresolved = ssg::app::route_pointer(
        editorHit, ssg::app::PointerButton::left, ssg::app::PointerKind::press, false,
        false, std::nullopt, empty);
    ASSERT_TRUE(unresolved.commands.empty());
    ASSERT_FALSE(unresolved.begins_drag);
}

TEST(decodeInputPointerRejectsMalformedButTerminatedPayloads) {
    std::size_t consumed = 0;
    // Empty Cb, empty Cx, empty Cy, and non-digit Cy each terminate with M/m but
    // are malformed: they are consumed and dropped (none), never dispatched as a
    // real pointer event at (0,0).
    for (std::string_view malformed :
         {"\x1b[<;1;1M", "\x1b[<0;;1M", "\x1b[<0;1;M", "\x1b[<0;1M",
          "\x1b[<0;1;5;9M"}) {
        consumed = 0;
        auto decoded = ssg::app::decode_input(malformed, true, consumed);
        ASSERT_TRUE(decoded.status == ssg::app::DecodeStatus::none);
        ASSERT_EQ(consumed, malformed.size());  // consumed so the loop advances
    }

    // A non-digit in a parameter position is a CSI *final* byte (0x40-0x7E), so
    // the sequence ends there under the grammar and the trailing 'M' is an
    // ordinary printable that follows it.  The extent of a sequence is decided by
    // the grammar rather than by hunting for the byte we hoped to find, which is
    // what bounds the scan (INV-decode-terminates).
    std::string_view const earlyFinal = "\x1b[<0;1;xM";
    consumed = 0;
    auto const decoded = ssg::app::decode_input(earlyFinal, true, consumed);
    ASSERT_TRUE(decoded.status == ssg::app::DecodeStatus::none);
    ASSERT_EQ(consumed, earlyFinal.size() - 1);
}

TEST(routePointerDragExtendsSelectionFromAnchor) {
    auto const anchor =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    hit.byteOffset = 10;
    ssg::app::PointerTargets targets;
    auto const active =
        ssg::DocumentPosition{ssg::ByteOffset{10}, ssg::LineIndex{1}, ssg::CellIndex{2}};
    targets.document_position = active;

    // A drag while dragging with an anchor -> select.set_range spanning the two.
    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::drag, false, true,
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

TEST(routePointerFieldHitDispatchesPublishedCommandGenerically) {
    ssg::app::PointerTargets targets;
    targets.field_command_id = std::string{"panel.show_files"};
    for (auto region :
         {ssg::HitRegion::HeaderField, ssg::HitRegion::FooterField}) {
        ssg::RegionHit hit;
        hit.region = region;
        auto plan = ssg::app::route_pointer(
            hit, ssg::app::PointerButton::left, ssg::app::PointerKind::press, false,
            false, std::nullopt, targets);
        ASSERT_EQ(plan.commands.size(), std::size_t{1});
        if (plan.commands.size() == 1) {
            ASSERT_EQ(plan.commands[0].command_id,
                      std::string{"panel.show_files"});
        }
        ASSERT_FALSE(plan.begins_drag);
        ASSERT_FALSE(plan.ends_drag);
    }
}

TEST(routePointerDragWithoutAnchorOrTargetIsANoOp) {
    auto const anchor =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    ssg::RegionHit editorHit;
    editorHit.region = ssg::HitRegion::Editor;
    ssg::app::PointerTargets targets;
    targets.document_position =
        ssg::DocumentPosition{ssg::ByteOffset{5}, ssg::LineIndex{0}, ssg::CellIndex{5}};

    // Not dragging (no prior press) -> no command even over the editor.
    auto notDragging = ssg::app::route_pointer(
        editorHit, ssg::app::PointerButton::left, ssg::app::PointerKind::drag, false,
        false, anchor, targets);
    ASSERT_TRUE(notDragging.commands.empty());

    // Dragging but the pointer is over a cell with no document target (past a
    // short line's end / beyond the viewport edge) -> no command, selection holds.
    ssg::app::PointerTargets const noTarget;
    auto offContent = ssg::app::route_pointer(
        editorHit, ssg::app::PointerButton::left, ssg::app::PointerKind::drag, false,
        true, anchor, noTarget);
    ASSERT_TRUE(offContent.commands.empty());

    // Dragging over a non-editor region (e.g. the panel) -> no command.
    ssg::RegionHit panelHit;
    panelHit.region = ssg::HitRegion::Panel;
    auto offEditor = ssg::app::route_pointer(
        panelHit, ssg::app::PointerButton::left, ssg::app::PointerKind::drag, false,
        true, anchor, targets);
    ASSERT_TRUE(offEditor.commands.empty());
}

TEST(routePointerReleaseEndsDragWithoutACommand) {
    ssg::RegionHit editorHit;
    editorHit.region = ssg::HitRegion::Editor;
    ssg::app::PointerTargets targets;
    targets.document_position =
        ssg::DocumentPosition{ssg::ByteOffset{5}, ssg::LineIndex{0}, ssg::CellIndex{5}};

    // Release while dragging ends the drag and dispatches nothing.
    auto ending = ssg::app::route_pointer(
        editorHit, ssg::app::PointerButton::left, ssg::app::PointerKind::release, false,
        true, targets.document_position, targets);
    ASSERT_TRUE(ending.commands.empty());
    ASSERT_TRUE(ending.ends_drag);

    // A release when not dragging is inert.
    auto stray = ssg::app::route_pointer(
        editorHit, ssg::app::PointerButton::left, ssg::app::PointerKind::release, false,
        false, std::nullopt, targets);
    ASSERT_TRUE(stray.commands.empty());
    ASSERT_FALSE(stray.ends_drag);
}

TEST(routePointerAltPressAddsCollapsedCaret) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    hit.byteOffset = 3;
    ssg::app::PointerTargets targets;
    auto const pos =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    targets.document_position = pos;

    // Alt press adds a collapsed caret (add_range), not cursor.set_position, and
    // still begins the drag.
    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::press, true, false,
                                        std::nullopt, targets);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"select.add_range"});
        auto const* args = std::any_cast<ssg::SelectionCommandArguments>(
            &plan.commands[0].payload);
        ASSERT_TRUE(args != nullptr);
        if (args) {
            ASSERT_FALSE(args->position.has_value());
            ASSERT_TRUE(args->selection.has_value());
            if (args->selection) {
                ASSERT_EQ(args->selection->anchor, pos);
                ASSERT_EQ(args->selection->active, pos);
            }
            ASSERT_TRUE(args->selections.empty());
        }
    }
    ASSERT_TRUE(plan.begins_drag);

    // The same press without Alt keeps cursor.set_position.
    auto plain = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                         ssg::app::PointerKind::press, false, false,
                                         std::nullopt, targets);
    ASSERT_EQ(plain.commands.size(), std::size_t{1});
    if (plain.commands.size() == 1) {
        ASSERT_EQ(plain.commands[0].command_id, std::string{"cursor.set_position"});
    }
}

// Alt+click on an existing caret/selection
// REMOVES it (Sublime toggle) via select.set_ranges of the baseline minus the
// hit, and starts no drag; a click on empty space still ADDS; the sole caret is
// never removed. These are pure-router hand cases.
namespace {
ssg::DocumentPosition posAt(std::uint64_t byte, std::uint32_t line,
                            std::uint32_t cell) {
    return ssg::DocumentPosition{ssg::ByteOffset{byte}, ssg::LineIndex{line},
                                 ssg::CellIndex{cell}};
}
ssg::app::PointerDispatch altPressAt(ssg::DocumentPosition p,
                                     std::vector<ssg::Selection> baseline) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    hit.byteOffset = static_cast<int>(p.byteOffset.value());
    ssg::app::PointerTargets targets;
    targets.document_position = p;
    return ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                   ssg::app::PointerKind::press, true, false,
                                   std::nullopt, targets, std::move(baseline));
}
const ssg::SelectionCommandArguments* argsOf(
    ssg::app::PointerDispatch const& plan) {
    if (plan.commands.size() != 1) return nullptr;
    return std::any_cast<ssg::SelectionCommandArguments>(
        &plan.commands[0].payload);
}
}  // namespace

TEST(altClickRemovesOneOfTwoCarets) {
    auto const c3 = posAt(3, 0, 3);
    auto const c7 = posAt(7, 0, 7);
    std::vector<ssg::Selection> baseline{{c3, c3}, {c7, c7}};

    // Alt-click the caret at 3 -> set_ranges with only the caret at 7, no drag.
    auto plan = altPressAt(c3, baseline);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"select.set_ranges"});
        auto const* args = argsOf(plan);
        ASSERT_TRUE(args != nullptr);
        if (args) {
            ASSERT_EQ(args->selections.size(), std::size_t{1});
            if (args->selections.size() == 1)
                ASSERT_EQ(args->selections[0], (ssg::Selection{c7, c7}));
        }
    }
    ASSERT_FALSE(plan.begins_drag);
}

TEST(altClickInsideARangeRemovesThatRange) {
    auto const c1 = posAt(1, 0, 1);
    auto const rlo = posAt(5, 0, 5);
    auto const rhi = posAt(10, 0, 10);
    std::vector<ssg::Selection> baseline{{c1, c1}, {rlo, rhi}};

    // Click at byte 7 (inside [5,10)) removes the range, keeps the caret at 1.
    auto plan = altPressAt(posAt(7, 0, 7), baseline);
    auto const* args = argsOf(plan);
    ASSERT_TRUE(args != nullptr);
    if (args) {
        ASSERT_EQ(args->selections.size(), std::size_t{1});
        if (args->selections.size() == 1)
            ASSERT_EQ(args->selections[0], (ssg::Selection{c1, c1}));
    }
    ASSERT_FALSE(plan.begins_drag);
}

TEST(altClickOnEmptySpaceAddsACaret) {
    auto const c3 = posAt(3, 0, 3);
    std::vector<ssg::Selection> baseline{{c3, c3}, {posAt(7, 0, 7), posAt(7, 0, 7)}};
    // Byte 5 is on neither -> add_range, begins_drag stays true.
    auto plan = altPressAt(posAt(5, 0, 5), baseline);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1)
        ASSERT_EQ(plan.commands[0].command_id, std::string{"select.add_range"});
    ASSERT_TRUE(plan.begins_drag);
}

TEST(altClickOnTheSoleCaretIsANoOpAdd) {
    auto const c3 = posAt(3, 0, 3);
    std::vector<ssg::Selection> baseline{{c3, c3}};
    // Size == 1: never removes; falls through to the (self-deduping) add path.
    auto plan = altPressAt(c3, baseline);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1)
        ASSERT_EQ(plan.commands[0].command_id, std::string{"select.add_range"});
    ASSERT_TRUE(plan.begins_drag);
}

TEST(altClickRangeBoundaryIsExclusiveAtTheUpperEnd) {
    auto const c1 = posAt(1, 0, 1);
    auto const rlo = posAt(5, 0, 5);
    auto const rhi = posAt(10, 0, 10);
    std::vector<ssg::Selection> baseline{{c1, c1}, {rlo, rhi}};

    // P == hi (10) is NOT inside [5,10) -> no hit -> add (not set_ranges).
    auto atUpper = altPressAt(posAt(10, 0, 10), baseline);
    ASSERT_EQ(atUpper.commands.size(), std::size_t{1});
    if (atUpper.commands.size() == 1)
        ASSERT_EQ(atUpper.commands[0].command_id, std::string{"select.add_range"});

    // P == lo (5) IS inside -> removes the range.
    auto atLower = altPressAt(posAt(5, 0, 5), baseline);
    auto const* args = argsOf(atLower);
    ASSERT_TRUE(args != nullptr);
    if (args) {
        ASSERT_EQ(args->selections.size(), std::size_t{1});
        if (args->selections.size() == 1)
            ASSERT_EQ(args->selections[0], (ssg::Selection{c1, c1}));
    }
}

TEST(altClickPrefersACaretOverARangeSharingItsLowerBound) {
    // A caret [5,5] coexists with a range [5,10) (strict-< merge keeps both).
    // The baseline is in normalized order (caret before range), so a click at 5
    // removes the CARET, leaving the range.
    auto const caret5 = posAt(5, 0, 5);
    auto const rlo = posAt(5, 0, 5);
    auto const rhi = posAt(10, 0, 10);
    std::vector<ssg::Selection> baseline{{caret5, caret5}, {rlo, rhi}};

    auto plan = altPressAt(posAt(5, 0, 5), baseline);
    auto const* args = argsOf(plan);
    ASSERT_TRUE(args != nullptr);
    if (args) {
        ASSERT_EQ(args->selections.size(), std::size_t{1});
        if (args->selections.size() == 1)
            ASSERT_EQ(args->selections[0], (ssg::Selection{rlo, rhi}));
    }
}

TEST(altDoubleClickStillSelectsAWordNeverRemoves) {
    // The app routes ANY double-click (Alt or not) through double_click_dispatch,
    // which has no Alt parameter and only ever selects a word -- so the remove
    // path (which lives solely in route_pointer's Alt-press branch) is
    // unreachable for a double-click. Pin that the double-click seam yields word
    // selection and never a select.set_ranges/add_range, guarding the app-loop
    // short-circuit against a future refactor.
    auto const doubled = ssg::app::double_click_dispatch(posAt(5, 0, 5));
    ASSERT_EQ(doubled.commands.size(), std::size_t{1});
    if (doubled.commands.size() == 1) {
        ASSERT_EQ(doubled.commands[0].command_id,
                  std::string{"select.word_at_position"});
        ASSERT_NE(doubled.commands[0].command_id, std::string{"select.set_ranges"});
    }
    ASSERT_FALSE(doubled.begins_drag);
}

// End-to-end: the REAL router plan dispatched into a REAL runtime. Two carets,
// Alt-click one, and exactly the un-clicked caret survives.
TEST(altClickRemoveEndToEndLeavesTheSurvivingCaret) {
    auto root = fs::temp_directory_path() / "ssg-altremove-e2e";
    fs::remove_all(root);
    fs::create_directories(root / "workspace");
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "f.txt"} << "abcdefghij\n";

    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1}, {"file.open", runtime.revision(),
                                                 std::string{"f.txt"}})
                    .accepted());

    auto const doc = runtime.activeDocumentText();
    auto const p2 = ssg::SelectionNavigator::resolvePosition(doc, ssg::ByteOffset{2});
    auto const p7 = ssg::SelectionNavigator::resolvePosition(doc, ssg::ByteOffset{7});
    ASSERT_TRUE(p2.has_value() && p7.has_value());
    if (!p2 || !p7) return;

    // Two carets: one at 2, one at 7.
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"cursor.set_position", runtime.revision(),
                               ssg::SelectionCommandArguments{p2, std::nullopt}})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"select.add_range", runtime.revision(),
                               ssg::SelectionCommandArguments{
                                   std::nullopt, ssg::Selection{*p7, *p7}}})
                    .accepted());

    auto beforeSnap = runtime.snapshot(ssg::ClientId{1}, {80, 12});
    ASSERT_TRUE(beforeSnap.has_value());
    if (!beforeSnap) return;
    auto const& items = beforeSnap->sections().selection.items();
    ASSERT_EQ(items.size(), std::size_t{2});
    std::vector<ssg::Selection> baseline(items.begin(), items.end());

    // Route a REAL Alt-press on the caret at 2 and dispatch the plan.
    auto plan = altPressAt(*p2, baseline);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    for (auto const& command : plan.commands) {
        ASSERT_TRUE(
            runtime.dispatch(ssg::ClientId{1},
                             {command.command_id, runtime.revision(),
                              std::any_cast<ssg::SelectionCommandArguments>(
                                  command.payload)})
                .accepted());
    }

    auto afterSnap = runtime.snapshot(ssg::ClientId{1}, {80, 12});
    ASSERT_TRUE(afterSnap.has_value());
    if (!afterSnap) return;
    auto const& survivors = afterSnap->sections().selection.items();
    ASSERT_EQ(survivors.size(), std::size_t{1});
    if (survivors.size() == 1) {
        ASSERT_EQ(survivors[0], (ssg::Selection{*p7, *p7}));  // the un-clicked one
    }
}

TEST(routePointerAltDragSetsRangesFromBaseline) {
    auto const anchor =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    auto const active =
        ssg::DocumentPosition{ssg::ByteOffset{10}, ssg::LineIndex{1}, ssg::CellIndex{2}};
    auto const baselineCaret =
        ssg::DocumentPosition{ssg::ByteOffset{1}, ssg::LineIndex{0}, ssg::CellIndex{1}};
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    ssg::app::PointerTargets targets;
    targets.document_position = active;
    std::vector<ssg::Selection> const baseline{
        ssg::Selection{baselineCaret, baselineCaret}};

    // Alt drag rebuilds the whole set: baseline + the dragged range.
    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::drag, true, true,
                                        anchor, targets, baseline);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"select.set_ranges"});
        auto const* args = std::any_cast<ssg::SelectionCommandArguments>(
            &plan.commands[0].payload);
        ASSERT_TRUE(args != nullptr);
        if (args) {
            ASSERT_FALSE(args->selection.has_value());
            ASSERT_EQ(args->selections.size(), std::size_t{2});
            if (args->selections.size() == 2) {
                ASSERT_EQ(args->selections[0].anchor, baselineCaret);
                ASSERT_EQ(args->selections[0].active, baselineCaret);
                ASSERT_EQ(args->selections[1].anchor, anchor);
                ASSERT_EQ(args->selections[1].active, active);
            }
        }
    }

    // A no-Alt drag keeps select.set_range (unchanged single-selection path).
    auto plain = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                         ssg::app::PointerKind::drag, false, true,
                                         anchor, targets, baseline);
    ASSERT_EQ(plain.commands.size(), std::size_t{1});
    if (plain.commands.size() == 1) {
        ASSERT_EQ(plain.commands[0].command_id, std::string{"select.set_range"});
    }
}

TEST(routePointerAltDragIgnoresPerMotionModifierBit) {
    // The app feeds the router the ESTABLISHED alt for a drag, so the router
    // routes to select.set_ranges whenever alt is true regardless of the raw
    // motion bit: a mid-drag modifier drop still extends the multi-cursor set.
    auto const anchor =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    auto const active =
        ssg::DocumentPosition{ssg::ByteOffset{6}, ssg::LineIndex{0}, ssg::CellIndex{6}};
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    ssg::app::PointerTargets targets;
    targets.document_position = active;
    std::vector<ssg::Selection> const baseline{
        ssg::Selection{anchor, anchor}};

    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::drag, true, true,
                                        anchor, targets, baseline);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"select.set_ranges"});
    }
    // Alt release ends the drag and dispatches nothing.
    auto release = ssg::app::route_pointer(
        hit, ssg::app::PointerButton::left, ssg::app::PointerKind::release, true,
        true, anchor, targets, baseline);
    ASSERT_TRUE(release.commands.empty());
    ASSERT_TRUE(release.ends_drag);
}

TEST(routePointerEditorScrollbarScrollsToFraction) {
    // A press or drag on the editor gutter scrolls to the fraction hit_test
    // reported, independent of the selection drag state. The bottom of the
    // gutter reports numerator == denominator (-> maximum_first_row); the top
    // reports numerator 0 (-> first_row 0).
    auto scrollArgs = [](ssg::app::PointerDispatch const& plan)
        -> ssg::ScrollFractionArguments const* {
        if (plan.commands.size() != 1) return nullptr;
        if (plan.commands[0].command_id != "view.scroll_to_fraction") return nullptr;
        return std::any_cast<ssg::ScrollFractionArguments>(&plan.commands[0].payload);
    };

    ssg::RegionHit bottom;
    bottom.region = ssg::HitRegion::EditorScrollbar;
    bottom.scrollNumerator = 7;
    bottom.scrollDenominator = 7;
    ssg::app::PointerTargets const empty;

    for (auto kind : {ssg::app::PointerKind::press, ssg::app::PointerKind::drag}) {
        auto plan = ssg::app::route_pointer(
            bottom, ssg::app::PointerButton::left, kind, false, false, std::nullopt, empty);
        auto const* args = scrollArgs(plan);
        ASSERT_TRUE(args != nullptr);
        if (args) {
            ASSERT_EQ(args->numerator, std::uint32_t{7});
            ASSERT_EQ(args->denominator, std::uint32_t{7});
        }
        ASSERT_FALSE(plan.begins_drag);
        ASSERT_FALSE(plan.ends_drag);
    }

    ssg::RegionHit top;
    top.region = ssg::HitRegion::EditorScrollbar;
    top.scrollNumerator = 0;
    top.scrollDenominator = 7;
    auto topPlan = ssg::app::route_pointer(
        top, ssg::app::PointerButton::left, ssg::app::PointerKind::press, false, false,
        std::nullopt, empty);
    auto const* topArgs = scrollArgs(topPlan);
    ASSERT_TRUE(topArgs != nullptr);
    if (topArgs) {
        ASSERT_EQ(topArgs->numerator, std::uint32_t{0});
        ASSERT_EQ(topArgs->denominator, std::uint32_t{7});
    }

    // A mid-drag onto the editor gutter scrolls even while a selection drag is
    // active; the scrollbar path does not consult the drag anchor.
    auto const anchor =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    auto mid = ssg::app::route_pointer(bottom, ssg::app::PointerButton::left,
                                       ssg::app::PointerKind::drag, false, true, anchor,
                                       empty);
    ASSERT_TRUE(scrollArgs(mid) != nullptr);
}

// S3: EVERY scrollable surface's gutter answers press AND drag. This replaces a
// test that asserted the panel and picker gutters were no-ops -- that pinned the
// defect the user reported ("the bar scrollbar has no mouse interactivity") as
// intended behavior. Driven from the catalog, so a surface listed there without
// routing fails here rather than being silently inert.
TEST(everyScrollableGutterAnswersPressAndDrag) {
    ssg::app::PointerTargets const empty;
    std::size_t checked = 0;

    for (auto const& descriptor : ssg::app::scrollable_regions()) {
        ssg::RegionHit hit;
        hit.region = descriptor.scrollbar;
        hit.scrollNumerator = 3;
        hit.scrollDenominator = 4;

        for (auto kind : {ssg::app::PointerKind::press,
                          ssg::app::PointerKind::drag}) {
            auto plan = ssg::app::route_pointer(
                hit, ssg::app::PointerButton::left, kind, false, false, std::nullopt,
                empty);

            if (descriptor.scrollCommand.empty()) {
                // A client-owned surface must NOT emit a command (S-I5): its
                // scroll would otherwise round-trip on a latency-critical path.
                ASSERT_TRUE(plan.commands.empty());
                ASSERT_TRUE(plan.client_scroll.has_value());
                if (plan.client_scroll) {
                    ASSERT_EQ(plan.client_scroll->numerator, std::uint32_t{3});
                    ASSERT_EQ(plan.client_scroll->denominator, std::uint32_t{4});
                    ASSERT_TRUE(plan.client_scroll->target == descriptor.target);
                }
            } else {
                ASSERT_EQ(plan.commands.size(), std::size_t{1});
                ASSERT_EQ(plan.commands[0].command_id,
                          std::string{descriptor.scrollCommand});
                auto const* args = std::any_cast<ssg::ScrollFractionArguments>(
                    &plan.commands[0].payload);
                ASSERT_TRUE(args != nullptr);
                if (args) {
                    ASSERT_EQ(args->numerator, std::uint32_t{3});
                    ASSERT_EQ(args->denominator, std::uint32_t{4});
                }
                ASSERT_FALSE(plan.client_scroll.has_value());
            }
            ++checked;
        }
    }

    // A catalog that scanned nothing would pass vacuously.
    ASSERT_EQ(checked, std::size_t{6});
}

// S-I5: no picker scroll may reach the server. Its ranked list is client-owned
// for latency, so a round-trip on this path would undo that design. Asserted
// over the catalog rather than on the picker alone, so a future client-owned
// surface is covered by the same rule.
TEST(noClientOwnedSurfaceEverDispatchesAScrollCommand) {
    ssg::app::PointerTargets const empty;
    std::size_t clientOwned = 0;

    for (auto const& descriptor : ssg::app::scrollable_regions()) {
        if (!descriptor.scrollCommand.empty()) continue;
        ++clientOwned;
        // A client-owned surface must name a target the loop can act on;
        // `none` would be a gesture routed nowhere.
        ASSERT_TRUE(descriptor.target != ssg::app::WheelTarget::none);

        ssg::RegionHit hit;
        hit.region = descriptor.scrollbar;
        hit.scrollNumerator = 1;
        hit.scrollDenominator = 2;
        for (auto kind : {ssg::app::PointerKind::press,
                          ssg::app::PointerKind::drag}) {
            auto plan = ssg::app::route_pointer(
                hit, ssg::app::PointerButton::left, kind, false, false, std::nullopt,
                empty);
            ASSERT_TRUE(plan.commands.empty());
        }
    }
    // The picker is the one such surface today; if that ever becomes zero the
    // rule above would be vacuous.
    ASSERT_EQ(clientOwned, std::size_t{1});
}

// The wheel and the gutter must agree about which surface a region belongs to.
// They previously came from separate code, which is how they could drift.
TEST(theWheelAndTheGutterAgreeOnEverySurface) {
    for (auto const& descriptor : ssg::app::scrollable_regions()) {
        ASSERT_TRUE(ssg::app::route_wheel(descriptor.content) ==
                    descriptor.target);
        ASSERT_TRUE(ssg::app::route_wheel(descriptor.scrollbar) ==
                    descriptor.target);
    }
}

// Every scrollbar region the library can report must be in the catalog. Named
// explicitly because adding a HitRegion does NOT fail to compile here.
TEST(theCatalogCoversEveryScrollbarHitRegion) {
    constexpr ssg::HitRegion kScrollbarRegions[] = {
        ssg::HitRegion::EditorScrollbar,
        ssg::HitRegion::PanelScrollbar,
        ssg::HitRegion::PaletteScrollbar,
    };
    for (auto region : kScrollbarRegions) {
        bool found = false;
        for (auto const& descriptor : ssg::app::scrollable_regions()) {
            if (descriptor.scrollbar == region) found = true;
        }
        ASSERT_TRUE(found);
    }
    ASSERT_EQ(ssg::app::scrollable_regions().size(),
              std::size(kScrollbarRegions));
}

TEST(routePointerTabPressActivatesTheTab) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Tab;
    hit.tabIndex = 2;
    ssg::app::PointerTargets targets;
    targets.tab_id = ssg::TabId{7};

    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::press, false, false,
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
        hit, ssg::app::PointerButton::left, ssg::app::PointerKind::press, false, false,
        std::nullopt, empty);
    ASSERT_TRUE(unresolved.commands.empty());
}

TEST(routePointerMiddleClickOnATabClosesIt) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Tab;
    hit.tabIndex = 2;
    ssg::app::PointerTargets targets;
    targets.tab_id = ssg::TabId{7};

    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::middle,
                                        ssg::app::PointerKind::press, false, false,
                                        std::nullopt, targets);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"tab.close"});
        auto const* id = std::any_cast<ssg::TabId>(&plan.commands[0].payload);
        ASSERT_TRUE(id != nullptr);
        if (id) ASSERT_TRUE(*id == ssg::TabId{7});
    }

    // Middle-click off a tab, or a release rather than a press, does nothing.
    ssg::RegionHit editorHit;
    editorHit.region = ssg::HitRegion::Editor;
    ASSERT_TRUE(ssg::app::route_pointer(editorHit, ssg::app::PointerButton::middle,
                                        ssg::app::PointerKind::press, false, false,
                                        std::nullopt, targets)
                    .commands.empty());
    ASSERT_TRUE(ssg::app::route_pointer(hit, ssg::app::PointerButton::middle,
                                        ssg::app::PointerKind::release, false, false,
                                        std::nullopt, targets)
                    .commands.empty());
    // An unresolved tab id dispatches nothing.
    ssg::app::PointerTargets const noTab;
    ASSERT_TRUE(ssg::app::route_pointer(hit, ssg::app::PointerButton::middle,
                                        ssg::app::PointerKind::press, false, false,
                                        std::nullopt, noTab)
                    .commands.empty());
}

TEST(doubleClickDetectorPairsPressesByTimeAndCell) {
    using namespace std::chrono_literals;
    auto const window = 400ms;
    auto t0 = std::chrono::steady_clock::time_point{};
    std::uint64_t const surface = 1;  // one document/tab

    ssg::app::ClickTracker tracker;
    ASSERT_FALSE(ssg::app::register_click_is_double(tracker, t0, surface, 5, 10,
                                                    window));
    ASSERT_TRUE(ssg::app::register_click_is_double(tracker, t0 + 100ms, surface,
                                                   5, 10, window));
    // After firing, the tracker resets: a third rapid press is a fresh single
    // (no triple-click).
    ASSERT_FALSE(ssg::app::register_click_is_double(tracker, t0 + 150ms, surface,
                                                    5, 10, window));

    // Same cell but OUTSIDE the window -> two singles.
    ssg::app::ClickTracker slow;
    ASSERT_FALSE(
        ssg::app::register_click_is_double(slow, t0, surface, 2, 2, window));
    ASSERT_FALSE(ssg::app::register_click_is_double(slow, t0 + 500ms, surface, 2,
                                                    2, window));

    // Within the window but on a DIFFERENT cell -> two singles, then a same-cell
    // press within the window of THAT press pairs.
    ssg::app::ClickTracker moved;
    ASSERT_FALSE(
        ssg::app::register_click_is_double(moved, t0, surface, 1, 1, window));
    ASSERT_FALSE(ssg::app::register_click_is_double(moved, t0 + 50ms, surface, 1,
                                                    2, window));
    ASSERT_TRUE(ssg::app::register_click_is_double(moved, t0 + 80ms, surface, 1,
                                                   2, window));

    // Same cell within the window but a DIFFERENT surface (a fast click after a
    // tab switch) -> not a double-click.
    ssg::app::ClickTracker switched;
    ASSERT_FALSE(
        ssg::app::register_click_is_double(switched, t0, 1, 3, 3, window));
    ASSERT_FALSE(ssg::app::register_click_is_double(switched, t0 + 50ms, 2, 3, 3,
                                                    window));
}

// The app loop's decision is a thin gate over the classifier: a single left
// editor press routes to cursor.set_position (with a drag armed), while a
// recognized double-click routes through double_click_dispatch, which selects
// the word at the position and arms NO drag. Pinned here so a mis-wire at that
// seam is caught.
TEST(aDoubleClickOnTheEditorSelectsTheWordNotJustTheCaret) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    hit.byteOffset = 3;
    ssg::app::PointerTargets targets;
    auto const position =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0},
                              ssg::CellIndex{3}};
    targets.document_position = position;

    // Single press: caret placement, drag armed.
    auto single = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                          ssg::app::PointerKind::press, false, false,
                                          std::nullopt, targets);
    ASSERT_EQ(single.commands.size(), std::size_t{1});
    ASSERT_EQ(single.commands[0].command_id,
              std::string{"cursor.set_position"});
    ASSERT_TRUE(single.begins_drag);

    // Double-click: word selection, NO drag, and the clicked position passed
    // through as the command argument.
    auto doubled = ssg::app::double_click_dispatch(position);
    ASSERT_EQ(doubled.commands.size(), std::size_t{1});
    ASSERT_EQ(doubled.commands[0].command_id,
              std::string{"select.word_at_position"});
    ASSERT_FALSE(doubled.begins_drag);
    auto const* args = std::any_cast<ssg::SelectionCommandArguments>(
        &doubled.commands[0].payload);
    ASSERT_TRUE(args != nullptr);
    if (args) {
        ASSERT_TRUE(args->position.has_value());
        if (args->position) ASSERT_EQ(*args->position, position);
        ASSERT_FALSE(args->selection.has_value());
    }
}

TEST(scrollbarGrabOffsetHoldsTheThumbUnderTheCursor) {
    // Press ON the thumb: the offset is where within the thumb it was grabbed,
    // so the same point of the thumb stays under the cursor as it drags.
    int const thumbStart = 4;
    int const thumbSize = 3;  // rows 4,5,6
    ASSERT_EQ(ssg::app::scrollbar_grab_offset(4, thumbStart, thumbSize), 0);
    ASSERT_EQ(ssg::app::scrollbar_grab_offset(5, thumbStart, thumbSize), 1);
    ASSERT_EQ(ssg::app::scrollbar_grab_offset(6, thumbStart, thumbSize), 2);
    // Press in the well (above or below the thumb): centre the thumb on the
    // cursor, so it jumps to the click and can then be dragged from its middle.
    ASSERT_EQ(ssg::app::scrollbar_grab_offset(0, thumbStart, thumbSize),
              thumbSize / 2);
    ASSERT_EQ(ssg::app::scrollbar_grab_offset(20, thumbStart, thumbSize),
              thumbSize / 2);
}

TEST(gutterFractionTracksTheGrabbedPointAndClamps) {
    int const travel = 10;  // viewportRows - thumbSize
    int const grabOffset = 1;

    // The thumb top is rel - grabOffset, reported over the travel.
    auto const mid = ssg::app::gutter_fraction(6, grabOffset, travel);
    ASSERT_EQ(mid.numerator, std::uint32_t{5});
    ASSERT_EQ(mid.denominator, std::uint32_t{10});

    // Above the top and below the bottom clamp rather than escaping the range.
    auto const top = ssg::app::gutter_fraction(0, grabOffset, travel);
    ASSERT_EQ(top.numerator, std::uint32_t{0});
    auto const bottom = ssg::app::gutter_fraction(100, grabOffset, travel);
    ASSERT_EQ(bottom.numerator, std::uint32_t{10});
    ASSERT_EQ(bottom.denominator, std::uint32_t{10});

    // A thumb that fills the gutter (travel <= 0) never scrolls: 0/1.
    auto const still = ssg::app::gutter_fraction(3, grabOffset, 0);
    ASSERT_EQ(still.numerator, std::uint32_t{0});
    ASSERT_EQ(still.denominator, std::uint32_t{1});
}

TEST(routePointerPalettePressExecutesTheCandidate) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Palette;
    hit.itemIndex = 4;
    ssg::app::PointerTargets targets;
    targets.picker_candidate_id = std::string{"view.split"};
    targets.picker_mode = ssg::SearchMode::Command;

    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::press, false, false,
                                        std::nullopt, targets);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"palette.execute"});
        auto const* args = std::any_cast<ssg::PaletteExecuteArguments>(
            &plan.commands[0].payload);
        ASSERT_TRUE(args != nullptr);
        if (args) ASSERT_EQ(args->commandId, std::string{"view.split"});
    }
    ASSERT_FALSE(plan.begins_drag);

    // A palette hit with no resolved candidate id dispatches nothing.
    ssg::app::PointerTargets const empty;
    auto unresolved = ssg::app::route_pointer(
        hit, ssg::app::PointerButton::left, ssg::app::PointerKind::press, false, false,
        std::nullopt, empty);
    ASSERT_TRUE(unresolved.commands.empty());
}

// Clicking a row must mean the same as pressing Enter on it.  A file
// candidate's id is a PATH, so routing it to palette.execute would both be
// rejected by the server guard and be nonsense; this is the pointer half of the
// mode-dispatched submit.
TEST(routePointerFilePickerPressOpensTheFileRatherThanExecutingIt) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Palette;
    hit.itemIndex = 2;
    ssg::app::PointerTargets targets;
    targets.picker_candidate_id = std::string{"src/runtime/snapshot.cpp"};
    targets.picker_mode = ssg::SearchMode::File;

    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::press, false, false,
                                        std::nullopt, targets);
    ASSERT_EQ(plan.commands.size(), std::size_t{1});
    if (plan.commands.size() == 1) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"file.open"});
        ASSERT_TRUE(std::any_cast<ssg::PaletteExecuteArguments>(
                        &plan.commands[0].payload) == nullptr);
        auto const* path = std::any_cast<std::string>(&plan.commands[0].payload);
        ASSERT_TRUE(path != nullptr);
        if (path) ASSERT_EQ(*path, std::string{"src/runtime/snapshot.cpp"});
    }
    ASSERT_FALSE(plan.begins_drag);
}

TEST(routePointerPanelPressSelectsAndActivatesTheNode) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Panel;
    hit.nodeId = ssg::TreeNodeId{"files:src/main.cpp"};
    ssg::app::PointerTargets const empty;

    auto plan = ssg::app::route_pointer(hit, ssg::app::PointerButton::left,
                                        ssg::app::PointerKind::press, false, false,
                                        std::nullopt, empty);
    // A tree-row click selects the node, then activates it (matching keyboard
    // select-then-Enter), in that order.
    ASSERT_EQ(plan.commands.size(), std::size_t{2});
    if (plan.commands.size() == 2) {
        ASSERT_EQ(plan.commands[0].command_id, std::string{"tree.select"});
        auto const* args = std::any_cast<ssg::TreeSelectArguments>(
            &plan.commands[0].payload);
        ASSERT_TRUE(args != nullptr);
        if (args) ASSERT_EQ(args->nodeId, *hit.nodeId);
        ASSERT_EQ(plan.commands[1].command_id, std::string{"tree.activate"});
    }
    ASSERT_FALSE(plan.begins_drag);
    ASSERT_FALSE(plan.ends_drag);

    // A panel hit with no node id (the provider-label row / empty area) is inert.
    ssg::RegionHit noNode;
    noNode.region = ssg::HitRegion::Panel;
    auto inert = ssg::app::route_pointer(
        noNode, ssg::app::PointerButton::left, ssg::app::PointerKind::press, false,
        false, std::nullopt, empty);
    ASSERT_TRUE(inert.commands.empty());
}

TEST(routeWheelMapsRegionToScrollTarget) {
    using ssg::app::WheelTarget;
    // The side panel and its gutter scroll the tree.
    ASSERT_TRUE(ssg::app::route_wheel(ssg::HitRegion::Panel) == WheelTarget::tree);
    ASSERT_TRUE(ssg::app::route_wheel(ssg::HitRegion::PanelScrollbar) ==
                WheelTarget::tree);
    // The palette and its gutter scroll the client-owned palette window.
    ASSERT_TRUE(ssg::app::route_wheel(ssg::HitRegion::Palette) ==
                WheelTarget::palette);
    ASSERT_TRUE(ssg::app::route_wheel(ssg::HitRegion::PaletteScrollbar) ==
                WheelTarget::palette);
    // The editor, its gutter, a tab, and no region all scroll the document.
    for (auto region : {ssg::HitRegion::Editor, ssg::HitRegion::EditorScrollbar,
                        ssg::HitRegion::Tab, ssg::HitRegion::None}) {
        ASSERT_TRUE(ssg::app::route_wheel(region) == WheelTarget::editor);
    }
}

TEST(edgeScrollDecidesDirectionAtTheContentEdges) {
    // Editor content occupying rows [2, 23): y=2, height=21, bottom=23.
    ssg::Rect const content{0, 2, 79, 21};

    // Not dragging: never auto-scrolls, wherever the pointer is.
    ASSERT_FALSE(ssg::app::edge_scroll(false, 0, content).has_value());
    ASSERT_FALSE(ssg::app::edge_scroll(false, 100, content).has_value());

    // Above the top content row -> scroll up.
    auto up = ssg::app::edge_scroll(true, 1, content);
    ASSERT_TRUE(up.has_value());
    if (up) ASSERT_EQ(*up, -1);

    // At or below the bottom -> scroll down.
    auto atBottom = ssg::app::edge_scroll(true, 23, content);  // == bottom()
    ASSERT_TRUE(atBottom.has_value());
    if (atBottom) ASSERT_EQ(*atBottom, 1);
    auto below = ssg::app::edge_scroll(true, 60, content);
    ASSERT_TRUE(below.has_value());
    if (below) ASSERT_EQ(*below, 1);

    // Every interior row (including the top and last visible content rows) is
    // within-viewport -> no auto-scroll (M8-S handles those).
    for (int row = content.y; row < content.bottom(); ++row) {
        ASSERT_FALSE(ssg::app::edge_scroll(true, row, content).has_value());
    }

    // A degenerate (zero-height) content rect never scrolls.
    ASSERT_FALSE(ssg::app::edge_scroll(true, 5, ssg::Rect{0, 2, 79, 0}).has_value());
}

TEST(oneCopyIsWrittenOnceAndOnlyToATerminalThatAdvertisedOsc52) {
    ssg::app::SystemClipboardWriter writer;
    ssg::ClipboardWrite const first{1, ssg::Revision{1}, "hi"};

    // A snapshot keeps republishing the same write, because nothing reports
    // back.  It reaches the terminal exactly once.
    auto const served = writer.bytesFor(first, true);
    ASSERT_TRUE(served.has_value());
    ASSERT_EQ(*served, ssg::app::encode_clipboard_write("hi"));
    ASSERT_FALSE(writer.bytesFor(first, true).has_value());
    ASSERT_FALSE(writer.bytesFor(first, true).has_value());

    // A new copy is a new id, and is served.
    ssg::ClipboardWrite const second{2, ssg::Revision{2}, "there"};
    auto const again = writer.bytesFor(second, true);
    ASSERT_TRUE(again.has_value());
    ASSERT_EQ(*again, ssg::app::encode_clipboard_write("there"));

    // Nothing published, nothing written.
    ASSERT_FALSE(writer.bytesFor(std::nullopt, true).has_value());

    // Without the capability nothing is written at all -- the bytes would be an
    // unrecognised escape sequence, not a copy.  And the id is NOT consumed, so
    // a terminal that later advertises OSC 52 still gets the pending copy.
    ssg::app::SystemClipboardWriter gated;
    ssg::ClipboardWrite const third{3, ssg::Revision{3}, "x"};
    ASSERT_FALSE(gated.bytesFor(third, false).has_value());
    ASSERT_TRUE(gated.bytesFor(third, true).has_value());
}

// Host oracle: the SHARED
// evaluateInitScript funnel (used by BOTH startup loadInitScript and reload
// drainAndEvaluate) must push the script's chrome composition into the runtime,
// so a refactor cannot silently stop wiring one path.
TEST(evaluateInitScriptPushesComposedChromeToTheRuntime) {
    auto root = fs::temp_directory_path() / "ssg-init-chrome";
    fs::remove_all(root);
    fs::create_directories(root / "workspace");
    fs::create_directories(root / "scratch");
    fs::create_directories(root / "recovery");
    std::ofstream{root / "workspace" / "f.txt"} << "hello\n";

    auto created = ssg::EditorRuntime::create(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.runtime;
    ASSERT_TRUE(runtime.attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                               ssg::ViewId{1})
                    .accepted());
    ASSERT_TRUE(runtime
                    .dispatch(ssg::ClientId{1},
                              {"file.open", runtime.revision(),
                               std::string{"f.txt"}})
                    .accepted());
    ssg::ScriptHost scripts{runtime};

    const ssg::ViewportDimensions dims{80, 12};
    const auto hasHeaderField = [&](std::string_view id) {
        auto snap = runtime.snapshot(ssg::ClientId{1}, dims);
        if (!snap) return false;
        for (const auto& node : snap->presentation()->shell.accessibilityNodes) {
            if (node.kind == ssg::ShellNodeKind::HeaderField && node.id == id)
                return true;
        }
        return false;
    };

    // A script composing a header replaces the built-in path field, once run
    // through the funnel.
    ssg::app::evaluateInitScript(
        scripts, runtime, root / "init.lua",
        "ssg.chrome{ header = { left = { "
        "{ kind = 'field', id = 'app.path', provider = 'path' } } } }");
    ASSERT_TRUE(hasHeaderField("app.path"));
    ASSERT_FALSE(hasHeaderField("path"));

    // A later reload that drops the ssg.chrome call reverts to built-in, again
    // via the SAME funnel -- proving both effects flow through it.
    ssg::app::evaluateInitScript(scripts, runtime, root / "init.lua",
                                 "local x = 1");
    ASSERT_FALSE(hasHeaderField("app.path"));
    ASSERT_TRUE(hasHeaderField("path"));
}

int main() {
    RUN(resolveLaunchNoArgumentOpensCwd);
    RUN(evaluateInitScriptPushesComposedChromeToTheRuntime);
    RUN(resolveLaunchDirectoryOpensThatDirectory);
    RUN(resolveLaunchFileOpensParentDirectoryAndFile);
    RUN(everyDeclaredModeLeavesExactlyWhatItEnters);
    RUN(modeStackReproducesTheCuratedSetupAndRestoreSequences);
    RUN(everyEnteredModeIsLeftInReverseOrder);
    RUN(theCrashUndoLeavesEveryDeclaredModeInReverseOrder);
    RUN(kittyKeyboardModeRoundTripsThroughAGuard);
    RUN(aFrameWithNoCaretLeavesTheCursorVisible);
    RUN(unicodeEndToEndGridAndEncoding);
    RUN(classifySignalTagsMapsSignalNumbers);
    RUN(encodeAnsiFrameAdaptsToColorDepth);
    RUN(statusActionPointerClickRoutesToInvokeActionWithGeneration);
    RUN(aClickOnAnExternalActionRoutesThroughPointerTargetsToSelectThenAct);
    RUN(exmdParsesActionAndIdWithoutSplittingTheIdOnColon);
    RUN(exmdOnlyDispatchesForAPublishedIdAndOfferedAction);
    RUN(encodeAnsiFrameEmitsOrthogonalTintBackgrounds);
    RUN(detectColorDepthReadsEnvironment);
    RUN(encodeAnsiFrameAddressesRowsAndEmitsPaletteColors);
    RUN(encodeAnsiFrameSkipsWideGlyphContinuation);
    RUN(decodeInputMapsPrintablesAndNamedKeys);
    RUN(decodeInputModifiedArrows);
    RUN(decodeInputMetaPrefixedCsiFoldsAlt);
    RUN(decodeInputTildeHomeEndPlainAndModified);
    RUN(decodeInputStripsLockModifiersFromFunctionalKeys);
    RUN(decodeKittyKeyMatchesEveryDefaultBinding);
    RUN(decodeKittyKeyHandCasesAndCapsLockImmunity);
    RUN(everyEnterEncodingNormalizesToBareEnter);
    RUN(decodeKittyKeySelfIdentifyingAndMalformed);
    RUN(decodeInputModifiedArrowSplitReadsAreIncomplete);
    RUN(decodeInputDeleteKeyPlainAndModified);
    RUN(decodeInputPageKeysPlainAndModified);
    RUN(decodeInputArrowsAndMouse);
    RUN(noCapabilityReplyIsEverEmittedAsText);
    RUN(anUnboundedSequenceScanCannotHoldTheInputBuffer);
    RUN(aReplySplitAcrossReadsIsStillConsumedWhole);
    RUN(noDcsOrOscQueryMaySolicitAnUnparsedReply);
    RUN(onlyADa1ReplyMeansTheFeatureIsAbsent);
    RUN(reprobingDoesNotCarryStaleAnswersForward);
    RUN(aReplyShapeAfterTheFenceIsNotACapabilityAnswer);
    RUN(anOverrideBeatsTheTerminalsOwnAnswer);
    RUN(everyCapabilityHasAWorkingOverride);
    RUN(theProbeAsksOnlyQuestionsItCanUnderstand);
    RUN(aReplyArrivingAfterTheWindowExpiresIsNotBelieved);
    RUN(configDocDocumentsEveryCapabilityOverride);
    RUN(theProbeLogSeparatesWhatWasBelievedFromWhatArrivedLate);
    RUN(realTerminalRepliesResolveAsObserved);
    RUN(aDragFrameEndsWithTheCursorHiddenAndEveryOtherFrameShowsIt);
    RUN(aBracketedPasteIsContentAndNeverKeys);
    RUN(clipboardWriteEncodesOsc52WithBase64);
    RUN(diagnosticsUnderlineTheirCellsWithoutRecolouringThem);
    RUN(hyperlinkRunsAreOpenedAndAlwaysClosed);
    RUN(decodeInputPointerPressReleaseDrag);
    RUN(decodeInputPointerCarriesAltModifier);
    RUN(decodeInputPointerSplitReadsAreIncomplete);
    RUN(decodeInputPointerRejectsMalformedButTerminatedPayloads);
    RUN(routePointerLeftPressOnEditorPlacesCaret);
    RUN(routePointerIgnoresNonEditorAndNonLeft);
    RUN(routePointerDragExtendsSelectionFromAnchor);
    RUN(routePointerFieldHitDispatchesPublishedCommandGenerically);
    RUN(routePointerDragWithoutAnchorOrTargetIsANoOp);
    RUN(routePointerReleaseEndsDragWithoutACommand);
    RUN(routePointerAltPressAddsCollapsedCaret);
    RUN(altClickRemovesOneOfTwoCarets);
    RUN(altClickInsideARangeRemovesThatRange);
    RUN(altClickOnEmptySpaceAddsACaret);
    RUN(altClickOnTheSoleCaretIsANoOpAdd);
    RUN(altClickRangeBoundaryIsExclusiveAtTheUpperEnd);
    RUN(altClickPrefersACaretOverARangeSharingItsLowerBound);
    RUN(altDoubleClickStillSelectsAWordNeverRemoves);
    RUN(altClickRemoveEndToEndLeavesTheSurvivingCaret);
    RUN(routePointerAltDragSetsRangesFromBaseline);
    RUN(routePointerAltDragIgnoresPerMotionModifierBit);
    RUN(routePointerEditorScrollbarScrollsToFraction);
    RUN(everyScrollableGutterAnswersPressAndDrag);
    RUN(noClientOwnedSurfaceEverDispatchesAScrollCommand);
    RUN(theWheelAndTheGutterAgreeOnEverySurface);
    RUN(theCatalogCoversEveryScrollbarHitRegion);
    RUN(routePointerTabPressActivatesTheTab);
    RUN(routePointerMiddleClickOnATabClosesIt);
    RUN(doubleClickDetectorPairsPressesByTimeAndCell);
    RUN(aDoubleClickOnTheEditorSelectsTheWordNotJustTheCaret);
    RUN(scrollbarGrabOffsetHoldsTheThumbUnderTheCursor);
    RUN(gutterFractionTracksTheGrabbedPointAndClamps);
    RUN(routePointerPalettePressExecutesTheCandidate);
    RUN(routePointerFilePickerPressOpensTheFileRatherThanExecutingIt);
    RUN(routePointerPanelPressSelectsAndActivatesTheNode);
    RUN(routeWheelMapsRegionToScrollTarget);
    RUN(edgeScrollDecidesDirectionAtTheContentEdges);
    RUN(decodeInputEscapeBoundaryIsBounded);
    RUN(decodeInputCoalescesMetaPrefixIntoAltStrokes);
    RUN(oneCopyIsWrittenOnceAndOnlyToATerminalThatAdvertisedOsc52);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed > 0 ? 1 : 0;
}
