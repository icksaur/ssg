// Milestone 11 — M11-2: the app is transport only.
//
// An INDEPENDENT terminal model decodes a byte stream in the renderer's output
// vocabulary into a grid of {text, foreground, background} + a cursor cell, with
// NO dependency on the app's encoder.  It is round-trip self-tested (M11-2b:
// decode(encode(grid)) reproduces the grid), then applied to the REAL `ssg`
// binary's pty output (M11-2a) to prove the decoded screen equals render(snapshot)
// — text, resolved color, and cursor — so the app contributes no screen content.
//
// Byte-for-byte equality against the encoder is NOT used as the oracle (a faulty
// encoder would satisfy encoder-vs-encoder equality); the oracle is the hand-
// written decoder vs. the library's render(snapshot).  Colors are forced to
// truecolor (COLORTERM=truecolor) so an SGR carries the raw palette RGB, directly
// comparable to render(snapshot).palette[index].
//
// The pty capture is Linux-scoped (forkpty); the decoder and comparison are
// portable and would back a Windows ConPTY harness unchanged.

#include <ssg/EditorSession.h>
#include <tui/Renderer.h>
#include <ssg/session_snapshot.h>
#include <ssg/Theme.h>

#include "ssg_terminal.h"  // encode_ansi_frame, for the decoder round-trip only.
#include "test_helpers.h"
#include "grid_test_frame.h"

#include <pty.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>

#include <csignal>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Independent terminal model.
// ---------------------------------------------------------------------------

// The independent model keeps its OWN color type (raw wire RGB), so it borrows
// nothing from the library's color surface and is a genuinely separate decoder.
struct Rgb {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
};

struct DecodedCell {
    std::string text{" "};
    Rgb foreground{};
    Rgb background{};
};

struct DecodedScreen {
    int columns = 0;
    int rows = 0;
    std::vector<DecodedCell> cells;
    std::optional<std::pair<int, int>> cursor;  // {row, column}, 0-based.

    DecodedCell& at(int row, int column) {
        return cells[static_cast<std::size_t>(row) * columns + column];
    }
    DecodedCell const& at(int row, int column) const {
        return cells[static_cast<std::size_t>(row) * columns + column];
    }
};

std::size_t utf8Length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xF0) return 4;
    if (lead >= 0xE0) return 3;
    if (lead >= 0xC0) return 2;
    return 1;
}

std::uint32_t utf8Codepoint(std::string_view text) {
    auto const lead = static_cast<unsigned char>(text[0]);
    std::size_t const length = utf8Length(lead);
    if (length == 1) return lead;
    std::uint32_t cp = lead & (0xFF >> (length + 1));
    for (std::size_t k = 1; k < length && k < text.size(); ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(text[k]) & 0x3F);
    }
    return cp;
}

// Terminal display columns of one codepoint.  Zero-width for combining marks,
// two for the East Asian Wide/Fullwidth ranges the renderer treats as wide, one
// otherwise.  (The parity fixture is ASCII, so only the width-1 path runs here,
// but the model honors the full emitted vocabulary.)
int codepointWidth(std::uint32_t cp) {
    if (cp == 0) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || cp == 0x2329 || cp == 0x232A ||
        (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

std::vector<int> splitParams(std::string_view params) {
    std::vector<int> out;
    int value = 0;
    bool any = false;
    for (char ch : params) {
        if (ch >= '0' && ch <= '9') {
            value = value * 10 + (ch - '0');
            any = true;
        } else if (ch == ';') {
            out.push_back(any ? value : 0);
            value = 0;
            any = false;
        }
        // Intermediates such as '?' or ' ' are handled by the caller.
    }
    out.push_back(any ? value : 0);
    return out;
}

// Decode `bytes` (which may contain multiple frames and the terminal setup /
// teardown sequences) cumulatively; the final settled screen is returned, since
// each frame re-addresses every row and later writes overwrite earlier ones.
DecodedScreen decode(std::string_view bytes, int columns, int rows) {
    DecodedScreen screen;
    screen.columns = columns;
    screen.rows = rows;
    screen.cells.assign(static_cast<std::size_t>(columns) * rows, DecodedCell{});

    int cursorRow = 0;
    int cursorColumn = 0;
    Rgb fg{};
    Rgb bg{};

    std::size_t i = 0;
    std::size_t const n = bytes.size();
    while (i < n) {
        auto const ch = static_cast<unsigned char>(bytes[i]);
        if (ch == 0x1b) {
            if (i + 1 >= n) break;
            char const introducer = bytes[i + 1];
            if (introducer != '[') { i += 2; continue; }  // Non-CSI escape: skip.
            std::size_t j = i + 2;
            while (j < n && !(static_cast<unsigned char>(bytes[j]) >= 0x40 &&
                              static_cast<unsigned char>(bytes[j]) <= 0x7e)) {
                ++j;
            }
            if (j >= n) break;  // Incomplete CSI.
            char const final = bytes[j];
            std::string_view const params = bytes.substr(i + 2, j - (i + 2));
            bool const privateMode = !params.empty() && params.front() == '?';

            if (final == 'H' || final == 'f') {
                auto values = splitParams(params);
                int const r = values.size() > 0 ? values[0] : 1;
                int const c = values.size() > 1 ? values[1] : 1;
                cursorRow = r > 0 ? r - 1 : 0;
                cursorColumn = c > 0 ? c - 1 : 0;
            } else if (final == 'm') {
                auto values = splitParams(params);
                for (std::size_t k = 0; k < values.size(); ++k) {
                    if (values[k] == 0) {
                        fg = Rgb{};
                        bg = Rgb{};
                    } else if ((values[k] == 38 || values[k] == 48) &&
                               k + 4 < values.size() && values[k + 1] == 2) {
                        Rgb color{static_cast<std::uint8_t>(values[k + 2]),
                                  static_cast<std::uint8_t>(values[k + 3]),
                                  static_cast<std::uint8_t>(values[k + 4])};
                        if (values[k] == 38) fg = color; else bg = color;
                        k += 4;
                    }
                }
            } else if (privateMode && (final == 'h' || final == 'l') &&
                       params == "?25") {
                if (final == 'h') {
                    screen.cursor = std::pair<int, int>{cursorRow, cursorColumn};
                } else {
                    screen.cursor.reset();
                }
            }
            // All other CSI (private modes, erase, cursor shape) affect no cell.
            i = j + 1;
            continue;
        }
        if (ch < 0x20) { ++i; continue; }  // Bare control byte: ignore.

        std::size_t const length = utf8Length(ch);
        if (i + length > n) break;
        std::string_view const glyph = bytes.substr(i, length);
        int const width = codepointWidth(utf8Codepoint(glyph));
        if (cursorRow >= 0 && cursorRow < rows && cursorColumn >= 0 &&
            cursorColumn < columns && width > 0) {
            auto& cell = screen.at(cursorRow, cursorColumn);
            cell.text = std::string{glyph};
            cell.foreground = fg;
            cell.background = bg;
        }
        cursorColumn += width == 0 ? 0 : width;
        i += length;
    }
    return screen;
}

bool colorEq(Rgb const& a, ssg::SrgbColor const& b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

// ---------------------------------------------------------------------------
// Fixtures.
// ---------------------------------------------------------------------------

fs::path makeFixture() {
    auto root = fs::temp_directory_path() /
                ("ssg-parity-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "workspace");
    std::ofstream{root / "workspace" / "alpha.txt", std::ios::binary}
        << "first line\nsecond line\nthird line\n";
    return root;
}

// A headless production runtime over the fixture workspace, fully enriched (tree
// scanned), matching the app's FINAL frame after it primes deferred enrichment.
// `startsOnANewBuffer` mirrors apps/ssg_main.cpp opening an unnamed buffer when
// no file was opened at startup. Tests that go on to open a real file leave it
// false, because in that case the app opens the file instead.
std::unique_ptr<ssg::EditorSession> makeHeadless(fs::path const& root,
                                                 bool startsOnANewBuffer = false) {
    ssg::EditorSessionConfig config;
    config.cwd = root / "workspace";
    config.scratchRoot = root / "scratch";
    config.recoveryRoot = root / "recovery";
    auto created = ssg::EditorSession::create(config);
    if (!created.accepted()) return nullptr;
    auto runtime = std::move(created.session);
    (void)runtime->attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                          ssg::ViewId{1});
    // The app opens its startup document BEFORE showing the sidebar, so the
    // fixture must too or the final frame differs.
    if (startsOnANewBuffer) {
        (void)runtime->dispatch(ssg::ClientId{1},
                                {"file.new", runtime->revision(), {}});
    }
    // apps/ssg_main.cpp opens the Files sidebar at startup ONLY when it did not
    // open a file by name -- over a file the user asked for, the sidebar just
    // covers the text they came to edit.  `startsOnANewBuffer` is exactly that
    // case here, so mirror it or this fixture stops matching the app's frame.
    if (startsOnANewBuffer) {
        (void)runtime->dispatch(ssg::ClientId{1},
                                {"panel.show_files", runtime->revision(), {}});
        // Showing the sidebar moves focus to the panel, and the app re-asserts
        // editor focus afterwards whenever startup left something editable. The
        // caret position differs otherwise, which this fixture's check sees.
        runtime->focusEditor();
    }
    return runtime;
}

// Wait for a pty child without ever blocking forever.  An unconditional waitpid
// turns a child that regresses into a hang into a hung SUITE, which reports
// nothing useful; escalating to SIGKILL keeps the failure a bounded assertion.
void reapBounded(pid_t pid) {
    ::kill(pid, SIGTERM);
    auto const deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (::waitpid(pid, &status, WNOHANG) == pid) return;
        ::usleep(10000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
}

// Launch the real `ssg` binary over `launch_path` (a workspace dir or a file)
// under a pty and capture its output until it settles (frames drawn, blocked on
// input).
std::string captureFrames(std::string const& binary, fs::path const& launchPath) {
    winsize ws{};
    ws.ws_col = 80;
    ws.ws_row = 24;
    int master = -1;
    pid_t const pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) return {};
    if (pid == 0) {
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM", "xterm-256color", 1);
        execl(binary.c_str(), binary.c_str(), launchPath.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL, 0) | O_NONBLOCK);

    std::string output;
    auto const hardDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{4};
    auto lastData = std::chrono::steady_clock::now();
    char buffer[4096];
    while (std::chrono::steady_clock::now() < hardDeadline) {
        pollfd pfd{master, POLLIN, 0};
        ::poll(&pfd, 1, 50);
        bool got = false;
        for (;;) {
            auto const count = ::read(master, buffer, sizeof buffer);
            if (count > 0) {
                output.append(buffer, static_cast<std::size_t>(count));
                got = true;
            } else {
                break;
            }
        }
        auto const now = std::chrono::steady_clock::now();
        if (got) lastData = now;
        // Settled: both frames drawn and quiet for a spell.
        else if (!output.empty() && now - lastData > std::chrono::milliseconds{400}) {
            break;
        }
    }
    reapBounded(pid);
    ::close(master);
    return output;
}

// Assert the decoded screen equals render(snapshot): every non-continuation cell
// matches text + resolved color, and every continuation cell decodes to a blank
// (the wide glyph advanced the cursor past it — this locks wide-glyph handling).
// Returns the number of content cells compared.
int compareScreen(DecodedScreen const& screen, ssg::CellGrid const& grid) {
    ASSERT_EQ(screen.columns, grid.size.columns);
    ASSERT_EQ(screen.rows, grid.size.rows);
    int compared = 0;
    for (int row = 0; row < grid.size.rows; ++row) {
        for (int column = 0; column < grid.size.columns; ++column) {
            auto const& cell =
                grid.cells[static_cast<std::size_t>(row) * grid.size.columns +
                           column];
            auto const& decoded = screen.at(row, column);
            if (cell.continuation) {
                ASSERT_EQ(decoded.text, std::string{" "});
                continue;
            }
            std::string const expected = cell.text.empty() ? " " : cell.text;
            ASSERT_EQ(decoded.text, expected);
            ASSERT_TRUE(colorEq(decoded.foreground, grid.colors[cell.foreground]));
            auto expectedBackground = grid.colors[cell.background];
            switch (cell.tint) {
                case ssg::DiffTint::AddedRow:
                    expectedBackground = grid.diffTints.addedRow;
                    break;
                case ssg::DiffTint::RemovedRow:
                    expectedBackground = grid.diffTints.removedRow;
                    break;
                case ssg::DiffTint::ModifiedRow:
                    expectedBackground = grid.diffTints.modifiedRow;
                    break;
                case ssg::DiffTint::AddedWord:
                    expectedBackground = grid.diffTints.addedWord;
                    break;
                case ssg::DiffTint::RemovedWord:
                    expectedBackground = grid.diffTints.removedWord;
                    break;
                case ssg::DiffTint::ModifiedWord:
                    expectedBackground = grid.diffTints.modifiedWord;
                    break;
                case ssg::DiffTint::None:
                    break;
            }
            ASSERT_TRUE(colorEq(decoded.background, expectedBackground));
            ++compared;
        }
    }
    return compared;
}

}  // namespace

// M11-2b: the independent decoder round-trips the app's encoded frame — proving
// the decoder faithfully models the renderer's output vocabulary before it is
// trusted against the real binary.  (This uses the encoder only as a decoder
// self-test, never as the parity oracle.)
TEST(decoderRoundtripsTheEncodedFrame) {
    auto root = makeFixture();
    auto runtime = makeHeadless(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"file.open", runtime->revision(),
                                   std::string{"alpha.txt"}})
                    .accepted());
    auto frame = ssg::test::projectGridFrame(
        *runtime, ssg::ClientId{1}, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(frame.has_value());
    if (!frame) { fs::remove_all(root); return; }
    auto grid = ssg::Renderer{}.render(*frame);

    auto encoded =
        ssg::app::encode_ansi_frame(grid, ssg::ColorDepth::Truecolor);
    auto screen = decode(encoded, grid.size.columns, grid.size.rows);
    compareScreen(screen, grid);
    fs::remove_all(root);
}

TEST(decoderRoundtripsOrthogonalTintBackgrounds) {
    ssg::CellGrid grid;
    grid.size = {2, 1};
    grid.colors[0] = {30, 30, 30};
    grid.colors[1] = {212, 212, 212};
    grid.diffTints.addedRow = {0, 0, 95};
    ssg::CellGridCell plain;
    plain.text = "X";
    plain.foreground = 1;
    ssg::CellGridCell tinted = plain;
    tinted.text = "Y";
    tinted.tint = ssg::DiffTint::AddedRow;
    grid.cells = {plain, tinted};

    const auto encoded =
        ssg::app::encode_ansi_frame(grid, ssg::ColorDepth::Truecolor);
    const auto decoded = decode(encoded, grid.size.columns, grid.size.rows);
    ASSERT_EQ(compareScreen(decoded, grid), 2);
}

TEST(canonicalIncludesTintedBlankCells) {
    ssg::CellGrid grid;
    grid.size = {1, 1};
    grid.cells.resize(1);
    grid.cells.front().tint = ssg::DiffTint::AddedRow;
    ASSERT_TRUE(grid.canonical().find("cell 0 0") != std::string::npos);
    ASSERT_TRUE(grid.canonical().find("tint 1") != std::string::npos);
}

// M11-2a: the real `ssg` binary's pty output, decoded independently, equals
// render(snapshot) for the same headless session — text, resolved color, and
// cursor.  The app adds no screen content.
TEST(realBinaryOutputMatchesRenderSnapshot) {
    auto root = makeFixture();
    auto workspace = root / "workspace";

    auto output = captureFrames(SSG_APP_BINARY, workspace);
    ASSERT_TRUE(!output.empty());
    if (output.empty()) { fs::remove_all(root); return; }
    auto screen = decode(output, 80, 24);

    auto runtime = makeHeadless(root, true);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) { fs::remove_all(root); return; }
    auto frame = ssg::test::projectGridFrame(
        *runtime, ssg::ClientId{1}, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(frame.has_value());
    if (!frame) { fs::remove_all(root); return; }
    auto grid = ssg::Renderer{}.render(*frame);

    ASSERT_TRUE(compareScreen(screen, grid) > 0);

    // The captured hardware cursor equals the rendered caret.
    ASSERT_EQ(screen.cursor.has_value(), grid.caret.has_value());
    if (screen.cursor && grid.caret) {
        ASSERT_EQ(screen.cursor->first, grid.caret->row);
        ASSERT_EQ(screen.cursor->second, grid.caret->column);
    }
    fs::remove_all(root);
}

// M11-2a, wide glyphs: open a document of wide (CJK) text in the REAL binary and
// assert the decoded screen still equals render(snapshot).  This exercises the
// decoder's wide-glyph advance and the encoder's continuation-cell handling
// end-to-end (the ASCII case above never advances the cursor by two).
TEST(realBinaryWideGlyphOutputMatchesRender) {
    auto root = makeFixture();
    auto wide = root / "workspace" / "wide.txt";
    std::ofstream{wide, std::ios::binary}
        << "\xe6\xbc\xa2\xe5\xad\x97\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e ascii\n"
        << "second line\n";  // U+6F22 U+5B57 U+65E5 U+672C U+8A9E ("漢字日本語")

    auto output = captureFrames(SSG_APP_BINARY, wide);
    ASSERT_TRUE(!output.empty());
    if (output.empty()) { fs::remove_all(root); return; }
    auto screen = decode(output, 80, 24);

    auto runtime = makeHeadless(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) { fs::remove_all(root); return; }
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"file.open", runtime->revision(),
                                   std::string{"wide.txt"}})
                    .accepted());
    // The real binary above was launched with wide.txt as its file
    // ARGUMENT (captureFrames(SSG_APP_BINARY, wide)), so apps/ssg_main.cpp's
    // startup focuses the editor after opening it (see "Always open Files
    // sidebar at startup"); mirror that here so the reference matches.
    runtime->focusEditor();
    auto frame = ssg::test::projectGridFrame(
        *runtime, ssg::ClientId{1}, ssg::ViewId{1}, {80, 24});
    ASSERT_TRUE(frame.has_value());
    if (!frame) { fs::remove_all(root); return; }
    auto grid = ssg::Renderer{}.render(*frame);

    // Sanity: the rendered document actually contains wide (continuation) cells,
    // so this case genuinely exercises wide-glyph handling.
    bool hasContinuation = false;
    for (auto const& cell : grid.cells) {
        if (cell.continuation) { hasContinuation = true; break; }
    }
    ASSERT_TRUE(hasContinuation);

    ASSERT_TRUE(compareScreen(screen, grid) > 0);
    fs::remove_all(root);
}

// Oracle (INV-startup-unblocked, INV-reply-never-input) against the REAL binary.
//
// Two things are only observable end to end.  First, that no capability query
// delays the first frame: a terminal is simulated that holds its DA1 answer back
// far longer than any real one would, and the frame must already be on screen
// before that answer is written.  The ordering is decided by the code rather than
// by a race -- the reply provably has not been sent when the frame is observed.
//
// Second, that a reply never reaches the document.  The answer is written LATE,
// after the probe window has closed, which is the case most likely to be treated
// as ordinary typing; its bytes must appear nowhere in any later frame.
TEST(theFirstFrameIsWrittenBeforeAnyReplyIsRead) {
    auto const root = fs::temp_directory_path() /
                      ("ssg-probe-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "workspace");
    // A marker that cannot occur in terminal setup bytes, so seeing it in the
    // output means a rendered content frame and not merely mode-setting.
    std::string const marker = "CAPABILITYPROBEMARKER";
    std::ofstream{root / "workspace" / "alpha.txt", std::ios::binary}
        << marker << "\n";

    winsize ws{};
    ws.ws_col = 80;
    ws.ws_row = 24;
    int master = -1;
    pid_t const pid = forkpty(&master, nullptr, nullptr, &ws);
    ASSERT_TRUE(pid >= 0);
    if (pid < 0) { fs::remove_all(root); return; }
    if (pid == 0) {
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM", "xterm-256color", 1);
        auto const file = (root / "workspace" / "alpha.txt").string();
        execl(SSG_APP_BINARY, SSG_APP_BINARY, file.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL, 0) | O_NONBLOCK);

    // A real terminal answers within a round trip; this one stalls, standing in
    // for a terminal that is slow, wedged, or not a terminal at all.
    constexpr auto kReplyDelay = std::chrono::milliseconds{1500};
    std::string const reply = "\x1b[?62;22c";
    auto const start = std::chrono::steady_clock::now();

    std::string output;
    std::optional<std::chrono::steady_clock::time_point> frameSeen;
    bool replySent = false;
    std::size_t outputAtReply = 0;
    char buffer[4096];
    auto const deadline = start + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{master, POLLIN, 0};
        ::poll(&pfd, 1, 20);
        for (;;) {
            auto const count = ::read(master, buffer, sizeof buffer);
            if (count <= 0) break;
            output.append(buffer, static_cast<std::size_t>(count));
        }
        if (!frameSeen && output.find(marker) != std::string::npos) {
            frameSeen = std::chrono::steady_clock::now();
        }
        auto const now = std::chrono::steady_clock::now();
        if (!replySent && now - start >= kReplyDelay) {
            outputAtReply = output.size();
            ASSERT_EQ(::write(master, reply.data(), reply.size()),
                      static_cast<ssize_t>(reply.size()));
            replySent = true;
        }
        // Give the app time to mishandle the late reply if it is going to.
        if (replySent && now - start >= kReplyDelay + std::chrono::milliseconds{600}) {
            break;
        }
    }
    reapBounded(pid);
    ::close(master);

    // The queries were actually written -- otherwise the rest proves nothing.
    ASSERT_TRUE(output.find("\x1b[c") != std::string::npos);
    ASSERT_TRUE(output.find("\x1b[?2026$p") != std::string::npos);

    // The frame was on screen before the answer was even sent.
    ASSERT_TRUE(frameSeen.has_value());
    ASSERT_TRUE(replySent);
    if (frameSeen) {
        ASSERT_TRUE(*frameSeen - start < kReplyDelay);
    }

    // Nothing the terminal answered was ever typed into the document.  Only
    // output produced AFTER the reply was written can carry it.
    auto const afterReply = output.substr(outputAtReply);
    ASSERT_TRUE(afterReply.find("62;22c") == std::string::npos);
    ASSERT_TRUE(output.find(marker + "62") == std::string::npos);

    fs::remove_all(root);
}

// Oracle for the `--capabilities` diagnostic: run the real binary against a
// simulated terminal that answers everything, and check the report says so.
// Without this the diagnostic could confidently print "no" for a capable
// terminal, which is worse than having no diagnostic -- a user would take it as
// evidence and stop looking.
TEST(theCapabilitiesReportReflectsWhatTheTerminalAnswered) {
    winsize ws{};
    ws.ws_col = 80;
    ws.ws_row = 24;
    int master = -1;
    pid_t const pid = forkpty(&master, nullptr, nullptr, &ws);
    ASSERT_TRUE(pid >= 0);
    if (pid < 0) return;
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        // Forced, so the reported depth proves the diagnostic surfaces the
        // resolved value including overrides rather than echoing a hint.
        setenv("SSG_COLOR_DEPTH", "ansi16", 1);
        execl(SSG_APP_BINARY, SSG_APP_BINARY, "--capabilities",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL, 0) | O_NONBLOCK);

    std::string output;
    bool answered = false;
    bool eof = false;
    char buffer[4096];
    auto const deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!eof && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{master, POLLIN, 0};
        ::poll(&pfd, 1, 10);
        for (;;) {
            auto const count = ::read(master, buffer, sizeof buffer);
            if (count == 0) {  // EOF: the child exited and closed the pty.
                eof = true;
                break;
            }
            if (count < 0) {
                // On Linux a pty master read after the slave closes reports EIO
                // rather than 0, so treat it (and any non-transient error) as
                // end-of-output.  EAGAIN/EWOULDBLOCK ("nothing right now") and
                // EINTR (a signal interrupted the read) are transient: keep
                // polling rather than ending the drain early, which would
                // reintroduce the partial-report race.
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    eof = true;
                }
                break;
            }
            output.append(buffer, static_cast<std::size_t>(count));
        }
        // Answer as soon as the queries arrive, the way a real terminal would:
        // the speculative answers first, then the DA1 fence advertising OSC 52.
        if (!answered && output.find("\x1b[c") != std::string::npos) {
            std::string const replies =
                "\x1b[?2026;2$y"
                "\x1b[?1u"
                "\x1b[?62;4;52c";
            auto const written =
                ::write(master, replies.data(), replies.size());
            ASSERT_EQ(written, static_cast<ssize_t>(replies.size()));
            answered = true;
        }
        // Drain the ENTIRE report before asserting: the `replies:` section the
        // assertions below check is printed AFTER the capability lines, so
        // breaking on a mid-report marker (e.g. "clipboard_write") would race
        // the later output that has not been read yet.
    }
    reapBounded(pid);
    ::close(master);

    ASSERT_TRUE(answered);
    ASSERT_TRUE(output.find("synchronized_output      yes") != std::string::npos);
    ASSERT_TRUE(output.find("keyboard_protocol        yes") != std::string::npos);
    ASSERT_TRUE(output.find("clipboard_write          yes") != std::string::npos);
    // The replies must not be ECHOED as raw bytes (that would be the terminal
    // typing into the report), but they must appear in the log in escaped form,
    // which is what makes a reported "no" explainable.
    ASSERT_TRUE(output.find("\x1b[?62;4;52c") == std::string::npos);
    ASSERT_TRUE(output.find("<ESC>[?62;4;52c") != std::string::npos);
    ASSERT_TRUE(output.find("<ESC>[?1u") != std::string::npos);
    // The depth was forced by SSG_COLOR_DEPTH, and the report shows the resolved
    // value rather than what TERM alone would have implied (truecolor).
    ASSERT_TRUE(output.find("color_depth              ansi16") !=
                std::string::npos);
}

int main() {
    RUN(decoderRoundtripsTheEncodedFrame);
    RUN(decoderRoundtripsOrthogonalTintBackgrounds);
    RUN(canonicalIncludesTintedBlankCells);
    RUN(realBinaryOutputMatchesRenderSnapshot);
    RUN(realBinaryWideGlyphOutputMatchesRender);
    RUN(theFirstFrameIsWrittenBeforeAnyReplyIsRead);
    RUN(theCapabilitiesReportReflectsWhatTheTerminalAnswered);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
