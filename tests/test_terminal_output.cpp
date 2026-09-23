#include <ssg/Terminal.h>
#include <ssg/TerminalCapabilities.h>
#include <ssg/TerminalInput.h>
#include <ssg/TerminalOutput.h>
#include <ssg/pointer_routing.h>

#include <ssg/focus.h>
#include <ssg/HitTester.h>
#include <ssg/PromptSurface.h>
#include <ssg/Renderer.h>
#include <ssg/Selection.h>

#include "test_helpers.h"
#include "ansi_screen_model.h"
#include "editor_test_support.h"
#include "grid_test_frame.h"

#include <algorithm>
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
    auto truecolor = ssg::encodeAnsiFrame(screen, ssg::ColorDepth::Truecolor);
    ASSERT_TRUE(truecolor.find("\x1b[38;2;255;0;0m") != std::string::npos);
    ASSERT_TRUE(truecolor.find("\x1b[48;2;0;0;0m") != std::string::npos);

    // Indexed256: pure red is xterm cube index 196; black is index 16.
    auto indexed = ssg::encodeAnsiFrame(screen, ssg::ColorDepth::Indexed256);
    ASSERT_TRUE(indexed.find("\x1b[38;5;196m") != std::string::npos);
    ASSERT_TRUE(indexed.find("\x1b[48;5;16m") != std::string::npos);
    ASSERT_TRUE(indexed.find(";2;") == std::string::npos);  // no truecolor bytes

    // ANSI16: pure red is base index 9 (bright red) -> fg SGR 91; black is index
    // 0 -> bg SGR 40.
    auto ansi = ssg::encodeAnsiFrame(screen, ssg::ColorDepth::Ansi16);
    ASSERT_TRUE(ansi.find("\x1b[91m") != std::string::npos);
    ASSERT_TRUE(ansi.find("\x1b[40m") != std::string::npos);
    ASSERT_TRUE(ansi.find(";5;") == std::string::npos);
    ASSERT_TRUE(ansi.find(";2;") == std::string::npos);
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
        ssg::encodeAnsiFrame(screen, ssg::ColorDepth::Truecolor);
    ASSERT_TRUE(truecolor.find("\x1b[48;2;30;30;30m") != std::string::npos);
    ASSERT_TRUE(truecolor.find("\x1b[48;2;0;0;95m") != std::string::npos);
    const auto indexed =
        ssg::encodeAnsiFrame(screen, ssg::ColorDepth::Indexed256);
    const auto tintIndex =
        ssg::ColorResolver{ssg::ColorDepth::Indexed256}.resolve(screen.diffTints.addedRow)
            .index;
    ASSERT_TRUE(indexed.find("\x1b[48;5;" + std::to_string(tintIndex) + "m") !=
                std::string::npos);
}

TEST(detectColorDepthReadsEnvironment) {
    using ssg::ColorDepth;
    ASSERT_TRUE(ssg::detectColorDepth("truecolor", nullptr, "dumb", nullptr) ==
                ColorDepth::Truecolor);
    ASSERT_TRUE(ssg::detectColorDepth("24BIT", nullptr, "dumb", nullptr) ==
                ColorDepth::Truecolor);
    ASSERT_TRUE(ssg::detectColorDepth("256", nullptr, "xterm", nullptr) ==
                ColorDepth::Indexed256);
    ASSERT_TRUE(ssg::detectColorDepth("indexed256", nullptr, "xterm", nullptr) ==
                ColorDepth::Indexed256);
    ASSERT_TRUE(ssg::detectColorDepth("ansi16", nullptr, "xterm", nullptr) ==
                ColorDepth::Ansi16);
    ASSERT_TRUE(ssg::detectColorDepth("bogus", "truecolor", "dumb", nullptr) ==
                ColorDepth::Truecolor);

    ASSERT_TRUE(ssg::detectColorDepth(nullptr, "truecolor", "xterm-256color",
                                             nullptr) == ColorDepth::Truecolor);
    ASSERT_TRUE(ssg::detectColorDepth(nullptr, "24bit", "xterm", nullptr) ==
                ColorDepth::Truecolor);

    ASSERT_TRUE(ssg::detectColorDepth(nullptr, nullptr, "dumb", "wezterm") ==
                ColorDepth::Ansi16);
    ASSERT_TRUE(ssg::detectColorDepth(nullptr, nullptr, "", "wezterm") ==
                ColorDepth::Ansi16);
    ASSERT_TRUE(ssg::detectColorDepth(nullptr, nullptr, nullptr, "wezterm") ==
                ColorDepth::Ansi16);

    ASSERT_TRUE(ssg::detectColorDepth(nullptr, nullptr, nullptr, nullptr,
                                     "windows-terminal-session") ==
                ColorDepth::Truecolor);
    ASSERT_TRUE(ssg::detectColorDepth(nullptr, nullptr, "", nullptr,
                                     "windows-terminal-session") ==
                ColorDepth::Truecolor);
    ASSERT_TRUE(ssg::detectColorDepth(nullptr, nullptr, "dumb", nullptr,
                                     "windows-terminal-session") ==
                ColorDepth::Ansi16);
    ASSERT_TRUE(ssg::detectColorDepth(nullptr, nullptr, nullptr, nullptr, "") ==
                ColorDepth::Ansi16);
    ASSERT_TRUE(
        ssg::detectColorDepth(nullptr, nullptr, nullptr, nullptr, nullptr, true) ==
        ColorDepth::Truecolor);
    ASSERT_TRUE(
        ssg::detectColorDepth("16", nullptr, nullptr, nullptr, nullptr, true) ==
        ColorDepth::Ansi16);

    ASSERT_TRUE(ssg::detectColorDepth(nullptr, nullptr, "screen",
                                             "iTerm.app") == ColorDepth::Truecolor);
    ASSERT_TRUE(ssg::detectColorDepth(nullptr, nullptr, "xterm-kitty",
                                             nullptr) == ColorDepth::Truecolor);

    ASSERT_TRUE(ssg::detectColorDepth(nullptr, nullptr, "vt100", nullptr) ==
                ColorDepth::Truecolor);
    ASSERT_TRUE(ssg::detectColorDepth(nullptr, "", "xterm-256color", nullptr) ==
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

    auto frame = ssg::encodeAnsiFrame(screen);
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

    auto frame = ssg::encodeAnsiFrame(screen);
    ASSERT_TRUE(frame.find("\xe4\xb8\xad") != std::string::npos);
    // The wide glyph occupies both columns; the continuation adds no glyph.
    auto rowStart = frame.find("\x1b[1;1H");
    ASSERT_TRUE(rowStart != std::string::npos);
    auto glyph = frame.find("\xe4\xb8\xad", rowStart);
    ASSERT_TRUE(frame.find(' ', glyph + 3) == std::string::npos ||
                frame.find("\x1b[0m", glyph) < frame.find(' ', glyph + 3));
}

TEST(unicodeEndToEndGridAndEncoding) {
    ssg::LineLayoutCache lineCache;
    // text -> snapshot -> render -> encode, locking the client Unicode path:
    // a wide CJG glyph occupies a cell + continuation, a combining mark folds
    // into its base grapheme (width 1), a ZWJ emoji sequence is one wide cluster,
    // the caret advances by 2 past a wide glyph, and the encoder emits one glyph
    // per cluster and nothing for a continuation cell.
    auto root = testSystemRuntimePath("terminal_output_m9u");
    fs::remove_all(root);
    fs::create_directories(root / "workspace");
    fs::create_directories(root / "recovery");
    // a b [U+4E00 wide] e [U+0301 combining] [U+1F468 ZWJ U+1F469]
    const std::string cjk = "\xE4\xB8\x80";                 // U+4E00, wide
    const std::string ecombining = "e\xCC\x81";             // e + U+0301
    const std::string emoji = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9";
    const std::string line = "ab" + cjk + ecombining + emoji;
    std::ofstream{root / "workspace" / "u.txt", std::ios::binary} << line << "\n";

    auto created = ssg::createEditor(
        {root / "workspace", root / "recovery", root / "archive"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"u.txt"})
                    .accepted());
    auto gridFrame = ssg::test::projectGridFrame(runtime, ssg::ViewportDimensions{80, 24});
    ASSERT_TRUE(gridFrame.has_value());
    if (!gridFrame) return;
    auto grid = ssg::renderFrame(*gridFrame, lineCache);

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
    auto before = ssg::resolveSelectionPosition(line, ssg::ByteOffset{2});
    auto after = ssg::resolveSelectionPosition(line, ssg::ByteOffset{5});
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(after.has_value());
    auto caretColumnAt = [&](std::optional<ssg::DocumentPosition> pos) -> int {
        ASSERT_TRUE(pos.has_value());
        if (!pos) return -1;
        (void)ssg::test::setSelections(
            runtime,
            {{pos->byteOffset.value(), pos->byteOffset.value()}});
        auto frame = ssg::test::projectGridFrame(runtime, ssg::ViewportDimensions{80, 24});
        if (!frame) return -1;
        auto g = ssg::renderFrame(*frame, lineCache);
        return g.caret ? g.caret->column : -1;
    };
    int const columnBefore = caretColumnAt(before);
    int const columnAfter = caretColumnAt(after);
    ASSERT_EQ(columnBefore, startx + 2);
    ASSERT_EQ(columnAfter, startx + 4);
    ASSERT_EQ(columnAfter - columnBefore, 2);

    // Encoding: each wide cluster emits exactly one glyph and continuation cells
    // emit nothing, so the CJK and emoji byte sequences each appear exactly once.
    auto frame = ssg::encodeAnsiFrame(grid, ssg::ColorDepth::Truecolor);
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

TEST(aDragFrameEndsWithTheCursorHiddenAndEveryOtherFrameShowsIt) {
    ssg::CellGrid grid;
    grid.size = {4, 2};
    grid.cells.resize(8);
    grid.caret = ssg::GridPosition{1, 1};

    auto const shown = ssg::encodeFrame(grid, ssg::ColorDepth::Ansi16);
    auto const hidden =
        ssg::encodeFrame(grid, ssg::ColorDepth::Ansi16, false);

    std::string const enter{ssg::kCursorHidden.enter};
    std::string const leave{ssg::kCursorHidden.leave};

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

TEST(clipboardWriteEncodesOsc52WithBase64) {
    // Base64 checked against known answers rather than against another encoder,
    // including both padding lengths, which is where an encoder goes wrong.
    ASSERT_EQ(ssg::encodeClipboardWrite("hi"),
              std::string{"\x1b]52;c;aGk=\x1b\\"});
    ASSERT_EQ(ssg::encodeClipboardWrite("abc"),
              std::string{"\x1b]52;c;YWJj\x1b\\"});
    ASSERT_EQ(ssg::encodeClipboardWrite("a"),
              std::string{"\x1b]52;c;YQ==\x1b\\"});
    ASSERT_EQ(ssg::encodeClipboardWrite(""),
              std::string{"\x1b]52;c;\x1b\\"});
    // Bytes above 0x7f survive: the payload is base64 of raw bytes, not of text
    // the encoder has opinions about.
    ASSERT_EQ(ssg::encodeClipboardWrite("\xc3\xa9"),
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
        ssg::encodeAnsiFrame(grid, ssg::ColorDepth::Truecolor);
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
        ssg::encodeAnsiFrame(undecorated, ssg::ColorDepth::Truecolor);
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
        ssg::encodeAnsiFrame(grid, ssg::ColorDepth::Truecolor);
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
        ssg::encodeAnsiFrame(edge, ssg::ColorDepth::Truecolor);
    auto const edgeOpen = edgeFrame.find("\x1b]8;;https://example.com\x1b\\");
    ASSERT_TRUE(edgeOpen != std::string::npos);
    ASSERT_TRUE(edgeFrame.find("\x1b]8;;\x1b\\", edgeOpen) != std::string::npos);

    // No links, no OSC 8 at all: a document without URLs pays nothing.
    ssg::CellGrid plain = grid;
    plain.hyperlinks.clear();
    auto const plainFrame =
        ssg::encodeAnsiFrame(plain, ssg::ColorDepth::Truecolor);
    ASSERT_TRUE(plainFrame.find("\x1b]8;") == std::string::npos);
}

TEST(oneCopyIsWrittenOnceAndOnlyToATerminalThatAdvertisedOsc52) {
    ssg::SystemClipboardWriter writer;
    ssg::ClipboardWrite const first{1, std::uint64_t{1}, "hi"};

    // A snapshot keeps republishing the same write, because nothing reports
    // back.  It reaches the terminal exactly once.
    auto const served = writer.bytesFor(first, true);
    ASSERT_TRUE(served.has_value());
    ASSERT_EQ(*served, ssg::encodeClipboardWrite("hi"));
    ASSERT_FALSE(writer.bytesFor(first, true).has_value());
    ASSERT_FALSE(writer.bytesFor(first, true).has_value());

    // A new copy is a new id, and is served.
    ssg::ClipboardWrite const second{2, std::uint64_t{2}, "there"};
    auto const again = writer.bytesFor(second, true);
    ASSERT_TRUE(again.has_value());
    ASSERT_EQ(*again, ssg::encodeClipboardWrite("there"));

    // Nothing published, nothing written.
    ASSERT_FALSE(writer.bytesFor(std::nullopt, true).has_value());

    // Without the capability nothing is written at all -- the bytes would be an
    // unrecognised escape sequence, not a copy.  And the id is NOT consumed, so
    // a terminal that later advertises OSC 52 still gets the pending copy.
    ssg::SystemClipboardWriter gated;
    ssg::ClipboardWrite const third{3, std::uint64_t{3}, "x"};
    ASSERT_FALSE(gated.bytesFor(third, false).has_value());
    ASSERT_TRUE(gated.bytesFor(third, true).has_value());
}

TEST(retainedEncoderEmitsOnlyChangedRunsAndCursorState) {
    ssg::CellGrid first;
    first.size = {5, 1};
    first.cells.resize(5);
    for (int column = 0; column < 5; ++column) {
        first.cells[static_cast<std::size_t>(column)].text =
            std::string{static_cast<char>('a' + column)};
    }
    first.caret = ssg::GridPosition{0, 0};

    ssg::RetainedTerminalEncoder encoder{ssg::ColorDepth::Ansi16};
    auto initial = encoder.encode(first);
    ASSERT_TRUE(initial.complete);
    ASSERT_EQ(initial.changedCells, std::size_t{5});
    encoder.commit(first);

    auto caretOnly = first;
    caretOnly.caret = ssg::GridPosition{4, 0};
    auto caretPatch = encoder.encode(caretOnly);
    ASSERT_FALSE(caretPatch.complete);
    ASSERT_EQ(caretPatch.changedCells, std::size_t{0});
    ASSERT_TRUE(caretPatch.bytes.find("\x1b[1;5H") != std::string::npos);
    ASSERT_TRUE(caretPatch.bytes.find('a') == std::string::npos);
    encoder.commit(caretOnly);

    auto changed = caretOnly;
    changed.cells[0].text = "x";
    changed.cells[0].foreground = 1;
    changed.cells[4].text = "y";
    changed.cells[4].foreground = 2;
    auto patch = encoder.encode(changed);
    ASSERT_FALSE(patch.complete);
    ASSERT_EQ(patch.changedCells, std::size_t{2});
    ASSERT_TRUE(patch.bytes.find("\x1b[1;1H") != std::string::npos);
    ASSERT_TRUE(patch.bytes.find("\x1b[1;5H") != std::string::npos);
    ASSERT_TRUE(patch.bytes.find('b') == std::string::npos);
    ASSERT_TRUE(patch.bytes.find('c') == std::string::npos);
    ASSERT_TRUE(patch.bytes.find('d') == std::string::npos);
}

TEST(retainedEncoderFallsBackForCoordinateAndRangeStateChanges) {
    ssg::CellGrid grid;
    grid.size = {2, 1};
    grid.cells.resize(2);
    ssg::RetainedTerminalEncoder encoder{ssg::ColorDepth::Truecolor};
    encoder.commit(grid);

    auto palette = grid;
    palette.colors[0] = {1, 2, 3};
    ASSERT_TRUE(encoder.encode(palette).complete);

    auto links = grid;
    links.hyperlinks.push_back({0, 0, 1, "https://example.com"});
    ASSERT_TRUE(encoder.encode(links).complete);
    encoder.commit(links);
    links.cells[0].text = "x";
    ASSERT_TRUE(encoder.encode(links).complete);

    auto resized = grid;
    resized.size = {1, 1};
    resized.cells.resize(1);
    ASSERT_TRUE(encoder.encode(resized).complete);

    encoder.invalidate();
    ASSERT_TRUE(encoder.encode(grid).complete);
}

TEST(retainedEncoderAdvancesOnlyWhenCommitted) {
    ssg::CellGrid initial;
    initial.size = {1, 1};
    initial.cells.resize(1);
    initial.cells[0].text = "a";
    ssg::RetainedTerminalEncoder encoder{ssg::ColorDepth::Ansi16};
    encoder.commit(initial);

    auto changed = initial;
    changed.cells[0].text = "b";
    const auto firstAttempt = encoder.encode(changed);
    const auto secondAttempt = encoder.encode(changed);
    ASSERT_EQ(firstAttempt.bytes, secondAttempt.bytes);
    ASSERT_EQ(firstAttempt.changedCells, secondAttempt.changedCells);
    encoder.commit(changed);
    ASSERT_EQ(encoder.encode(changed).changedCells, std::size_t{0});
}

TEST(retainedOutputReplaysToTheCompleteFrameState) {
    ssg::CellGrid initial;
    initial.size = {5, 1};
    initial.cells.resize(5);
    initial.colors[0] = {0, 0, 0};
    initial.colors[1] = {255, 0, 0};
    initial.colors[2] = {0, 255, 0};
    for (int column = 0; column < 5; ++column) {
        auto& cell = initial.cells[static_cast<std::size_t>(column)];
        cell.text = std::string{static_cast<char>('a' + column)};
        cell.foreground = static_cast<std::uint8_t>(column % 2 + 1);
    }
    initial.caret = ssg::GridPosition{0, 0};

    auto target = initial;
    target.cells[0].text = "x";
    target.cells[0].foreground = 2;
    target.cells[4].text = "y";
    target.cells[4].foreground = 1;
    target.caret = ssg::GridPosition{4, 0};

    ssg::RetainedTerminalEncoder encoder{ssg::ColorDepth::Truecolor};
    const auto completeInitial = encoder.encode(initial);
    encoder.commit(initial);
    const auto patch = encoder.encode(target);

    AnsiScreenModel retainedState{5, 1};
    retainedState.apply(completeInitial.bytes);
    retainedState.apply(patch.bytes);
    AnsiScreenModel completeState{5, 1};
    completeState.apply(
        ssg::encodeFrame(target, ssg::ColorDepth::Truecolor));
    ASSERT_TRUE(retainedState == completeState);
}

TEST(showingTheCursorAfterDragRestoresTheCaretPosition) {
    ssg::CellGrid initial;
    initial.size = {3, 1};
    initial.cells.resize(3);
    initial.caret = ssg::GridPosition{0, 0};

    ssg::RetainedTerminalEncoder encoder{ssg::ColorDepth::Ansi16};
    auto visible = encoder.encode(initial);
    encoder.commit(initial, true);

    auto dragged = initial;
    dragged.cells[2].text = "x";
    auto hidden = encoder.encode(dragged, false);
    encoder.commit(dragged, false);

    auto shown = encoder.encode(dragged, true);
    ASSERT_TRUE(shown.bytes.find("\x1b[1;1H") != std::string::npos);
    ASSERT_TRUE(shown.bytes.ends_with(ssg::kCursorHidden.leave));

    AnsiScreenModel retainedState{3, 1};
    retainedState.apply(visible.bytes);
    retainedState.apply(hidden.bytes);
    retainedState.apply(shown.bytes);
    AnsiScreenModel completeState{3, 1};
    completeState.apply(
        ssg::encodeFrame(dragged, ssg::ColorDepth::Ansi16, true));
    ASSERT_TRUE(retainedState == completeState);
}

SSG_TEST_SUITE(test_terminal_output) {
    RUN(encodeAnsiFrameAdaptsToColorDepth);
    RUN(encodeAnsiFrameEmitsOrthogonalTintBackgrounds);
    RUN(detectColorDepthReadsEnvironment);
    RUN(encodeAnsiFrameAddressesRowsAndEmitsPaletteColors);
    RUN(encodeAnsiFrameSkipsWideGlyphContinuation);
    RUN(unicodeEndToEndGridAndEncoding);
    RUN(aDragFrameEndsWithTheCursorHiddenAndEveryOtherFrameShowsIt);
    RUN(clipboardWriteEncodesOsc52WithBase64);
    RUN(diagnosticsUnderlineTheirCellsWithoutRecolouringThem);
    RUN(hyperlinkRunsAreOpenedAndAlwaysClosed);
    RUN(oneCopyIsWrittenOnceAndOnlyToATerminalThatAdvertisedOsc52);
    RUN(retainedEncoderEmitsOnlyChangedRunsAndCursorState);
    RUN(retainedEncoderFallsBackForCoordinateAndRangeStateChanges);
    RUN(retainedEncoderAdvancesOnlyWhenCommitted);
    RUN(retainedOutputReplaysToTheCompleteFrameState);
    RUN(showingTheCursorAfterDragRestoresTheCaretPosition);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
