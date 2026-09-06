#include <ssg/Terminal.h>
#include <ssg/TerminalCapabilities.h>
#include <ssg/TerminalInput.h>
#include <ssg/TerminalOutput.h>
#include <ssg/pointer_routing.h>

#include <ssg/Editor.h>
#include <ssg/focus.h>
#include <ssg/HitTester.h>
#include <ssg/PromptSurface.h>
#include <ssg/Renderer.h>
#include <ssg/ScriptHost.h>
#include <ssg/Selection.h>

#include <ssg/InitScriptWatcher.h>

#include "test_helpers.h"
#include "grid_test_frame.h"

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
    struct Named { char const* name; ssg::TerminalMode mode; };
    Named const declared[]{
        {"alternate screen", ssg::kAlternateScreen},
        {"cursor style", ssg::kCursorStyleBar},
        {"mouse buttons", ssg::kMouseButtons},
        {"mouse motion", ssg::kMouseMotion},
        {"mouse SGR coordinates", ssg::kMouseSgrCoordinates},
        {"cursor hidden", ssg::kCursorHidden},
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
    ssg::TerminalModes modes{
        [&written](std::string_view bytes) { written.append(bytes); }};
    {
        auto alt = modes.enter(ssg::kAlternateScreen);
        auto cursor = modes.enter(ssg::kCursorStyleBar);
        auto buttons = modes.enter(ssg::kMouseButtons);
        auto motion = modes.enter(ssg::kMouseMotion);
        auto sgr = modes.enter(ssg::kMouseSgrCoordinates);
        ASSERT_EQ(written, std::string{kExpectedSetup});
        written.clear();
    }
    ASSERT_EQ(written, std::string{kExpectedRestore});
    ASSERT_EQ(modes.depth(), std::size_t{0});
}

TEST(everyEnteredModeIsLeftInReverseOrder) {
    std::string written;
    ssg::TerminalModes modes{
        [&written](std::string_view bytes) { written.append(bytes); }};
    {
        auto outer = modes.enter(ssg::kAlternateScreen);
        auto inner = modes.enter(ssg::kMouseButtons);
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
    // what is currently entered: see allModesUndoSequence for why precision
    // there cannot be published safely, and why over-approximating is the safe
    // direction.
    //
    // Assembled from the one list, reversed, rather than by asking the function
    // how it built itself.
    std::string expected;
    for (std::size_t i = std::size(ssg::kAllModes); i-- > 0;) {
        expected.append(ssg::kAllModes[i].leave);
    }
    auto const undo = std::string{ssg::allModesUndoSequence()};
    ASSERT_EQ(undo, expected);

    // And the list itself must hold every mode DECLARED, or a mode could be
    // declared, entered, and left out of the crash undo. Counted from the
    // header, so this does not just re-read the list it is checking.
    std::ifstream header{std::string{SSG_TEST_SOURCE_DIR} +
                         "/include/ssg/Terminal.h"};
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
    ASSERT_EQ(declared, std::size(ssg::kAllModes));

    // The non-idempotent Kitty pop must NOT appear in the constant crash-undo.
    ASSERT_TRUE(undo.find(std::string{ssg::kKeyboardProtocol.leave}) ==
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
        ssg::TerminalModes modes{
            [&](std::string_view bytes) { written.append(bytes); }};
        auto guard = modes.enter(ssg::kKeyboardProtocol);
        ASSERT_EQ(written, std::string{ssg::kKeyboardProtocol.enter});
    }
    ASSERT_EQ(written, std::string{ssg::kKeyboardProtocol.enter} +
                           std::string{ssg::kKeyboardProtocol.leave});
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
    ASSERT_TRUE(balanced(ssg::encodeFrame(withCaret,
                                                ssg::ColorDepth::Truecolor)));

    ssg::CellGrid withoutCaret;
    withoutCaret.size = {4, 2};
    withoutCaret.cells.resize(8);
    withoutCaret.caret.reset();
    ASSERT_TRUE(balanced(ssg::encodeFrame(withoutCaret,
                                                ssg::ColorDepth::Truecolor)));

    // And the hide still comes first, so the redraw itself is not visible.
    auto const frame =
        ssg::encodeFrame(withoutCaret, ssg::ColorDepth::Truecolor);
    ASSERT_TRUE(frame.find("\x1b[?25l") == 0);
}

TEST(classifySignalTagsMapsSignalNumbers) {
    // Empty drain: no events.
    auto none = ssg::classifySignalTags({});
    ASSERT_FALSE(none.resize);
    ASSERT_FALSE(none.terminate.has_value());

    // A single SIGWINCH byte sets resize only.
    std::string winch(1, static_cast<char>(SIGWINCH));
    auto resize = ssg::classifySignalTags(winch);
    ASSERT_TRUE(resize.resize);
    ASSERT_FALSE(resize.terminate.has_value());

    // Duplicate resize tags coalesce to a single resize event.
    std::string winches(5, static_cast<char>(SIGWINCH));
    auto coalesced = ssg::classifySignalTags(winches);
    ASSERT_TRUE(coalesced.resize);

    // SIGTERM sets terminate carrying the exact signal for a correct re-raise.
    std::string term(1, static_cast<char>(SIGTERM));
    auto terminate = ssg::classifySignalTags(term);
    ASSERT_TRUE(terminate.terminate.has_value());
    ASSERT_EQ(*terminate.terminate, SIGTERM);

    // Mixed drain: resize is set and the last terminating signal wins.
    std::string mixed;
    mixed.push_back(static_cast<char>(SIGWINCH));
    mixed.push_back(static_cast<char>(SIGTERM));
    mixed.push_back(static_cast<char>(SIGHUP));
    auto both = ssg::classifySignalTags(mixed);
    ASSERT_TRUE(both.resize);
    ASSERT_TRUE(both.terminate.has_value());
    ASSERT_EQ(*both.terminate, SIGHUP);

    // Unknown bytes are ignored (total function).
    std::string junk(1, static_cast<char>(7));
    auto ignored = ssg::classifySignalTags(junk);
    ASSERT_FALSE(ignored.resize);
    ASSERT_FALSE(ignored.terminate.has_value());
}

SSG_TEST_SUITE(test_terminal) {
    RUN(everyDeclaredModeLeavesExactlyWhatItEnters);
    RUN(modeStackReproducesTheCuratedSetupAndRestoreSequences);
    RUN(everyEnteredModeIsLeftInReverseOrder);
    RUN(theCrashUndoLeavesEveryDeclaredModeInReverseOrder);
    RUN(kittyKeyboardModeRoundTripsThroughAGuard);
    RUN(aFrameWithNoCaretLeavesTheCursorVisible);
    RUN(classifySignalTagsMapsSignalNumbers);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
