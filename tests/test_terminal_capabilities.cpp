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

ssg::TerminalCapabilities::EnvironmentLookup fakeEnvironment(
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
    ssg::TerminalCapabilities rich{fakeEnvironment({})};
    (void)rich.beginProbe();
    rich.observeReply("\x1b[?2026;2$y");
    rich.observeReply("\x1b[?1u");
    rich.observeReply("\x1b[?62;4;52c");
    ASSERT_TRUE(rich.has(ssg::Capability::SynchronizedOutput));
    ASSERT_TRUE(rich.has(ssg::Capability::KeyboardProtocol));
    ASSERT_TRUE(rich.has(ssg::Capability::ClipboardWrite));
    ASSERT_FALSE(rich.probing());  // The fence closed the window.

    // A terminal that answers only the fence: everything else is absent, and
    // nothing waited to find that out.
    ssg::TerminalCapabilities plain{fakeEnvironment({})};
    (void)plain.beginProbe();
    plain.observeReply("\x1b[?62;22c");
    for (auto const capability : ssg::kAllCapabilities) {
        ASSERT_FALSE(plain.has(capability));
    }
    ASSERT_FALSE(plain.probing());

    // A terminal that answers nothing at all: still absent, still not waiting.
    ssg::TerminalCapabilities silent{fakeEnvironment({})};
    (void)silent.beginProbe();
    silent.endProbe();
    for (auto const capability : ssg::kAllCapabilities) {
        ASSERT_FALSE(silent.has(capability));
    }

    // A terminal that knows mode 2026 but reports it unrecognized (state 0), and
    // one that recognizes it but can never enable it (state 4, permanently
    // reset), are both absent.  States 1/2/3 are usable.
    for (auto const* const unusable : {"\x1b[?2026;0$y", "\x1b[?2026;4$y"}) {
        ssg::TerminalCapabilities capabilities{fakeEnvironment({})};
        (void)capabilities.beginProbe();
        capabilities.observeReply(unusable);
        ASSERT_FALSE(capabilities.has(ssg::Capability::SynchronizedOutput));
    }
    for (auto const* const usable :
         {"\x1b[?2026;1$y", "\x1b[?2026;2$y", "\x1b[?2026;3$y"}) {
        ssg::TerminalCapabilities capabilities{fakeEnvironment({})};
        (void)capabilities.beginProbe();
        capabilities.observeReply(usable);
        ASSERT_TRUE(capabilities.has(ssg::Capability::SynchronizedOutput));
    }
}

// Re-probing asks a terminal that may not be the one that answered last time --
// a resumed session, a reattached multiplexer.  An answer from the old terminal
// must not survive the fence that is supposed to be able to retire it.
TEST(reprobingDoesNotCarryStaleAnswersForward) {
    ssg::TerminalCapabilities capabilities{fakeEnvironment({})};
    (void)capabilities.beginProbe();
    capabilities.observeReply("\x1b[?2026;2$y");
    capabilities.observeReply("\x1b[?1u");
    capabilities.observeReply("\x1b[?62;4;52c");
    for (auto const capability : ssg::kAllCapabilities) {
        ASSERT_TRUE(capabilities.has(capability));
    }

    // The new terminal answers only the fence: every previous answer is retired.
    (void)capabilities.beginProbe();
    capabilities.observeReply("\x1b[?62;22c");
    for (auto const capability : ssg::kAllCapabilities) {
        ASSERT_FALSE(capabilities.has(capability));
    }
}

// Oracle (the probe window): a reply shape arriving after the fence -- pasted by
// the user, or emitted by some protocol adopted later -- must not reconfigure the
// editor.  Consumption is unconditional; belief is not.
TEST(aReplyShapeAfterTheFenceIsNotACapabilityAnswer) {
    ssg::TerminalCapabilities capabilities{fakeEnvironment({})};
    (void)capabilities.beginProbe();
    capabilities.observeReply("\x1b[?62;22c");  // The fence closes the window.
    capabilities.observeReply("\x1b[?2026;2$y");
    capabilities.observeReply("\x1b[?1u");
    ASSERT_FALSE(capabilities.has(ssg::Capability::SynchronizedOutput));
    ASSERT_FALSE(capabilities.has(ssg::Capability::KeyboardProtocol));

    // Nor before the queries were ever written.
    ssg::TerminalCapabilities unprobed{fakeEnvironment({})};
    unprobed.observeReply("\x1b[?1u");
    ASSERT_FALSE(unprobed.has(ssg::Capability::KeyboardProtocol));
}

// Oracle (precedence): the override exists because terminals lie, so it has to
// beat the terminal's own answer in both directions.
TEST(anOverrideBeatsTheTerminalsOwnAnswer) {
    ssg::TerminalCapabilities forcedOff{
        fakeEnvironment({{"SSG_TERM_SYNCHRONIZED_OUTPUT", "off"}})};
    (void)forcedOff.beginProbe();
    forcedOff.observeReply("\x1b[?2026;2$y");  // The terminal says yes.
    ASSERT_FALSE(forcedOff.has(ssg::Capability::SynchronizedOutput));

    ssg::TerminalCapabilities forcedOn{
        fakeEnvironment({{"SSG_TERM_KEYBOARD_PROTOCOL", "1"}})};
    (void)forcedOn.beginProbe();
    forcedOn.observeReply("\x1b[?62;22c");  // The terminal never answered.
    ASSERT_TRUE(forcedOn.has(ssg::Capability::KeyboardProtocol));

    // An unparseable override defers to the terminal rather than forcing a guess.
    ssg::TerminalCapabilities garbage{
        fakeEnvironment({{"SSG_TERM_CLIPBOARD_WRITE", "perhaps"}})};
    (void)garbage.beginProbe();
    garbage.observeReply("\x1b[?62;4;52c");
    ASSERT_TRUE(garbage.has(ssg::Capability::ClipboardWrite));

    // Color depth is resolved by the same object, from the same environment.
    ssg::TerminalCapabilities colored{
        fakeEnvironment({{"COLORTERM", "truecolor"}, {"TERM", "xterm"}})};
    ASSERT_TRUE(colored.colorDepth() == ssg::ColorDepth::Truecolor);
    ssg::TerminalCapabilities dumb{fakeEnvironment({{"TERM", "dumb"}})};
    ASSERT_TRUE(dumb.colorDepth() == ssg::ColorDepth::Ansi16);
}

// Every capability must be reachable by an override, or a user hitting a
// rendering bug in one of them has no escape hatch.  Enumerated from the enum so
// a capability added later cannot quietly skip its override.
TEST(everyCapabilityHasAWorkingOverride) {
    for (auto const capability : ssg::kAllCapabilities) {
        std::string variable = "SSG_TERM_";
        for (char const ch : ssg::capabilityName(capability)) {
            variable.push_back(
                static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
        ssg::TerminalCapabilities forced{fakeEnvironment({{variable, "on"}})};
        ASSERT_TRUE(forced.has(capability));
        ssg::TerminalCapabilities suppressed{
            fakeEnvironment({{variable, "off"}})};
        ASSERT_FALSE(suppressed.has(capability));
    }
}

// The queries must be answerable by the parser that reads their replies, and the
// fence must be written last or it cannot fence anything.
TEST(theProbeAsksOnlyQuestionsItCanUnderstand) {
    ssg::TerminalCapabilities capabilities{fakeEnvironment({})};
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
            ssg::decodeInput(std::string_view{queries}.substr(offset), true,
                                   consumed);
        ASSERT_TRUE(decoded.status != ssg::DecodeStatus::incomplete);
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

    ssg::TerminalCapabilities capabilities{fakeEnvironment({}), clock};
    (void)capabilities.beginProbe();
    ASSERT_TRUE(capabilities.probing());

    // Just inside the window: still believed.
    now += ssg::TerminalCapabilities::kProbeWindow -
           std::chrono::milliseconds{1};
    ASSERT_TRUE(capabilities.probing());
    capabilities.observeReply("\x1b[?1u");
    ASSERT_TRUE(capabilities.has(ssg::Capability::KeyboardProtocol));

    // Past the window, with no loop having run in between: not believed.
    ssg::TerminalCapabilities late{fakeEnvironment({}), clock};
    (void)late.beginProbe();
    now += ssg::TerminalCapabilities::kProbeWindow +
           std::chrono::milliseconds{1};
    ASSERT_FALSE(late.probing());
    late.observeReply("\x1b[?1u");
    late.observeReply("\x1b[?2026;2$y");
    ASSERT_FALSE(late.has(ssg::Capability::KeyboardProtocol));
    ASSERT_FALSE(late.has(ssg::Capability::SynchronizedOutput));
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
    for (auto const capability : ssg::kAllCapabilities) {
        std::string variable = "SSG_TERM_";
        for (char const ch : ssg::capabilityName(capability)) {
            variable.push_back(
                static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
        ASSERT_TRUE(doc.find(variable) != std::string::npos);
    }
    ASSERT_TRUE(doc.find("SSG_COLOR_DEPTH") != std::string::npos);
}

// Field-captured replies, kept verbatim because a hand-written reply would not
// have exercised a thirteen-parameter DA1 whose class code (61) sits where an
// extension could, nor a real terminal's choice of DECRPM state.
TEST(realTerminalRepliesResolveAsObserved) {
    // Windows Terminal 1.24 over ssh.
    ssg::TerminalCapabilities windowsTerminal{fakeEnvironment({})};
    (void)windowsTerminal.beginProbe();
    windowsTerminal.observeReply("\x1b[?2026;2$y");
    windowsTerminal.observeReply(
        "\x1b[?61;4;6;7;14;21;22;23;24;28;32;42;52c");
    ASSERT_TRUE(windowsTerminal.has(ssg::Capability::ClipboardWrite));
    // DECRPM state 2 is "reset": the terminal knows the mode, which is what was
    // asked.  Reading state 2 as unsupported would report no on a terminal that
    // demonstrably has it.
    ASSERT_TRUE(windowsTerminal.has(ssg::Capability::SynchronizedOutput));
    // It answered nothing about the keyboard protocol, so it does not have it
    // (1.24 predates the release that implements it).
    ASSERT_FALSE(windowsTerminal.has(ssg::Capability::KeyboardProtocol));
    // xterm.js: a minimal DA1 with no extensions, and no answer to either
    // speculative question.
    ssg::TerminalCapabilities xtermJs{fakeEnvironment({})};
    (void)xtermJs.beginProbe();
    xtermJs.observeReply("\x1b[?1;2c");
    for (auto const capability : ssg::kAllCapabilities) {
        ASSERT_FALSE(xtermJs.has(capability));
    }

    // The terminal class must not be mistaken for an extension: a terminal whose
    // class happens to be 52 advertises no clipboard by saying so.
    ssg::TerminalCapabilities classFiftyTwo{fakeEnvironment({})};
    (void)classFiftyTwo.beginProbe();
    classFiftyTwo.observeReply("\x1b[?52;1c");
    ASSERT_FALSE(classFiftyTwo.has(ssg::Capability::ClipboardWrite));
}

SSG_TEST_SUITE(test_terminal_capabilities) {
    RUN(onlyADa1ReplyMeansTheFeatureIsAbsent);
    RUN(reprobingDoesNotCarryStaleAnswersForward);
    RUN(aReplyShapeAfterTheFenceIsNotACapabilityAnswer);
    RUN(anOverrideBeatsTheTerminalsOwnAnswer);
    RUN(everyCapabilityHasAWorkingOverride);
    RUN(theProbeAsksOnlyQuestionsItCanUnderstand);
    RUN(aReplyArrivingAfterTheWindowExpiresIsNotBelieved);
    RUN(configDocDocumentsEveryCapabilityOverride);
    RUN(realTerminalRepliesResolveAsObserved);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
