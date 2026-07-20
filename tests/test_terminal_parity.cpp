// Milestone 11 — M11-2: the app is transport only (doc/spec-library-contract.md,
// INV-app-transport-only).
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

#include <ssg/editor_runtime.h>
#include <ssg/render.h>
#include <ssg/session_snapshot.h>
#include <ssg/theme.h>

#include "ssg_terminal.h"  // encode_ansi_frame, for the decoder round-trip only.
#include "test_helpers.h"

#include <pty.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

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

std::size_t utf8_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xF0) return 4;
    if (lead >= 0xE0) return 3;
    if (lead >= 0xC0) return 2;
    return 1;
}

std::uint32_t utf8_codepoint(std::string_view text) {
    auto const lead = static_cast<unsigned char>(text[0]);
    std::size_t const length = utf8_length(lead);
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
int codepoint_width(std::uint32_t cp) {
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

std::vector<int> split_params(std::string_view params) {
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

    int cursor_row = 0;
    int cursor_column = 0;
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
            bool const private_mode = !params.empty() && params.front() == '?';

            if (final == 'H' || final == 'f') {
                auto values = split_params(params);
                int const r = values.size() > 0 ? values[0] : 1;
                int const c = values.size() > 1 ? values[1] : 1;
                cursor_row = r > 0 ? r - 1 : 0;
                cursor_column = c > 0 ? c - 1 : 0;
            } else if (final == 'm') {
                auto values = split_params(params);
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
            } else if (private_mode && (final == 'h' || final == 'l') &&
                       params == "?25") {
                if (final == 'h') {
                    screen.cursor = std::pair<int, int>{cursor_row, cursor_column};
                } else {
                    screen.cursor.reset();
                }
            }
            // All other CSI (private modes, erase, cursor shape) affect no cell.
            i = j + 1;
            continue;
        }
        if (ch < 0x20) { ++i; continue; }  // Bare control byte: ignore.

        std::size_t const length = utf8_length(ch);
        if (i + length > n) break;
        std::string_view const glyph = bytes.substr(i, length);
        int const width = codepoint_width(utf8_codepoint(glyph));
        if (cursor_row >= 0 && cursor_row < rows && cursor_column >= 0 &&
            cursor_column < columns && width > 0) {
            auto& cell = screen.at(cursor_row, cursor_column);
            cell.text = std::string{glyph};
            cell.foreground = fg;
            cell.background = bg;
        }
        cursor_column += width == 0 ? 0 : width;
        i += length;
    }
    return screen;
}

bool color_eq(Rgb const& a, ssg::SrgbColor const& b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

// ---------------------------------------------------------------------------
// Fixtures.
// ---------------------------------------------------------------------------

fs::path make_fixture() {
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
std::unique_ptr<ssg::EditorRuntime> make_headless(fs::path const& root) {
    ssg::EditorRuntimeConfig config;
    config.cwd = root / "workspace";
    config.scratch_root = root / "scratch";
    config.recovery_root = root / "recovery";
    auto created = ssg::EditorRuntime::create(config);
    if (!created.accepted()) return nullptr;
    auto runtime = std::move(created.runtime);
    (void)runtime->attach({ssg::ClientId{1}, ssg::InvocationOrigin::InProcess},
                          ssg::ViewId{1});
    return runtime;
}

// Launch the real `ssg` binary over `launch_path` (a workspace dir or a file)
// under a pty and capture its output until it settles (frames drawn, blocked on
// input).
std::string capture_frames(std::string const& binary, fs::path const& launch_path) {
    winsize ws{};
    ws.ws_col = 80;
    ws.ws_row = 24;
    int master = -1;
    pid_t const pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) return {};
    if (pid == 0) {
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM", "xterm-256color", 1);
        execl(binary.c_str(), binary.c_str(), launch_path.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL, 0) | O_NONBLOCK);

    std::string output;
    auto const hard_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{4};
    auto last_data = std::chrono::steady_clock::now();
    char buffer[4096];
    while (std::chrono::steady_clock::now() < hard_deadline) {
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
        if (got) last_data = now;
        // Settled: both frames drawn and quiet for a spell.
        else if (!output.empty() && now - last_data > std::chrono::milliseconds{400}) {
            break;
        }
    }
    ::kill(pid, SIGTERM);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ::close(master);
    return output;
}

// Assert the decoded screen equals render(snapshot): every non-continuation cell
// matches text + resolved color, and every continuation cell decodes to a blank
// (the wide glyph advanced the cursor past it — this locks wide-glyph handling).
// Returns the number of content cells compared.
int compare_screen(DecodedScreen const& screen, ssg::CellGrid const& grid) {
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
            ASSERT_TRUE(color_eq(decoded.foreground, grid.palette[cell.foreground]));
            ASSERT_TRUE(color_eq(decoded.background, grid.palette[cell.background]));
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
TEST(decoder_roundtrips_the_encoded_frame) {
    auto root = make_fixture();
    auto runtime = make_headless(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) return;
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"file.open", runtime->revision(),
                                   std::string{"alpha.txt"}})
                    .accepted());
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) { fs::remove_all(root); return; }
    auto grid = ssg::render(*snapshot);

    auto encoded =
        ssg::app::encode_ansi_frame(grid, ssg::ColorDepth::Truecolor);
    auto screen = decode(encoded, grid.size.columns, grid.size.rows);
    compare_screen(screen, grid);
    fs::remove_all(root);
}

// M11-2a: the real `ssg` binary's pty output, decoded independently, equals
// render(snapshot) for the same headless session — text, resolved color, and
// cursor.  The app adds no screen content.
TEST(real_binary_output_matches_render_snapshot) {
    auto root = make_fixture();
    auto workspace = root / "workspace";

    auto output = capture_frames(SSG_APP_BINARY, workspace);
    ASSERT_TRUE(!output.empty());
    if (output.empty()) { fs::remove_all(root); return; }
    auto screen = decode(output, 80, 24);

    auto runtime = make_headless(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) { fs::remove_all(root); return; }
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) { fs::remove_all(root); return; }
    auto grid = ssg::render(*snapshot);

    ASSERT_TRUE(compare_screen(screen, grid) > 0);

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
TEST(real_binary_wide_glyph_output_matches_render) {
    auto root = make_fixture();
    auto wide = root / "workspace" / "wide.txt";
    std::ofstream{wide, std::ios::binary}
        << "\xe6\xbc\xa2\xe5\xad\x97\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e ascii\n"
        << "second line\n";  // U+6F22 U+5B57 U+65E5 U+672C U+8A9E ("漢字日本語")

    auto output = capture_frames(SSG_APP_BINARY, wide);
    ASSERT_TRUE(!output.empty());
    if (output.empty()) { fs::remove_all(root); return; }
    auto screen = decode(output, 80, 24);

    auto runtime = make_headless(root);
    ASSERT_TRUE(runtime != nullptr);
    if (!runtime) { fs::remove_all(root); return; }
    ASSERT_TRUE(runtime->dispatch(ssg::ClientId{1},
                                  {"file.open", runtime->revision(),
                                   std::string{"wide.txt"}})
                    .accepted());
    auto snapshot = runtime->snapshot(ssg::ClientId{1}, {80, 24});
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot) { fs::remove_all(root); return; }
    auto grid = ssg::render(*snapshot);

    // Sanity: the rendered document actually contains wide (continuation) cells,
    // so this case genuinely exercises wide-glyph handling.
    bool has_continuation = false;
    for (auto const& cell : grid.cells) {
        if (cell.continuation) { has_continuation = true; break; }
    }
    ASSERT_TRUE(has_continuation);

    ASSERT_TRUE(compare_screen(screen, grid) > 0);
    fs::remove_all(root);
}

int main() {
    RUN(decoder_roundtrips_the_encoded_frame);
    RUN(real_binary_output_matches_render_snapshot);
    RUN(real_binary_wide_glyph_output_matches_render);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
