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

TEST(aClickOnAnExternalActionRoutesThroughPointerTargetsToSelectThenAct) {
    // A click on an external-modification action first selects the runtime-minted
    // file (external.select), then runs the payload-less action on the library-
    // owned selection -- select-then-act, the one behavior path, with live (not
    // dead) hit regions.
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::ExternalAction;
    hit.externalFileId = "external:src/foo:bar.cpp";  // an id that contains ':'
    hit.commandId = "external.reload";
    ssg::PointerTargets targets;
    targets.external_invocation = ssg::ExternalActionInvocation{
        ssg::DiffFileId{*hit.externalFileId}, ssg::ExternalAction::Reload};
    auto plan = ssg::route_pointer(
        hit, ssg::PointerButton::left, ssg::PointerKind::press,
        false, false, std::nullopt, targets);
    ASSERT_TRUE(plan.semantic_input.has_value());
    auto const* input = std::get_if<ssg::ExternalActionPointerInput>(
        &*plan.semantic_input);
    ASSERT_TRUE(input != nullptr);
    if (input) {
        ASSERT_EQ(input->invocation, *targets.external_invocation);
    }
    ASSERT_TRUE(!plan.begins_drag);
}

TEST(decodeInputMapsPrintablesAndNamedKeys) {
    std::size_t consumed = 0;
    // Lowercase letter: KeyA stroke (no shift) plus committed text.
    auto a = ssg::decodeInput("a", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_TRUE(a.status == ssg::DecodeStatus::key);
    ASSERT_EQ(a.stroke.code, ssg::KeyCode::KeyA);
    ASSERT_FALSE(a.stroke.shift);
    ASSERT_EQ(a.text, std::string{"a"});

    // Uppercase: Shift+KeyZ plus committed text "Z".
    auto z = ssg::decodeInput("Z", true, consumed);
    ASSERT_EQ(z.stroke.code, ssg::KeyCode::KeyZ);
    ASSERT_TRUE(z.stroke.shift);
    ASSERT_EQ(z.text, std::string{"Z"});

    // Bracket punctuation used by tab chords.
    auto bracket = ssg::decodeInput("]", true, consumed);
    ASSERT_EQ(bracket.stroke.code, ssg::KeyCode::BracketRight);
    ASSERT_EQ(bracket.text, std::string{"]"});

    // Enter and Backspace are strokes without committed text.
    auto enter = ssg::decodeInput("\r", true, consumed);
    ASSERT_EQ(enter.stroke.code, ssg::KeyCode::Enter);
    ASSERT_TRUE(enter.text.empty());
    auto back = ssg::decodeInput("\x7f", true, consumed);
    ASSERT_EQ(back.stroke.code, ssg::KeyCode::Backspace);
}

TEST(decodeInputModifiedArrows) {
    std::size_t consumed = 0;
    // Shift+ArrowUp: ESC [ 1 ; 2 A (modifier 2 -> bitmask 1 = Shift).
    auto shiftUp = ssg::decodeInput("\x1b[1;2A", true, consumed);
    ASSERT_EQ(consumed, std::size_t{6});
    ASSERT_TRUE(shiftUp.status == ssg::DecodeStatus::key);
    ASSERT_EQ(shiftUp.stroke.code, ssg::KeyCode::ArrowUp);
    ASSERT_TRUE(shiftUp.stroke.shift);
    ASSERT_FALSE(shiftUp.stroke.mod);

    // Ctrl+ArrowRight: modifier 5 -> bitmask 4 = Ctrl, which is Mod.
    auto ctrlRight = ssg::decodeInput("\x1b[1;5C", true, consumed);
    ASSERT_EQ(ctrlRight.stroke.code, ssg::KeyCode::ArrowRight);
    ASSERT_TRUE(ctrlRight.stroke.mod);
    ASSERT_FALSE(ctrlRight.stroke.shift);

    // Alt+ArrowLeft: modifier 3 -> bitmask 2 = Alt, the same Mod.
    auto altLeft = ssg::decodeInput("\x1b[1;3D", true, consumed);
    ASSERT_EQ(altLeft.stroke.code, ssg::KeyCode::ArrowLeft);
    ASSERT_TRUE(altLeft.stroke.mod);

    // Ctrl+Shift+ArrowDown: modifier 6 -> bitmask 5 = Shift|Ctrl.
    auto csDown = ssg::decodeInput("\x1b[1;6B", true, consumed);
    ASSERT_EQ(csDown.stroke.code, ssg::KeyCode::ArrowDown);
    ASSERT_TRUE(csDown.stroke.shift);
    ASSERT_TRUE(csDown.stroke.mod);

    // Shift+Home / Shift+End.
    auto shiftHome = ssg::decodeInput("\x1b[1;2H", true, consumed);
    ASSERT_EQ(shiftHome.stroke.code, ssg::KeyCode::Home);
    ASSERT_TRUE(shiftHome.stroke.shift);
    auto shiftEnd = ssg::decodeInput("\x1b[1;2F", true, consumed);
    ASSERT_EQ(shiftEnd.stroke.code, ssg::KeyCode::End);
    ASSERT_TRUE(shiftEnd.stroke.shift);

    // Plain arrow still decodes unmodified.
    auto plain = ssg::decodeInput("\x1b[A", true, consumed);
    ASSERT_EQ(plain.stroke.code, ssg::KeyCode::ArrowUp);
    ASSERT_FALSE(plain.stroke.shift);

    // An unsupported modifier (m=9 -> bitmask 8, a Meta bit) falls back to the
    // plain, unmodified arrow, consuming the whole sequence.
    auto meta = ssg::decodeInput("\x1b[1;9A", true, consumed);
    ASSERT_EQ(consumed, std::size_t{6});
    ASSERT_EQ(meta.stroke.code, ssg::KeyCode::ArrowUp);
    ASSERT_FALSE(meta.stroke.shift);
    ASSERT_FALSE(meta.stroke.mod);

    // Ctrl+Alt+ArrowRight: modifier 7 -> bitmask 6 = Alt|Ctrl.  Ctrl+Alt belongs
    // to the windowing system, so SSG decodes the PLAIN arrow rather than a Mod
    // chord it would then swallow.
    auto ctrlAlt = ssg::decodeInput("\x1b[1;7C", true, consumed);
    ASSERT_EQ(ctrlAlt.stroke.code, ssg::KeyCode::ArrowRight);
    ASSERT_FALSE(ctrlAlt.stroke.mod);
    ASSERT_FALSE(ctrlAlt.stroke.shift);

    // Ctrl+Alt+Shift is likewise dropped whole: the Shift bit must not survive a
    // discarded chord, or Shift+ArrowRight would fire on a Ctrl+Alt+Shift press.
    auto ctrlAltShift = ssg::decodeInput("\x1b[1;8C", true, consumed);
    ASSERT_EQ(ctrlAltShift.stroke.code, ssg::KeyCode::ArrowRight);
    ASSERT_FALSE(ctrlAltShift.stroke.mod);
    ASSERT_FALSE(ctrlAltShift.stroke.shift);

    // A multi-digit modifier parses; m=16 -> bitmask 15 includes the unsupported
    // Meta bit, so it falls back to the plain arrow (consuming all 7 bytes).
    auto multi = ssg::decodeInput("\x1b[1;16C", true, consumed);
    ASSERT_EQ(consumed, std::size_t{7});
    ASSERT_EQ(multi.stroke.code, ssg::KeyCode::ArrowRight);
    ASSERT_FALSE(multi.stroke.mod);
    ASSERT_FALSE(multi.stroke.shift);
}

TEST(decodeInputMetaPrefixedCsiFoldsMod) {
    std::size_t consumed = 0;
    // A terminal transmitting Alt as a leading ESC sends Mod+Home as ESC ESC[H:
    // the inner CSI decodes to Home and the modifier is folded in.
    auto altHome = ssg::decodeInput("\x1b\x1b[H", true, consumed);
    ASSERT_EQ(consumed, std::size_t{4});
    ASSERT_TRUE(altHome.status == ssg::DecodeStatus::key);
    ASSERT_EQ(altHome.stroke.code, ssg::KeyCode::Home);
    ASSERT_TRUE(altHome.stroke.mod);
    ASSERT_FALSE(altHome.stroke.shift);

    auto altEnd = ssg::decodeInput("\x1b\x1b[F", true, consumed);
    ASSERT_EQ(altEnd.stroke.code, ssg::KeyCode::End);
    ASSERT_TRUE(altEnd.stroke.mod);

    // Mod+ArrowLeft via the same meta-prefix form.
    auto altLeft = ssg::decodeInput("\x1b\x1b[D", true, consumed);
    ASSERT_EQ(altLeft.stroke.code, ssg::KeyCode::ArrowLeft);
    ASSERT_TRUE(altLeft.stroke.mod);

    // The ESC prefix IS Alt, so an inner sequence that already carries Ctrl makes
    // the whole thing Ctrl+Alt -- dropped, leaving the bare key.  This path has
    // no modifier bitmask of its own, so the rule has to hold here separately.
    auto altCtrlHome = ssg::decodeInput("\x1b\x1b[1;5H", true, consumed);
    ASSERT_EQ(altCtrlHome.stroke.code, ssg::KeyCode::Home);
    ASSERT_FALSE(altCtrlHome.stroke.mod);

    // An incomplete inner CSI keeps the whole thing pending rather than
    // surfacing a spurious bare Escape.
    auto pending = ssg::decodeInput("\x1b\x1b[1;3", false, consumed);
    ASSERT_TRUE(pending.status == ssg::DecodeStatus::incomplete);

    // ESC ESC with no CSI introducer following is still a bare Escape.
    auto doubleEsc = ssg::decodeInput("\x1b\x1bx", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_EQ(doubleEsc.stroke.code, ssg::KeyCode::Escape);
    ASSERT_FALSE(doubleEsc.stroke.mod);
}

TEST(decodeInputTildeHomeEndPlainAndModified) {
    std::size_t consumed = 0;
    // Terminals that send Home/End in the tilde form rather than the letter form:
    // ESC[1~/ESC[7~ = Home, ESC[4~/ESC[8~ = End.
    auto home1 = ssg::decodeInput("\x1b[1~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{4});
    ASSERT_EQ(home1.stroke.code, ssg::KeyCode::Home);
    auto home7 = ssg::decodeInput("\x1b[7~", true, consumed);
    ASSERT_EQ(home7.stroke.code, ssg::KeyCode::Home);
    auto end4 = ssg::decodeInput("\x1b[4~", true, consumed);
    ASSERT_EQ(end4.stroke.code, ssg::KeyCode::End);
    auto end8 = ssg::decodeInput("\x1b[8~", true, consumed);
    ASSERT_EQ(end8.stroke.code, ssg::KeyCode::End);

    // Modified tilde forms fold in the modifier (Alt = m 3, Ctrl = m 5).
    auto altHome = ssg::decodeInput("\x1b[1;3~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{6});
    ASSERT_EQ(altHome.stroke.code, ssg::KeyCode::Home);
    ASSERT_TRUE(altHome.stroke.mod);
    auto altEnd = ssg::decodeInput("\x1b[4;3~", true, consumed);
    ASSERT_EQ(altEnd.stroke.code, ssg::KeyCode::End);
    ASSERT_TRUE(altEnd.stroke.mod);
    auto ctrlHome7 = ssg::decodeInput("\x1b[7;5~", true, consumed);
    ASSERT_EQ(ctrlHome7.stroke.code, ssg::KeyCode::Home);
    ASSERT_TRUE(ctrlHome7.stroke.mod);

    // Delete/Page tilde forms still work after adding the Home/End numbers.
    auto del = ssg::decodeInput("\x1b[3~", true, consumed);
    ASSERT_EQ(del.stroke.code, ssg::KeyCode::Delete);
    auto pageUp = ssg::decodeInput("\x1b[5~", true, consumed);
    ASSERT_EQ(pageUp.stroke.code, ssg::KeyCode::PageUp);

    // A split modified tilde form is incomplete until the '~' arrives.
    auto pending = ssg::decodeInput("\x1b[4;3", false, consumed);
    ASSERT_TRUE(pending.status == ssg::DecodeStatus::incomplete);
}

TEST(decodeInputStripsLockModifiersFromFunctionalKeys) {
    std::size_t consumed = 0;
    // A terminal speaking the Kitty protocol reports lock states in the modifier
    // field of the legacy letter/tilde functional-key forms. Captured from a real
    // terminal with NumLock on: Alt+Home = ESC[1;131H, Alt+End = ESC[1;131F
    // (modifier 131 -> bitmask 130 = NumLock(128) | Alt(2)). The lock bit must be
    // stripped so Alt survives and the binding resolves.
    auto altHome = ssg::decodeInput("\x1b[1;131H", true, consumed);
    ASSERT_EQ(consumed, std::size_t{8});
    ASSERT_EQ(altHome.stroke.code, ssg::KeyCode::Home);
    ASSERT_TRUE(altHome.stroke.mod);
    ASSERT_FALSE(altHome.stroke.shift);
    auto altEnd = ssg::decodeInput("\x1b[1;131F", true, consumed);
    ASSERT_EQ(altEnd.stroke.code, ssg::KeyCode::End);
    ASSERT_TRUE(altEnd.stroke.mod);

    // Plain Home with NumLock on (modifier 129 = NumLock only) stays unmodified.
    auto plainHome = ssg::decodeInput("\x1b[1;129H", true, consumed);
    ASSERT_EQ(plainHome.stroke.code, ssg::KeyCode::Home);
    ASSERT_FALSE(plainHome.stroke.mod);
    ASSERT_FALSE(plainHome.stroke.shift);

    // CapsLock (bit 6 = 64, modifier 65) is likewise stripped; Ctrl+Shift+End
    // with CapsLock (modifier 1 + 64 + 1 + 4 = 70) keeps only Ctrl+Shift.
    auto capsCtrlShiftEnd = ssg::decodeInput("\x1b[1;70F", true, consumed);
    ASSERT_EQ(capsCtrlShiftEnd.stroke.code, ssg::KeyCode::End);
    ASSERT_TRUE(capsCtrlShiftEnd.stroke.mod);
    ASSERT_TRUE(capsCtrlShiftEnd.stroke.shift);

    // The tilde form carries lock bits too (Alt+End as ESC[4;131~).
    auto altEndTilde = ssg::decodeInput("\x1b[4;131~", true, consumed);
    ASSERT_EQ(altEndTilde.stroke.code, ssg::KeyCode::End);
    ASSERT_TRUE(altEndTilde.stroke.mod);

    // A genuine unsupported modifier (Super, bit 3 = 8, modifier 9) still falls
    // back to the plain key -- stripping locks must not weaken that guard.
    auto superArrow = ssg::decodeInput("\x1b[1;9A", true, consumed);
    ASSERT_EQ(superArrow.stroke.code, ssg::KeyCode::ArrowUp);
    ASSERT_FALSE(superArrow.stroke.mod);
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

// Kitty reports Ctrl and Alt on separate bits, but SSG has one Mod.  The oracle
// encodes Mod as Alt (bit 1): the decoder must map it back to the same stroke.
// Encoding it as Ctrl would test the same collapse from the other side; that the
// two agree is what decodeInput's own tests assert.
int kittyBitmask(const ssg::KeyStroke& stroke) {
    return (stroke.shift ? 0b1 : 0) | (stroke.mod ? 0b10 : 0) |
           (stroke.meta ? 0b100000 : 0);
}

// The load-bearing parity oracle: for EVERY binding in the real default keymap,
// feeding the Kitty encoding of its KeyStroke must decode back to exactly that
// KeyStroke. Driven off the live keymap so a new binding cannot be added without
// coverage. Proves decodeKittyKey is complete for the keys SSG actually binds --
// including the non-letter modified keys flag 1 reroutes (Alt+Backspace,
// Alt+Slash, Alt+Digit8, Alt+Period/Comma) that a partial decoder would miss.
TEST(decodeKittyKeyHandCasesAndCapsLockImmunity) {
    std::size_t consumed = 0;
    // Alt+Shift+P: unicode key 'p' (112), mods = 1 + (shift|alt) = 4.
    auto altShiftP = ssg::decodeInput("\x1b[112;4u", true, consumed);
    ASSERT_TRUE(altShiftP.status == ssg::DecodeStatus::key);
    ASSERT_EQ(altShiftP.stroke.code, ssg::KeyCode::KeyP);
    ASSERT_TRUE(altShiftP.stroke.mod);
    ASSERT_TRUE(altShiftP.stroke.shift);
    ASSERT_TRUE(altShiftP.text.empty());  // a modified key commits no text

    // The SAME key with caps-lock ALSO held: mods = 1 + (shift|alt|caps) where
    // caps is bit 6 (64), so 1 + 3 + 64 = 68. The caps bit must NOT perturb the
    // stroke -- this is the whole fix, and why the binding no longer swaps under
    // caps lock.
    auto withCaps = ssg::decodeInput("\x1b[112;68u", true, consumed);
    ASSERT_TRUE(withCaps.stroke == altShiftP.stroke);

    // Ctrl+C: unicode 'c' (99), mods = 1 + ctrl(4) = 5.
    auto ctrlC = ssg::decodeInput("\x1b[99;5u", true, consumed);
    ASSERT_EQ(ctrlC.stroke.code, ssg::KeyCode::KeyC);
    ASSERT_TRUE(ctrlC.stroke.mod);
    ASSERT_FALSE(ctrlC.stroke.shift);

    // Plain Escape disambiguates to CSI 27 u under flag 1 -- the freed-Escape
    // (prompt cancel) must survive the reroute.
    auto esc = ssg::decodeInput("\x1b[27u", true, consumed);
    ASSERT_TRUE(esc.status == ssg::DecodeStatus::key);
    ASSERT_EQ(esc.stroke.code, ssg::KeyCode::Escape);
    ASSERT_FALSE(esc.stroke.mod);
}

TEST(everyEnterEncodingNormalizesToBareEnter) {
    std::size_t consumed = 0;
    auto isBareEnter = [](ssg::Decoded const& d) {
        return d.status == ssg::DecodeStatus::key &&
               d.stroke.code == ssg::KeyCode::Enter && !d.stroke.shift &&
               !d.stroke.mod && !d.stroke.mod && !d.stroke.meta;
    };

    // Plain CR / LF stay bare Enter (regression guard).
    ASSERT_TRUE(isBareEnter(ssg::decodeInput("\r", true, consumed)));
    ASSERT_TRUE(isBareEnter(ssg::decodeInput("\n", true, consumed)));

    // Kitty modified Enter (CSI 13 ; mods u): Shift, Ctrl, Alt, Ctrl+Alt, and
    // Meta (bit5) all fold onto a bare Enter so the newline binding fires.
    ASSERT_TRUE(isBareEnter(ssg::decodeInput("\x1b[13;2u", true, consumed)));
    ASSERT_TRUE(isBareEnter(ssg::decodeInput("\x1b[13;5u", true, consumed)));
    ASSERT_TRUE(isBareEnter(ssg::decodeInput("\x1b[13;3u", true, consumed)));
    ASSERT_TRUE(isBareEnter(ssg::decodeInput("\x1b[13;8u", true, consumed)));
    ASSERT_TRUE(isBareEnter(ssg::decodeInput("\x1b[13;33u", true, consumed)));

    // Kitty keypad Enter (KP_ENTER = 57414), plain and modified.
    ASSERT_TRUE(isBareEnter(ssg::decodeInput("\x1b[57414u", true, consumed)));
    ASSERT_TRUE(
        isBareEnter(ssg::decodeInput("\x1b[57414;2u", true, consumed)));

    // SS3 numpad Enter (application-keypad mode): ESC O M, consumed whole.
    auto ss3 = ssg::decodeInput("\x1bOM", true, consumed);
    ASSERT_TRUE(isBareEnter(ss3));
    ASSERT_EQ(consumed, std::size_t{3});

    // Alt-prefixed SS3 numpad Enter (ESC ESC O M) rides the recursive alt-chord
    // path; the outer normalization still strips the folded-in Alt.
    ASSERT_TRUE(isBareEnter(ssg::decodeInput("\x1b\x1bOM", true, consumed)));

    // ESC [ M (X10 mouse) must NOT be mistaken for Enter -- only the SS3
    // introducer means numpad Enter.
    auto mouse = ssg::decodeInput("\x1b[M\x20\x21\x21", true, consumed);
    ASSERT_TRUE(mouse.status != ssg::DecodeStatus::key ||
                mouse.stroke.code != ssg::KeyCode::Enter);
}

TEST(decodeKittyKeySelfIdentifyingAndMalformed) {
    std::size_t consumed = 0;
    // A private-prefixed `CSI ? ... u` is the KEYBOARD-PROTOCOL capability reply,
    // never a key -- it must classify as a reply so the probe still works.
    auto reply = ssg::decodeInput("\x1b[?1u", true, consumed);
    ASSERT_TRUE(reply.status == ssg::DecodeStatus::reply);

    // A non-private `CSI <n> u` is a key event (the self-identifying shape).
    auto key = ssg::decodeInput("\x1b[112u", true, consumed);
    ASSERT_TRUE(key.status == ssg::DecodeStatus::key);
    ASSERT_EQ(key.stroke.code, ssg::KeyCode::KeyP);

    // Field-1 sub-parameters (shifted-key : base-layout-key) are skipped; the
    // unshifted key code and the modifier field still decode.
    auto subparams = ssg::decodeInput("\x1b[112:80;4u", true, consumed);
    ASSERT_TRUE(subparams.status == ssg::DecodeStatus::key);
    ASSERT_EQ(subparams.stroke.code, ssg::KeyCode::KeyP);
    ASSERT_TRUE(subparams.stroke.mod);
    ASSERT_TRUE(subparams.stroke.shift);

    // An event-type sub-parameter on the modifier field is skipped.
    auto eventType = ssg::decodeInput("\x1b[99;5:1u", true, consumed);
    ASSERT_TRUE(eventType.status == ssg::DecodeStatus::key);
    ASSERT_TRUE(eventType.stroke.mod);

    // An empty modifier field decodes as no modifiers, not garbage.
    auto emptyMods = ssg::decodeInput("\x1b[112;u", true, consumed);
    ASSERT_TRUE(emptyMods.status == ssg::DecodeStatus::key);
    ASSERT_FALSE(emptyMods.stroke.mod);
    ASSERT_FALSE(emptyMods.stroke.shift);

    // A key SSG does not name (a Kitty functional PUA code) is consumed whole and
    // emits nothing, rather than leaking bytes into the document.
    auto unknown = ssg::decodeInput("\x1b[57400u", true, consumed);
    ASSERT_EQ(consumed, std::string{"\x1b[57400u"}.size());
    ASSERT_TRUE(unknown.status == ssg::DecodeStatus::none);

    // A truncated `CSI ... u` is incomplete (await more), never a partial key.
    auto partial = ssg::decodeInput("\x1b[112;4", false, consumed);
    ASSERT_TRUE(partial.status == ssg::DecodeStatus::incomplete);
}

TEST(decodeInputModifiedArrowSplitReadsAreIncomplete) {
    std::size_t consumed = 0;
    // Every partial-parameter prefix is incomplete and consumes nothing until the
    // final letter arrives.
    for (auto const* partial : {"\x1b[1", "\x1b[1;", "\x1b[1;2", "\x1b[1;16"}) {
        consumed = 99;
        auto decoded = ssg::decodeInput(partial, true, consumed);
        ASSERT_TRUE(decoded.status == ssg::DecodeStatus::incomplete);
        ASSERT_EQ(consumed, std::size_t{0});
    }
    // The completing bytes finish the sequence.
    auto done = ssg::decodeInput("\x1b[1;16D", true, consumed);
    ASSERT_EQ(done.stroke.code, ssg::KeyCode::ArrowLeft);
    ASSERT_EQ(consumed, std::size_t{7});

    // A pathologically long modifier parameter must not overflow the decimal
    // accumulator; it saturates, falls back to the plain arrow, and consumes the
    // whole sequence.
    std::string huge = "\x1b[1;";
    huge.append(40, '9');
    huge += "A";
    auto overflow = ssg::decodeInput(huge, true, consumed);
    ASSERT_EQ(consumed, huge.size());
    ASSERT_EQ(overflow.stroke.code, ssg::KeyCode::ArrowUp);
    ASSERT_FALSE(overflow.stroke.shift);
    ASSERT_FALSE(overflow.stroke.mod);
}

TEST(decodeInputDeleteKeyPlainAndModified) {
    std::size_t consumed = 0;
    // Plain Delete: ESC [ 3 ~. Regression test: this byte sequence used to
    // fall through to the unknown-CSI `default` branch (only consuming the
    // "ESC [ 3" introducer), leaving the trailing '~' to be decoded on the
    // NEXT call as plain printable text -- inserting a literal "~" instead
    // of deleting forward.
    auto del = ssg::decodeInput("\x1b[3~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{4});
    ASSERT_TRUE(del.status == ssg::DecodeStatus::key);
    ASSERT_EQ(del.stroke.code, ssg::KeyCode::Delete);
    ASSERT_TRUE(del.text.empty());

    // Modified form ESC [ 3 ; m ~ (m = 1 + bitmask). Shift+Delete: m=2.
    auto shiftDel = ssg::decodeInput("\x1b[3;2~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{6});
    ASSERT_EQ(shiftDel.stroke.code, ssg::KeyCode::Delete);
    ASSERT_TRUE(shiftDel.stroke.shift);

    // Split reads of the plain form are incomplete until the '~' arrives.
    for (auto const* partial : {"\x1b[3"}) {
        consumed = 99;
        auto decoded = ssg::decodeInput(partial, true, consumed);
        ASSERT_TRUE(decoded.status == ssg::DecodeStatus::incomplete);
        ASSERT_EQ(consumed, std::size_t{0});
    }
}

TEST(decodeInputPageKeysPlainAndModified) {
    std::size_t consumed = 0;
    // Plain PageUp / PageDown: ESC [ 5 ~ / ESC [ 6 ~.
    auto pageUp = ssg::decodeInput("\x1b[5~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{4});
    ASSERT_TRUE(pageUp.status == ssg::DecodeStatus::key);
    ASSERT_EQ(pageUp.stroke.code, ssg::KeyCode::PageUp);
    ASSERT_FALSE(pageUp.stroke.shift);
    auto pageDown = ssg::decodeInput("\x1b[6~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{4});
    ASSERT_EQ(pageDown.stroke.code, ssg::KeyCode::PageDown);

    // Modified form ESC [ 5 ; m ~ (m = 1 + bitmask). Shift+PageUp: m=2 -> Shift.
    auto shiftPgup = ssg::decodeInput("\x1b[5;2~", true, consumed);
    ASSERT_EQ(consumed, std::size_t{6});
    ASSERT_EQ(shiftPgup.stroke.code, ssg::KeyCode::PageUp);
    ASSERT_TRUE(shiftPgup.stroke.shift);
    ASSERT_FALSE(shiftPgup.stroke.mod);

    // Shift+PageDown.
    auto shiftPgdn = ssg::decodeInput("\x1b[6;2~", true, consumed);
    ASSERT_EQ(shiftPgdn.stroke.code, ssg::KeyCode::PageDown);
    ASSERT_TRUE(shiftPgdn.stroke.shift);

    // Ctrl+PageUp: m=5 -> bitmask 4 = Ctrl.
    auto ctrlPgup = ssg::decodeInput("\x1b[5;5~", true, consumed);
    ASSERT_EQ(ctrlPgup.stroke.code, ssg::KeyCode::PageUp);
    ASSERT_TRUE(ctrlPgup.stroke.mod);
    ASSERT_FALSE(ctrlPgup.stroke.shift);

    // Split reads of the modified form are incomplete until the '~' arrives.
    for (auto const* partial : {"\x1b[5", "\x1b[5;", "\x1b[5;2"}) {
        consumed = 99;
        auto decoded = ssg::decodeInput(partial, true, consumed);
        ASSERT_TRUE(decoded.status == ssg::DecodeStatus::incomplete);
        ASSERT_EQ(consumed, std::size_t{0});
    }
    // A '5'-prefixed sequence that is neither '~' nor ';' is skipped, not misread.
    auto junk = ssg::decodeInput("\x1b[5X", true, consumed);
    ASSERT_EQ(consumed, std::size_t{4});
    ASSERT_TRUE(junk.status == ssg::DecodeStatus::none);
}

TEST(decodeInputArrowsAndMouse) {
    std::size_t consumed = 0;
    auto up = ssg::decodeInput("\x1b[A", true, consumed);
    ASSERT_EQ(consumed, std::size_t{3});
    ASSERT_EQ(up.stroke.code, ssg::KeyCode::ArrowUp);
    auto down = ssg::decodeInput("\x1b[B", true, consumed);
    ASSERT_EQ(down.stroke.code, ssg::KeyCode::ArrowDown);

    auto wheel = ssg::decodeInput("\x1b[<65;10;5M", true, consumed);
    ASSERT_TRUE(wheel.status == ssg::DecodeStatus::scroll);
    ASSERT_EQ(wheel.scroll, std::int64_t{3});
    // The wheel carries its 0-based grid position (SGR 1-based 10,5 -> 9,4) so
    // the app can route it to the region under the pointer.
    ASSERT_EQ(wheel.pointer.column, 9);
    ASSERT_EQ(wheel.pointer.row, 4);
}

TEST(decodeInputEscapeBoundaryIsBounded) {
    std::size_t consumed = 0;
    // A buffered CSI introducer disambiguates to an arrow, not an Escape stroke.
    auto arrow = ssg::decodeInput("\x1b[A", false, consumed);
    ASSERT_EQ(arrow.stroke.code, ssg::KeyCode::ArrowUp);

    // ESC followed by a printable coalesces into one Alt stroke (legacy
    // meta-prefix), consuming both bytes.
    auto escThen = ssg::decodeInput("\x1bs", false, consumed);
    ASSERT_EQ(consumed, std::size_t{2});
    ASSERT_EQ(escThen.stroke.code, ssg::KeyCode::KeyS);
    ASSERT_TRUE(escThen.stroke.mod);

    // A lone ESC with more input possibly coming: incomplete, consume nothing.
    auto pending = ssg::decodeInput("\x1b", false, consumed);
    ASSERT_TRUE(pending.status == ssg::DecodeStatus::incomplete);
    ASSERT_EQ(consumed, std::size_t{0});

    // A lone ESC with input exhausted (bounded read returned nothing): the
    // Escape stroke.
    auto exhausted = ssg::decodeInput("\x1b", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_EQ(exhausted.stroke.code, ssg::KeyCode::Escape);

    // A truncated CSI is always incomplete regardless of exhaustion (the final
    // byte has not arrived).
    auto partial = ssg::decodeInput("\x1b[", true, consumed);
    ASSERT_TRUE(partial.status == ssg::DecodeStatus::incomplete);
}

// Legacy meta-prefix: Alt+<key> transmits as ESC then the key's byte, so the
// decoder coalesces ESC+printable into one Alt stroke.  Case supplies Shift for
// letters (there is no shift+lowercase on the wire); a shifted symbol carries no
// keycode and rides as text.  C0 controls become Ctrl+<letter>.
TEST(decodeInputCoalescesMetaPrefixIntoModStrokes) {
    std::size_t consumed = 0;

    // ESC s -> Alt+KeyS (lowercase: no shift), consuming both bytes.
    auto altS = ssg::decodeInput("\x1bs", false, consumed);
    ASSERT_EQ(consumed, std::size_t{2});
    ASSERT_TRUE(altS.status == ssg::DecodeStatus::key);
    ASSERT_EQ(altS.stroke.code, ssg::KeyCode::KeyS);
    ASSERT_TRUE(altS.stroke.mod);
    ASSERT_FALSE(altS.stroke.shift);

    // ESC P -> Alt+Shift+KeyP; ESC p -> Alt+KeyP.  Distinct strokes: case is the
    // only shift signal for a letter.
    auto altShiftP = ssg::decodeInput("\x1bP", false, consumed);
    ASSERT_EQ(altShiftP.stroke.code, ssg::KeyCode::KeyP);
    ASSERT_TRUE(altShiftP.stroke.mod);
    ASSERT_TRUE(altShiftP.stroke.shift);
    auto altP = ssg::decodeInput("\x1bp", false, consumed);
    ASSERT_EQ(altP.stroke.code, ssg::KeyCode::KeyP);
    ASSERT_TRUE(altP.stroke.mod);
    ASSERT_FALSE(altP.stroke.shift);

    // ESC * (Shift+8) has no keycode -- a shifted number-row symbol is
    // unbindable and rides as text, never {Digit8, shift}.
    auto altStar = ssg::decodeInput("\x1b*", false, consumed);
    ASSERT_EQ(consumed, std::size_t{2});
    ASSERT_TRUE(altStar.stroke.code == ssg::KeyCode::None);
    ASSERT_EQ(altStar.text, std::string{"*"});

    // C0 control byte -> Ctrl+<letter>.  0x13 = Ctrl+S.
    auto ctrlS = ssg::decodeInput("\x13", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_TRUE(ctrlS.status == ssg::DecodeStatus::key);
    ASSERT_EQ(ctrlS.stroke.code, ssg::KeyCode::KeyS);
    ASSERT_TRUE(ctrlS.stroke.mod);
    ASSERT_TRUE(ctrlS.text.empty());

    // Bytes handled as named keys above never fall into the C0 rule: Tab, Enter,
    // Backspace stay themselves.
    auto tab = ssg::decodeInput("\x09", true, consumed);
    ASSERT_EQ(tab.stroke.code, ssg::KeyCode::Tab);
    auto enter = ssg::decodeInput("\x0d", true, consumed);
    ASSERT_EQ(enter.stroke.code, ssg::KeyCode::Enter);
    auto lf = ssg::decodeInput("\x0a", true, consumed);
    ASSERT_EQ(lf.stroke.code, ssg::KeyCode::Enter);

    // Alt+Backspace (ESC 0x7f) is one Alt+Backspace stroke -- NOT keycode-less
    // text -- so the delete-word-backward binding resolves.  Alt+Enter/Tab the
    // same way.
    auto altBksp = ssg::decodeInput("\x1b\x7f", false, consumed);
    ASSERT_EQ(consumed, std::size_t{2});
    ASSERT_EQ(altBksp.stroke.code, ssg::KeyCode::Backspace);
    ASSERT_TRUE(altBksp.stroke.mod);
    ASSERT_TRUE(altBksp.text.empty());
    auto altBksp8 = ssg::decodeInput("\x1b\x08", false, consumed);
    ASSERT_EQ(altBksp8.stroke.code, ssg::KeyCode::Backspace);
    ASSERT_TRUE(altBksp8.stroke.mod);

    // A lone ESC still resolves to Escape once input is exhausted.
    auto esc = ssg::decodeInput("\x1b", true, consumed);
    ASSERT_EQ(esc.stroke.code, ssg::KeyCode::Escape);
    // ESC ESC is a bare Escape (consume one), not Alt+Escape.
    auto escEsc = ssg::decodeInput("\x1b\x1b", true, consumed);
    ASSERT_EQ(consumed, std::size_t{1});
    ASSERT_EQ(escEsc.stroke.code, ssg::KeyCode::Escape);
}

TEST(modQRequestsApplicationQuitBeforeEditorRouting) {
    ssg::KeyStroke quit;
    quit.code = ssg::KeyCode::KeyQ;
    quit.mod = true;
    ASSERT_TRUE(ssg::applicationQuitRequested(quit));

    quit.mod = false;
    ASSERT_FALSE(ssg::applicationQuitRequested(quit));
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
        auto const decoded = ssg::decodeInput(buffer, true, consumed);
        if (decoded.status == ssg::DecodeStatus::incomplete || consumed == 0) {
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
    auto const decoded = ssg::decodeInput(runaway, true, consumed);
    ASSERT_TRUE(decoded.status != ssg::DecodeStatus::incomplete);
    ASSERT_TRUE(consumed > 0);
    ASSERT_TRUE(consumed <= ssg::kMaxSequenceBytes);

    // A truncated SGR mouse prefix must not swallow arbitrary typed text while
    // hunting for its 'M'/'m'.
    std::string const mouseRunaway = std::string{"\x1b[<"} + std::string(4096, '9');
    consumed = 0;
    auto const mouse = ssg::decodeInput(mouseRunaway, true, consumed);
    ASSERT_TRUE(mouse.status != ssg::DecodeStatus::incomplete);
    ASSERT_TRUE(consumed > 0);
    ASSERT_TRUE(consumed <= ssg::kMaxSequenceBytes);

    // A short unterminated prefix is still held, so a sequence split across two
    // reads reassembles rather than being discarded.
    consumed = 99;
    auto const split = ssg::decodeInput("\x1b[1;2", true, consumed);
    ASSERT_TRUE(split.status == ssg::DecodeStatus::incomplete);
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
                ssg::decodeInput(whole.substr(0, prefix), false, consumed);
            ASSERT_TRUE(partial.status == ssg::DecodeStatus::incomplete);
            ASSERT_EQ(consumed, std::size_t{0});
            ASSERT_TRUE(partial.text.empty());
        }
        std::size_t consumed = 0;
        auto const complete = ssg::decodeInput(whole, false, consumed);
        ASSERT_TRUE(complete.status == ssg::DecodeStatus::reply);
        ASSERT_EQ(consumed, whole.size());
        ASSERT_EQ(complete.reply, std::string{whole});
        ASSERT_TRUE(complete.text.empty());
    }
}

// The DCS/OSC/APC/PM/SOS string introducers (ESC P/]/X/^/_) are byte-identical
// to the Alt+<key> chords the meta-prefix coalescing produces, so a stray such
// reply would be mistaken for a keystroke.  That is safe only while SSG asks no
// DCS/OSC question, so pin the real invariant: nothing under src/ emits a
// sequence that could solicit such a reply.  Adding one must fail here.
TEST(noDcsOrOscQueryMaySolicitAnUnparsedReply) {
    // The introducers now coalesce to Alt strokes (keyboard input); only the
    // no-query invariant below keeps a genuine reply from ever reaching here.
    std::size_t consumed = 0;
    auto const osc = ssg::decodeInput("\x1b]", true, consumed);
    ASSERT_TRUE(osc.status == ssg::DecodeStatus::key);
    ASSERT_EQ(osc.stroke.code, ssg::KeyCode::BracketRight);
    ASSERT_TRUE(osc.stroke.mod);
    // OSC 52 WRITE is emitted (see encodeClipboardWrite) and is deliberately
    // allowed: a write carries a payload and asks nothing, so no reply can come
    // back.  The scan below therefore looks for a QUERY -- an OSC or DCS ending
    // in "?" before its terminator, which is the form that solicits an answer.
    // Adding one must fail here until the decoder can parse what it will get
    // back, or the answer lands in the user's document.
    std::vector<std::string> emitters;
    for (auto const& entry : std::filesystem::directory_iterator{
             std::filesystem::path{SSG_TEST_SOURCE_DIR} / "src"}) {
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

TEST(aBracketedPasteIsContentAndNeverKeys) {
    std::size_t consumed = 0;
    // A paste containing the two things that would otherwise be interpreted: a
    // newline, and an escape sequence.
    std::string const payload = "line one\nline two\x1b[A";
    std::string const wrapped = "\x1b[200~" + payload + "\x1b[201~";
    auto const decoded = ssg::decodeInput(wrapped, true, consumed);
    ASSERT_TRUE(decoded.status == ssg::DecodeStatus::paste);
    ASSERT_EQ(decoded.text, payload);
    ASSERT_EQ(consumed, wrapped.size());
    // It is not a keypress, so nothing can route it to the keymap.
    ASSERT_TRUE(decoded.stroke.code == ssg::KeyCode::None);

    // An empty paste is still a paste, consumed whole.
    consumed = 0;
    auto const empty = ssg::decodeInput("\x1b[200~\x1b[201~", true, consumed);
    ASSERT_TRUE(empty.status == ssg::DecodeStatus::paste);
    ASSERT_TRUE(empty.text.empty());
    ASSERT_EQ(consumed, std::size_t{12});

    // Every proper prefix waits rather than being half-consumed, so a paste
    // split across reads reassembles instead of leaking its head as text.
    for (std::size_t prefix = 2; prefix < wrapped.size(); ++prefix) {
        consumed = 99;
        auto const partial =
            ssg::decodeInput(std::string_view{wrapped}.substr(0, prefix),
                                   false, consumed);
        ASSERT_TRUE(partial.status == ssg::DecodeStatus::incomplete);
        ASSERT_EQ(consumed, std::size_t{0});
    }

    // The mode is in kAllModes, so terminal teardown and the crash undo both
    // turn bracketed paste back off; a terminal left in it would wrap the
    // shell's pastes after ssg exits.
    bool listed = false;
    for (auto const& mode : ssg::kAllModes) {
        if (mode.enter == ssg::kBracketedPaste.enter) listed = true;
    }
    ASSERT_TRUE(listed);
}

TEST(decodeInputPointerPressReleaseDrag) {
    std::size_t consumed = 0;

    // Left press at SGR (1,1) -> grid (0,0). Button bits 0, final 'M'.
    auto press = ssg::decodeInput("\x1b[<0;1;1M", true, consumed);
    ASSERT_TRUE(press.status == ssg::DecodeStatus::pointer);
    ASSERT_EQ(consumed, std::size_t{9});
    ASSERT_EQ(press.pointer.column, 0);
    ASSERT_EQ(press.pointer.row, 0);
    ASSERT_TRUE(press.pointer.button == ssg::PointerButton::left);
    ASSERT_TRUE(press.pointer.kind == ssg::PointerKind::press);

    // Left release (final 'm') at (10,5) -> grid (9,4).
    auto release = ssg::decodeInput("\x1b[<0;10;5m", true, consumed);
    ASSERT_TRUE(release.status == ssg::DecodeStatus::pointer);
    ASSERT_EQ(release.pointer.column, 9);
    ASSERT_EQ(release.pointer.row, 4);
    ASSERT_TRUE(release.pointer.button == ssg::PointerButton::left);
    ASSERT_TRUE(release.pointer.kind == ssg::PointerKind::release);

    // Left drag: motion bit 32 set (Cb 32), final 'M', at (3,7) -> grid (2,6).
    auto drag = ssg::decodeInput("\x1b[<32;3;7M", true, consumed);
    ASSERT_TRUE(drag.status == ssg::DecodeStatus::pointer);
    ASSERT_EQ(drag.pointer.column, 2);
    ASSERT_EQ(drag.pointer.row, 6);
    ASSERT_TRUE(drag.pointer.button == ssg::PointerButton::left);
    ASSERT_TRUE(drag.pointer.kind == ssg::PointerKind::drag);

    // Middle press (button bits 1) and right press (button bits 2).
    auto middle = ssg::decodeInput("\x1b[<1;2;2M", true, consumed);
    ASSERT_TRUE(middle.pointer.button == ssg::PointerButton::middle);
    ASSERT_TRUE(middle.pointer.kind == ssg::PointerKind::press);
    auto right = ssg::decodeInput("\x1b[<2;2;2M", true, consumed);
    ASSERT_TRUE(right.pointer.button == ssg::PointerButton::right);

    // The wheel stays a scroll event, not a pointer event.
    auto wheelUp = ssg::decodeInput("\x1b[<64;10;5M", true, consumed);
    ASSERT_TRUE(wheelUp.status == ssg::DecodeStatus::scroll);
    ASSERT_EQ(wheelUp.scroll, std::int64_t{-3});
    ASSERT_EQ(wheelUp.pointer.column, 9);
    ASSERT_EQ(wheelUp.pointer.row, 4);
}

TEST(decodeInputPointerCarriesAltModifier) {
    std::size_t consumed = 0;

    // Alt-left press: Cb = left(0) | Alt(8) = 8, final 'M', at SGR (3,4) ->
    // grid (2,3). Bit3 surfaces as PointerEvent::alt.
    auto altPress = ssg::decodeInput("\x1b[<8;3;4M", true, consumed);
    ASSERT_TRUE(altPress.status == ssg::DecodeStatus::pointer);
    ASSERT_EQ(altPress.pointer.column, 2);
    ASSERT_EQ(altPress.pointer.row, 3);
    ASSERT_TRUE(altPress.pointer.button == ssg::PointerButton::left);
    ASSERT_TRUE(altPress.pointer.kind == ssg::PointerKind::press);
    ASSERT_TRUE(altPress.pointer.alt);

    // Same press without bit3: alt is false.
    auto plainPress = ssg::decodeInput("\x1b[<0;3;4M", true, consumed);
    ASSERT_TRUE(plainPress.status == ssg::DecodeStatus::pointer);
    ASSERT_FALSE(plainPress.pointer.alt);

    // Alt-drag: Cb = motion(32) | Alt(8) = 40, final 'M'. kind is drag, alt set.
    auto altDrag = ssg::decodeInput("\x1b[<40;3;4M", true, consumed);
    ASSERT_TRUE(altDrag.status == ssg::DecodeStatus::pointer);
    ASSERT_TRUE(altDrag.pointer.kind == ssg::PointerKind::drag);
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
        auto decoded = ssg::decodeInput(partial, true, consumed);
        ASSERT_TRUE(decoded.status == ssg::DecodeStatus::incomplete);
        ASSERT_EQ(consumed, std::size_t{0});
    }
    // The complete sequence then decodes.
    auto complete = ssg::decodeInput("\x1b[<0;10;5M", true, consumed);
    ASSERT_TRUE(complete.status == ssg::DecodeStatus::pointer);
}

const ssg::DocumentPointerInput* documentInputOf(
    ssg::PointerDispatch const& plan) {
    return plan.semantic_input
               ? std::get_if<ssg::DocumentPointerInput>(
                     &*plan.semantic_input)
               : nullptr;
}

TEST(routePointerLeftPressOnEditorEmitsDocumentInput) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    hit.byteOffset = 3;
    ssg::PointerTargets targets;
    targets.document_position =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};

    auto plan = ssg::route_pointer(hit, ssg::PointerButton::left,
                                        ssg::PointerKind::press, false, false,
                                        std::nullopt, targets);
    auto const* input = documentInputOf(plan);
    ASSERT_TRUE(input != nullptr);
    if (input) {
        ASSERT_EQ(input->position, std::optional<ssg::ByteOffset>{
                                       targets.document_position->byteOffset});
        ASSERT_EQ(input->phase, ssg::InputPointerPhase::Press);
        ASSERT_FALSE(input->additive);
    }
    // The press begins a potential selection drag.
    ASSERT_TRUE(plan.begins_drag);
    ASSERT_FALSE(plan.ends_drag);
}

TEST(routePointerIgnoresNonEditorAndNonLeft) {
    ssg::PointerTargets const empty;

    // A press on nothing (out of bounds / chrome) dispatches no command.
    ssg::RegionHit noneHit;  // region defaults to HitRegion::none
    auto nonePlan = ssg::route_pointer(
        noneHit, ssg::PointerButton::left, ssg::PointerKind::press, false,
        false, std::nullopt, empty);
    ASSERT_FALSE(nonePlan.semantic_input.has_value());
    ASSERT_FALSE(nonePlan.begins_drag);

    // A left press on a non-editor region (e.g. the panel) is not handled by
    // M8-C: no editor command, no drag.
    ssg::RegionHit panelHit;
    panelHit.region = ssg::HitRegion::Panel;
    auto panelPlan = ssg::route_pointer(
        panelHit, ssg::PointerButton::left, ssg::PointerKind::press, false,
        false, std::nullopt, empty);
    ASSERT_FALSE(panelPlan.semantic_input.has_value());

    // A right/middle press on the editor is a no-op in M8.
    ssg::RegionHit editorHit;
    editorHit.region = ssg::HitRegion::Editor;
    ssg::PointerTargets targets;
    targets.document_position =
        ssg::DocumentPosition{ssg::ByteOffset{0}, ssg::LineIndex{0}, ssg::CellIndex{0}};
    auto rightPlan = ssg::route_pointer(
        editorHit, ssg::PointerButton::right, ssg::PointerKind::press, false,
        false, std::nullopt, targets);
    ASSERT_FALSE(rightPlan.semantic_input.has_value());

    // A left press on the editor with no resolved position (e.g. a blank cell)
    // dispatches nothing.
    auto unresolved = ssg::route_pointer(
        editorHit, ssg::PointerButton::left, ssg::PointerKind::press, false,
        false, std::nullopt, empty);
    ASSERT_FALSE(unresolved.semantic_input.has_value());
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
        auto decoded = ssg::decodeInput(malformed, true, consumed);
        ASSERT_TRUE(decoded.status == ssg::DecodeStatus::none);
        ASSERT_EQ(consumed, malformed.size());  // consumed so the loop advances
    }

    // A non-digit in a parameter position is a CSI *final* byte (0x40-0x7E), so
    // the sequence ends there under the grammar and the trailing 'M' is an
    // ordinary printable that follows it.  The extent of a sequence is decided by
    // the grammar rather than by hunting for the byte we hoped to find, which is
    // what bounds the scan (INV-decode-terminates).
    std::string_view const earlyFinal = "\x1b[<0;1;xM";
    consumed = 0;
    auto const decoded = ssg::decodeInput(earlyFinal, true, consumed);
    ASSERT_TRUE(decoded.status == ssg::DecodeStatus::none);
    ASSERT_EQ(consumed, earlyFinal.size() - 1);
}

TEST(routePointerDragEmitsDocumentMove) {
    auto const anchor =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    hit.byteOffset = 10;
    ssg::PointerTargets targets;
    auto const active =
        ssg::DocumentPosition{ssg::ByteOffset{10}, ssg::LineIndex{1}, ssg::CellIndex{2}};
    targets.document_position = active;

    auto plan = ssg::route_pointer(hit, ssg::PointerButton::left,
                                        ssg::PointerKind::drag, false, true,
                                        anchor, targets);
    auto const* input = documentInputOf(plan);
    ASSERT_TRUE(input != nullptr);
    if (input) {
        ASSERT_EQ(input->position,
                  std::optional<ssg::ByteOffset>{active.byteOffset});
        ASSERT_EQ(input->phase, ssg::InputPointerPhase::Move);
    }
    ASSERT_FALSE(plan.begins_drag);
    ASSERT_FALSE(plan.ends_drag);
}

TEST(routePointerFieldHitEmitsUiActivationCommand) {
    ssg::PointerTargets targets;
    targets.ui_node_id = ssg::UiNodeId{"header.files"};
    for (auto region :
         {ssg::HitRegion::HeaderField, ssg::HitRegion::FooterField}) {
        ssg::RegionHit hit;
        hit.region = region;
        auto plan = ssg::route_pointer(
            hit, ssg::PointerButton::left, ssg::PointerKind::press, false,
            false, std::nullopt, targets);
        ASSERT_FALSE(plan.command.has_value());
        ASSERT_TRUE(plan.semantic_input.has_value());
        if (plan.semantic_input) {
            const auto* input =
                std::get_if<ssg::UiNodePointerInput>(&*plan.semantic_input);
            ASSERT_TRUE(input != nullptr);
            if (input) {
                ASSERT_EQ(input->nodeId, ssg::UiNodeId{"header.files"});
            }
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
    ssg::PointerTargets targets;
    targets.document_position =
        ssg::DocumentPosition{ssg::ByteOffset{5}, ssg::LineIndex{0}, ssg::CellIndex{5}};

    // Not dragging (no prior press) -> no command even over the editor.
    auto notDragging = ssg::route_pointer(
        editorHit, ssg::PointerButton::left, ssg::PointerKind::drag, false,
        false, anchor, targets);
    ASSERT_FALSE(notDragging.semantic_input.has_value());

    // Dragging but the pointer is over a cell with no document target (past a
    // short line's end / beyond the viewport edge) -> no command, selection holds.
    ssg::PointerTargets const noTarget;
    auto offContent = ssg::route_pointer(
        editorHit, ssg::PointerButton::left, ssg::PointerKind::drag, false,
        true, anchor, noTarget);
    ASSERT_FALSE(offContent.semantic_input.has_value());

    // Dragging over a non-editor region (e.g. the panel) -> no command.
    ssg::RegionHit panelHit;
    panelHit.region = ssg::HitRegion::Panel;
    auto offEditor = ssg::route_pointer(
        panelHit, ssg::PointerButton::left, ssg::PointerKind::drag, false,
        true, anchor, targets);
    ASSERT_FALSE(offEditor.semantic_input.has_value());
}

TEST(routePointerReleaseEndsAuthoritativeDocumentGesture) {
    ssg::RegionHit editorHit;
    editorHit.region = ssg::HitRegion::Editor;
    ssg::PointerTargets targets;
    targets.document_position =
        ssg::DocumentPosition{ssg::ByteOffset{5}, ssg::LineIndex{0}, ssg::CellIndex{5}};

    // Release while dragging ends the drag and dispatches nothing.
    auto ending = ssg::route_pointer(
        editorHit, ssg::PointerButton::left, ssg::PointerKind::release, false,
        true, targets.document_position, targets);
    auto const* input = documentInputOf(ending);
    ASSERT_TRUE(input != nullptr);
    if (input) {
        ASSERT_FALSE(input->position.has_value());
        ASSERT_EQ(input->phase, ssg::InputPointerPhase::Release);
    }
    ASSERT_TRUE(ending.ends_drag);

    // A release when not dragging is inert.
    auto stray = ssg::route_pointer(
        editorHit, ssg::PointerButton::left, ssg::PointerKind::release, false,
        false, std::nullopt, targets);
    ASSERT_FALSE(stray.semantic_input.has_value());
    ASSERT_FALSE(stray.ends_drag);
}

TEST(routePointerAltPressMarksDocumentInputAdditive) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    hit.byteOffset = 3;
    ssg::PointerTargets targets;
    auto const pos =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    targets.document_position = pos;

    auto plan = ssg::route_pointer(hit, ssg::PointerButton::left,
                                        ssg::PointerKind::press, true, false,
                                        std::nullopt, targets);
    auto const* input = documentInputOf(plan);
    ASSERT_TRUE(input != nullptr);
    if (input) ASSERT_TRUE(input->additive);
    ASSERT_TRUE(plan.begins_drag);

    // The same press without Alt stays on the ordinary caret-placement path.
    auto plain = ssg::route_pointer(hit, ssg::PointerButton::left,
                                         ssg::PointerKind::press, false, false,
                                         std::nullopt, targets);
    input = documentInputOf(plain);
    ASSERT_TRUE(input != nullptr);
    if (input) ASSERT_FALSE(input->additive);
}

namespace {
ssg::DocumentPosition posAt(std::uint64_t byte, std::uint32_t line,
                            std::uint32_t cell) {
    return ssg::DocumentPosition{ssg::ByteOffset{byte}, ssg::LineIndex{line},
                                 ssg::CellIndex{cell}};
}
}  // namespace

TEST(altDoubleClickStillSelectsAWordNeverRemoves) {
    auto const doubled = ssg::double_click_dispatch(posAt(5, 0, 5));
    auto const* input = documentInputOf(doubled);
    ASSERT_TRUE(input != nullptr);
    if (input) ASSERT_TRUE(input->selectWord);
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

    auto created = ssg::createEditor(
        {root / "workspace", root / "scratch", root / "recovery"});
    ASSERT_TRUE(created.accepted());
    if (!created.accepted()) return;
    auto& runtime = *created.session;
    ASSERT_TRUE(ssg::test::openFile(runtime, std::string{"f.txt"})
                    .accepted());

    auto const doc = ssg::test::activeDocumentText(runtime);
    auto const p2 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{2});
    auto const p7 = ssg::resolveSelectionPosition(doc, ssg::ByteOffset{7});
    ASSERT_TRUE(p2.has_value() && p7.has_value());
    if (!p2 || !p7) return;

    // Two carets: one at 2, one at 7.
    ASSERT_TRUE(ssg::test::setSelections(
                    runtime,
                    {{p2->byteOffset.value(), p2->byteOffset.value()},
                     {p7->byteOffset.value(), p7->byteOffset.value()}})
                    .accepted());

    auto result = runtime.input(ssg::DocumentPointerInput{
            p2->byteOffset, true});
    ASSERT_TRUE(result.command.has_value() && result.command->accepted());

    auto afterSnap = ssg::test::projectGridFrame(runtime);
    ASSERT_TRUE(afterSnap.has_value());
    if (!afterSnap) return;
    auto const& survivors =
        afterSnap->selections.items();
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
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    ssg::PointerTargets targets;
    targets.document_position = active;

    auto plan = ssg::route_pointer(hit, ssg::PointerButton::left,
                                        ssg::PointerKind::drag, true, true,
                                        anchor, targets);
    auto const* input = documentInputOf(plan);
    ASSERT_TRUE(input != nullptr);
    if (input) {
        ASSERT_FALSE(input->additive);
        ASSERT_EQ(input->phase, ssg::InputPointerPhase::Move);
        ASSERT_EQ(input->position,
                  std::optional<ssg::ByteOffset>{active.byteOffset});
    }

    // A no-Alt drag stays on the ordinary single-selection path.
    auto plain = ssg::route_pointer(hit, ssg::PointerButton::left,
                                         ssg::PointerKind::drag, false, true,
                                         anchor, targets);
    input = documentInputOf(plain);
    ASSERT_TRUE(input != nullptr);
    if (input) ASSERT_FALSE(input->additive);
}

TEST(routePointerAltDragIgnoresPerMotionModifierBit) {
    // The modifier is authoritative only on Press. Move and Release do not
    // restate it; the library retains the established gesture mode.
    auto const anchor =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    auto const active =
        ssg::DocumentPosition{ssg::ByteOffset{6}, ssg::LineIndex{0}, ssg::CellIndex{6}};
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    ssg::PointerTargets targets;
    targets.document_position = active;

    auto plan = ssg::route_pointer(hit, ssg::PointerButton::left,
                                        ssg::PointerKind::drag, true, true,
                                        anchor, targets);
    auto const* input = documentInputOf(plan);
    ASSERT_TRUE(input != nullptr);
    if (input) ASSERT_FALSE(input->additive);
    // Alt release ends the drag and dispatches nothing.
    auto release = ssg::route_pointer(
        hit, ssg::PointerButton::left, ssg::PointerKind::release, true,
        true, anchor, targets);
    input = documentInputOf(release);
    ASSERT_TRUE(input != nullptr);
    if (input) ASSERT_EQ(input->phase, ssg::InputPointerPhase::Release);
    ASSERT_TRUE(release.ends_drag);
}

TEST(routePointerEditorScrollbarScrollsToFraction) {
    // A press or drag on the editor gutter scrolls to the fraction hit_test
    // reported, independent of the selection drag state. The bottom of the
    // gutter reports numerator == denominator (-> maximum_first_row); the top
    // reports numerator 0 (-> first_row 0).
    auto scrollArgs = [](ssg::PointerDispatch const& plan)
        -> ssg::ScrollFractionInput const* {
        if (!plan.semantic_input) return nullptr;
        auto const* input =
            std::get_if<ssg::ScrollFractionInput>(&*plan.semantic_input);
        return input &&
                       input->action.target == ssg::ScrollTarget::Document
                   ? input
                   : nullptr;
    };

    ssg::RegionHit bottom;
    bottom.region = ssg::HitRegion::EditorScrollbar;
    bottom.scrollNumerator = 7;
    bottom.scrollDenominator = 7;
    ssg::PointerTargets const empty;

    for (auto kind : {ssg::PointerKind::press, ssg::PointerKind::drag}) {
        auto plan = ssg::route_pointer(
            bottom, ssg::PointerButton::left, kind, false, false, std::nullopt, empty);
        auto const* args = scrollArgs(plan);
        ASSERT_TRUE(args != nullptr);
        if (args) {
            ASSERT_EQ(args->action.numerator, std::uint32_t{7});
            ASSERT_EQ(args->action.denominator, std::uint32_t{7});
        }
        ASSERT_FALSE(plan.begins_drag);
        ASSERT_FALSE(plan.ends_drag);
    }

    ssg::RegionHit top;
    top.region = ssg::HitRegion::EditorScrollbar;
    top.scrollNumerator = 0;
    top.scrollDenominator = 7;
    auto topPlan = ssg::route_pointer(
        top, ssg::PointerButton::left, ssg::PointerKind::press, false, false,
        std::nullopt, empty);
    auto const* topArgs = scrollArgs(topPlan);
    ASSERT_TRUE(topArgs != nullptr);
    if (topArgs) {
        ASSERT_EQ(topArgs->action.numerator, std::uint32_t{0});
        ASSERT_EQ(topArgs->action.denominator, std::uint32_t{7});
    }

    // A mid-drag onto the editor gutter scrolls even while a selection drag is
    // active; the scrollbar path does not consult the drag anchor.
    auto const anchor =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0}, ssg::CellIndex{3}};
    auto mid = ssg::route_pointer(bottom, ssg::PointerButton::left,
                                       ssg::PointerKind::drag, false, true, anchor,
                                       empty);
    ASSERT_TRUE(scrollArgs(mid) != nullptr);
}

TEST(routePointerPanelAndPaletteScrollbars) {
    ssg::PointerTargets const empty;
    for (auto kind : {ssg::PointerKind::press, ssg::PointerKind::drag}) {
        ssg::RegionHit panel;
        panel.region = ssg::HitRegion::PanelScrollbar;
        panel.scrollNumerator = 3;
        panel.scrollDenominator = 4;
        auto panelPlan = ssg::route_pointer(
            panel, ssg::PointerButton::left, kind, false, false, std::nullopt,
            empty);
        auto const* panelInput =
            panelPlan.semantic_input
                ? std::get_if<ssg::ScrollFractionInput>(
                      &*panelPlan.semantic_input)
                : nullptr;
        ASSERT_TRUE(panelInput != nullptr);
        if (panelInput) {
            ASSERT_EQ(panelInput->action.target, ssg::ScrollTarget::Tree);
            ASSERT_EQ(panelInput->action.numerator, std::uint32_t{3});
            ASSERT_EQ(panelInput->action.denominator, std::uint32_t{4});
        }
        ASSERT_FALSE(panelPlan.client_scroll.has_value());

        ssg::RegionHit palette;
        palette.region = ssg::HitRegion::PaletteScrollbar;
        palette.scrollNumerator = 1;
        palette.scrollDenominator = 2;
        auto palettePlan = ssg::route_pointer(
            palette, ssg::PointerButton::left, kind, false, false, std::nullopt,
            empty);
        ASSERT_FALSE(palettePlan.semantic_input.has_value());
        ASSERT_TRUE(palettePlan.client_scroll.has_value());
        if (palettePlan.client_scroll) {
            ASSERT_EQ(palettePlan.client_scroll->target,
                      ssg::WheelTarget::palette);
            ASSERT_EQ(palettePlan.client_scroll->numerator, std::uint32_t{1});
            ASSERT_EQ(palettePlan.client_scroll->denominator, std::uint32_t{2});
        }
    }
}

TEST(routePointerTabPressActivatesTheTab) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Tab;
    hit.tabIndex = 2;
    ssg::PointerTargets targets;
    targets.tab_id = ssg::TabId{7};

    auto plan = ssg::route_pointer(hit, ssg::PointerButton::left,
                                        ssg::PointerKind::press, false, false,
                                        std::nullopt, targets);
    ASSERT_TRUE(plan.semantic_input.has_value());
    auto const* input =
        std::get_if<ssg::TabPointerInput>(&*plan.semantic_input);
    ASSERT_TRUE(input != nullptr);
    if (input) {
        ASSERT_EQ(input->tabId, ssg::TabId{7});
        ASSERT_EQ(input->button, ssg::InputPointerButton::Primary);
    }
    ASSERT_FALSE(plan.begins_drag);
    ASSERT_FALSE(plan.ends_drag);

    // A tab hit the caller could not resolve to a TabId dispatches nothing.
    ssg::PointerTargets const empty;
    auto unresolved = ssg::route_pointer(
        hit, ssg::PointerButton::left, ssg::PointerKind::press, false, false,
        std::nullopt, empty);
    ASSERT_FALSE(unresolved.semantic_input.has_value());
}

TEST(routePointerMiddleClickOnATabClosesIt) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Tab;
    hit.tabIndex = 2;
    ssg::PointerTargets targets;
    targets.tab_id = ssg::TabId{7};

    auto plan = ssg::route_pointer(hit, ssg::PointerButton::middle,
                                        ssg::PointerKind::press, false, false,
                                        std::nullopt, targets);
    ASSERT_TRUE(plan.semantic_input.has_value());
    auto const* input =
        std::get_if<ssg::TabPointerInput>(&*plan.semantic_input);
    ASSERT_TRUE(input != nullptr);
    if (input) {
        ASSERT_EQ(input->tabId, ssg::TabId{7});
        ASSERT_EQ(input->button, ssg::InputPointerButton::Auxiliary);
    }

    // Middle-click off a tab, or a release rather than a press, does nothing.
    ssg::RegionHit editorHit;
    editorHit.region = ssg::HitRegion::Editor;
    ASSERT_FALSE(ssg::route_pointer(editorHit, ssg::PointerButton::middle,
                                        ssg::PointerKind::press, false, false,
                                        std::nullopt, targets)
                    .semantic_input.has_value());
    ASSERT_FALSE(ssg::route_pointer(hit, ssg::PointerButton::middle,
                                        ssg::PointerKind::release, false, false,
                                        std::nullopt, targets)
                    .semantic_input.has_value());
    // An unresolved tab id dispatches nothing.
    ssg::PointerTargets const noTab;
    ASSERT_FALSE(ssg::route_pointer(hit, ssg::PointerButton::middle,
                                        ssg::PointerKind::press, false, false,
                                        std::nullopt, noTab)
                    .semantic_input.has_value());
}

TEST(doubleClickDetectorPairsPressesByTimeAndCell) {
    using namespace std::chrono_literals;
    auto const window = 400ms;
    auto t0 = std::chrono::steady_clock::time_point{};
    std::uint64_t const surface = 1;  // one document/tab

    ssg::ClickTracker tracker;
    ASSERT_FALSE(ssg::register_click_is_double(tracker, t0, surface, 5, 10,
                                                    window));
    ASSERT_TRUE(ssg::register_click_is_double(tracker, t0 + 100ms, surface,
                                                   5, 10, window));
    // After firing, the tracker resets: a third rapid press is a fresh single
    // (no triple-click).
    ASSERT_FALSE(ssg::register_click_is_double(tracker, t0 + 150ms, surface,
                                                    5, 10, window));

    // Same cell but OUTSIDE the window -> two singles.
    ssg::ClickTracker slow;
    ASSERT_FALSE(
        ssg::register_click_is_double(slow, t0, surface, 2, 2, window));
    ASSERT_FALSE(ssg::register_click_is_double(slow, t0 + 500ms, surface, 2,
                                                    2, window));

    // Within the window but on a DIFFERENT cell -> two singles, then a same-cell
    // press within the window of THAT press pairs.
    ssg::ClickTracker moved;
    ASSERT_FALSE(
        ssg::register_click_is_double(moved, t0, surface, 1, 1, window));
    ASSERT_FALSE(ssg::register_click_is_double(moved, t0 + 50ms, surface, 1,
                                                    2, window));
    ASSERT_TRUE(ssg::register_click_is_double(moved, t0 + 80ms, surface, 1,
                                                   2, window));

    // Same cell within the window but a DIFFERENT surface (a fast click after a
    // tab switch) -> not a double-click.
    ssg::ClickTracker switched;
    ASSERT_FALSE(
        ssg::register_click_is_double(switched, t0, 1, 3, 3, window));
    ASSERT_FALSE(ssg::register_click_is_double(switched, t0 + 50ms, 2, 3, 3,
                                                    window));
}

// The app loop's decision is a thin gate over the classifier: a single left
// editor press stays a caret-placement input (with a drag armed), while a
// recognized double-click routes through double_click_dispatch, which selects
// the word at the position and arms NO drag. Pinned here so a mis-wire at that
// seam is caught.
TEST(aDoubleClickOnTheEditorSelectsTheWordNotJustTheCaret) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Editor;
    hit.byteOffset = 3;
    ssg::PointerTargets targets;
    auto const position =
        ssg::DocumentPosition{ssg::ByteOffset{3}, ssg::LineIndex{0},
                              ssg::CellIndex{3}};
    targets.document_position = position;

    // Single press: caret placement, drag armed.
    auto single = ssg::route_pointer(hit, ssg::PointerButton::left,
                                          ssg::PointerKind::press, false, false,
                                          std::nullopt, targets);
    auto const* input = documentInputOf(single);
    ASSERT_TRUE(input != nullptr);
    if (input) ASSERT_FALSE(input->selectWord);
    ASSERT_TRUE(single.begins_drag);

    // Double-click: word selection, NO drag, and the clicked position passed
    // through as the command argument.
    auto doubled = ssg::double_click_dispatch(position);
    input = documentInputOf(doubled);
    ASSERT_TRUE(input != nullptr);
    ASSERT_FALSE(doubled.begins_drag);
    if (input) {
        ASSERT_TRUE(input->selectWord);
        ASSERT_EQ(input->position,
                  std::optional<ssg::ByteOffset>{position.byteOffset});
    }
}

TEST(scrollbarGrabOffsetHoldsTheThumbUnderTheCursor) {
    // Press ON the thumb: the offset is where within the thumb it was grabbed,
    // so the same point of the thumb stays under the cursor as it drags.
    int const thumbStart = 4;
    int const thumbSize = 3;  // rows 4,5,6
    ASSERT_EQ(ssg::scrollbar_grab_offset(4, thumbStart, thumbSize), 0);
    ASSERT_EQ(ssg::scrollbar_grab_offset(5, thumbStart, thumbSize), 1);
    ASSERT_EQ(ssg::scrollbar_grab_offset(6, thumbStart, thumbSize), 2);
    // Press in the well (above or below the thumb): centre the thumb on the
    // cursor, so it jumps to the click and can then be dragged from its middle.
    ASSERT_EQ(ssg::scrollbar_grab_offset(0, thumbStart, thumbSize),
              thumbSize / 2);
    ASSERT_EQ(ssg::scrollbar_grab_offset(20, thumbStart, thumbSize),
              thumbSize / 2);
}

TEST(gutterFractionTracksTheGrabbedPointAndClamps) {
    int const travel = 10;  // viewportRows - thumbSize
    int const grabOffset = 1;

    // The thumb top is rel - grabOffset, reported over the travel.
    auto const mid = ssg::gutter_fraction(6, grabOffset, travel);
    ASSERT_EQ(mid.numerator, std::uint32_t{5});
    ASSERT_EQ(mid.denominator, std::uint32_t{10});

    // Above the top and below the bottom clamp rather than escaping the range.
    auto const top = ssg::gutter_fraction(0, grabOffset, travel);
    ASSERT_EQ(top.numerator, std::uint32_t{0});
    auto const bottom = ssg::gutter_fraction(100, grabOffset, travel);
    ASSERT_EQ(bottom.numerator, std::uint32_t{10});
    ASSERT_EQ(bottom.denominator, std::uint32_t{10});

    // A thumb that fills the gutter (travel <= 0) never scrolls: 0/1.
    auto const still = ssg::gutter_fraction(3, grabOffset, 0);
    ASSERT_EQ(still.numerator, std::uint32_t{0});
    ASSERT_EQ(still.denominator, std::uint32_t{1});
}

TEST(routePointerPalettePressExecutesTheCandidate) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Palette;
    hit.itemIndex = 4;
    ssg::PointerTargets targets;
    targets.picker_candidate_id = std::string{"view.split"};
    targets.picker_activation = ssg::PickerActivation{
        ssg::SearchMode::Command, ssg::PickerActivationId{5}};

    auto plan = ssg::route_pointer(hit, ssg::PointerButton::left,
                                        ssg::PointerKind::press, false, false,
                                        std::nullopt, targets);
    ASSERT_TRUE(plan.semantic_input.has_value());
    auto const* input =
        std::get_if<ssg::PickerPointerInput>(&*plan.semantic_input);
    ASSERT_TRUE(input != nullptr);
    if (input) {
        ASSERT_TRUE(input->activation == *targets.picker_activation);
        ASSERT_EQ(input->candidateId, std::string{"view.split"});
    }
    ASSERT_FALSE(plan.begins_drag);

    // A palette hit with no resolved candidate id dispatches nothing.
    ssg::PointerTargets const empty;
    auto unresolved = ssg::route_pointer(
        hit, ssg::PointerButton::left, ssg::PointerKind::press, false, false,
        std::nullopt, empty);
    ASSERT_FALSE(unresolved.semantic_input.has_value());
}

// Clicking a row must mean the same as pressing Enter on it. A file
// candidate's id is a PATH, so its typed SubmitPicker mode must remain File;
// this is the pointer half of the same mode-dispatched submit used by Enter.
TEST(routePointerFilePickerPressUsesTheGenericPickerSubmit) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Palette;
    hit.itemIndex = 2;
    ssg::PointerTargets targets;
    targets.picker_candidate_id = std::string{"src/snapshot.cpp"};
    targets.picker_activation = ssg::PickerActivation{
        ssg::SearchMode::File, ssg::PickerActivationId{6}};

    auto plan = ssg::route_pointer(hit, ssg::PointerButton::left,
                                        ssg::PointerKind::press, false, false,
                                        std::nullopt, targets);
    ASSERT_TRUE(plan.semantic_input.has_value());
    auto const* input =
        std::get_if<ssg::PickerPointerInput>(&*plan.semantic_input);
    ASSERT_TRUE(input != nullptr);
    if (input) {
        ASSERT_TRUE(input->activation == *targets.picker_activation);
        ASSERT_EQ(input->candidateId,
                  std::string{"src/snapshot.cpp"});
    }
    ASSERT_FALSE(plan.begins_drag);
}

TEST(routePointerPanelPressSelectsAndActivatesTheNode) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::Panel;
    hit.nodeId = ssg::TreeNodeId{"files:src/main.cpp"};
    ssg::PointerTargets const empty;

    auto plan = ssg::route_pointer(hit, ssg::PointerButton::left,
                                        ssg::PointerKind::press, false, false,
                                        std::nullopt, empty);
    ASSERT_TRUE(plan.semantic_input.has_value());
    auto const* input =
        std::get_if<ssg::TreePointerInput>(&*plan.semantic_input);
    ASSERT_TRUE(input != nullptr);
    if (input) ASSERT_EQ(input->nodeId, *hit.nodeId);
    ASSERT_FALSE(plan.begins_drag);
    ASSERT_FALSE(plan.ends_drag);

    // A panel hit with no node id (the provider-label row / empty area) is inert.
    ssg::RegionHit noNode;
    noNode.region = ssg::HitRegion::Panel;
    auto inert = ssg::route_pointer(
        noNode, ssg::PointerButton::left, ssg::PointerKind::press, false,
        false, std::nullopt, empty);
    ASSERT_FALSE(inert.semantic_input.has_value());
}

TEST(routePointerSearchQueryPressFocusesTheQuery) {
    ssg::RegionHit hit;
    hit.region = ssg::HitRegion::PanelQuery;
    ssg::PointerTargets const empty;

    auto plan = ssg::route_pointer(
        hit, ssg::PointerButton::left, ssg::PointerKind::press, false,
        false, std::nullopt, empty);
    ASSERT_TRUE(plan.semantic_input.has_value());
    ASSERT_TRUE(std::holds_alternative<ssg::SearchQueryPointerInput>(
        *plan.semantic_input));
}

TEST(routeWheelMapsRegionToScrollTarget) {
    using ssg::WheelTarget;
    // The side panel and its gutter scroll the tree.
    ASSERT_TRUE(ssg::route_wheel(ssg::HitRegion::Panel) == WheelTarget::tree);
    ASSERT_TRUE(ssg::route_wheel(ssg::HitRegion::PanelScrollbar) ==
                WheelTarget::tree);
    // The palette and its gutter scroll the client-owned palette window.
    ASSERT_TRUE(ssg::route_wheel(ssg::HitRegion::Palette) ==
                WheelTarget::palette);
    ASSERT_TRUE(ssg::route_wheel(ssg::HitRegion::PaletteScrollbar) ==
                WheelTarget::palette);
    // The editor, its gutter, a tab, and no region all scroll the document.
    for (auto region : {ssg::HitRegion::Editor, ssg::HitRegion::EditorScrollbar,
                        ssg::HitRegion::Tab, ssg::HitRegion::None}) {
        ASSERT_TRUE(ssg::route_wheel(region) == WheelTarget::editor);
    }
}

TEST(edgeScrollDecidesDirectionAtTheContentEdges) {
    // Editor content occupying rows [2, 23): y=2, height=21, bottom=23.
    ssg::Rect const content{0, 2, 79, 21};

    // Not dragging: never auto-scrolls, wherever the pointer is.
    ASSERT_FALSE(ssg::edge_scroll(false, 0, content).has_value());
    ASSERT_FALSE(ssg::edge_scroll(false, 100, content).has_value());

    // Above the top content row -> scroll up.
    auto up = ssg::edge_scroll(true, 1, content);
    ASSERT_TRUE(up.has_value());
    if (up) ASSERT_EQ(*up, -1);

    // At or below the bottom -> scroll down.
    auto atBottom = ssg::edge_scroll(true, 23, content);  // == bottom()
    ASSERT_TRUE(atBottom.has_value());
    if (atBottom) ASSERT_EQ(*atBottom, 1);
    auto below = ssg::edge_scroll(true, 60, content);
    ASSERT_TRUE(below.has_value());
    if (below) ASSERT_EQ(*below, 1);

    // Every interior row (including the top and last visible content rows) is
    // within-viewport -> no auto-scroll (M8-S handles those).
    for (int row = content.y; row < content.bottom(); ++row) {
        ASSERT_FALSE(ssg::edge_scroll(true, row, content).has_value());
    }

    // A degenerate (zero-height) content rect never scrolls.
    ASSERT_FALSE(ssg::edge_scroll(true, 5, ssg::Rect{0, 2, 79, 0}).has_value());
}

SSG_TEST_SUITE(test_terminal_input) {
    RUN(aClickOnAnExternalActionRoutesThroughPointerTargetsToSelectThenAct);
    RUN(decodeInputMapsPrintablesAndNamedKeys);
    RUN(decodeInputModifiedArrows);
    RUN(decodeInputMetaPrefixedCsiFoldsMod);
    RUN(decodeInputTildeHomeEndPlainAndModified);
    RUN(decodeInputStripsLockModifiersFromFunctionalKeys);
    RUN(decodeKittyKeyHandCasesAndCapsLockImmunity);
    RUN(everyEnterEncodingNormalizesToBareEnter);
    RUN(decodeKittyKeySelfIdentifyingAndMalformed);
    RUN(decodeInputModifiedArrowSplitReadsAreIncomplete);
    RUN(decodeInputDeleteKeyPlainAndModified);
    RUN(decodeInputPageKeysPlainAndModified);
    RUN(decodeInputArrowsAndMouse);
    RUN(decodeInputEscapeBoundaryIsBounded);
    RUN(decodeInputCoalescesMetaPrefixIntoModStrokes);
    RUN(modQRequestsApplicationQuitBeforeEditorRouting);
    RUN(noCapabilityReplyIsEverEmittedAsText);
    RUN(anUnboundedSequenceScanCannotHoldTheInputBuffer);
    RUN(aReplySplitAcrossReadsIsStillConsumedWhole);
    RUN(noDcsOrOscQueryMaySolicitAnUnparsedReply);
    RUN(aBracketedPasteIsContentAndNeverKeys);
    RUN(decodeInputPointerPressReleaseDrag);
    RUN(decodeInputPointerCarriesAltModifier);
    RUN(decodeInputPointerSplitReadsAreIncomplete);
    RUN(routePointerLeftPressOnEditorEmitsDocumentInput);
    RUN(routePointerIgnoresNonEditorAndNonLeft);
    RUN(decodeInputPointerRejectsMalformedButTerminatedPayloads);
    RUN(routePointerDragEmitsDocumentMove);
    RUN(routePointerFieldHitEmitsUiActivationCommand);
    RUN(routePointerDragWithoutAnchorOrTargetIsANoOp);
    RUN(routePointerReleaseEndsAuthoritativeDocumentGesture);
    RUN(routePointerAltPressMarksDocumentInputAdditive);
    RUN(altDoubleClickStillSelectsAWordNeverRemoves);
    RUN(altClickRemoveEndToEndLeavesTheSurvivingCaret);
    RUN(routePointerAltDragSetsRangesFromBaseline);
    RUN(routePointerAltDragIgnoresPerMotionModifierBit);
    RUN(routePointerEditorScrollbarScrollsToFraction);
    RUN(routePointerPanelAndPaletteScrollbars);
    RUN(routePointerTabPressActivatesTheTab);
    RUN(routePointerMiddleClickOnATabClosesIt);
    RUN(doubleClickDetectorPairsPressesByTimeAndCell);
    RUN(aDoubleClickOnTheEditorSelectsTheWordNotJustTheCaret);
    RUN(scrollbarGrabOffsetHoldsTheThumbUnderTheCursor);
    RUN(gutterFractionTracksTheGrabbedPointAndClamps);
    RUN(routePointerPalettePressExecutesTheCandidate);
    RUN(routePointerFilePickerPressUsesTheGenericPickerSubmit);
    RUN(routePointerPanelPressSelectsAndActivatesTheNode);
    RUN(routeWheelMapsRegionToScrollTarget);
    RUN(edgeScrollDecidesDirectionAtTheContentEdges);
    std::cout << "\nPassed: " << passed << "  Failed: " << failed << "\n";
    return failed == 0 ? 0 : 1;
}
