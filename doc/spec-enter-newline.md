# Spec: every Enter/Return key inserts a newline

## Problem

`bind(seq({"Enter"}), "text.newline", "editor")` (and the `tree.activate` /
`prompt.submit` bindings) resolve only for a `KeyStroke{Enter}` with **no**
modifier flags. Two real keystrokes never reach those bindings today:

1. **Modified Enter** — Shift+Enter, Ctrl+Enter, Alt+Enter. Under the Kitty
   keyboard protocol these arrive as `CSI 13 ; mods u`, decoded into a
   `KeyStroke{Enter}` whose `shift`/`control`/`alt` flag is set. Because
   `KeyStroke` is matched by value, the modified stroke matches no binding and
   the keypress is silently dropped — pressing Shift+Enter in the editor does
   nothing.

2. **Numpad Enter** — the keypad Enter key. Under the Kitty protocol it is
   reported as `CSI 57414 u` (functional codepoint `KP_ENTER = 57414`), which
   `kittyKeyCode` does not map, so it decodes to `KeyCode::None` and is dropped.
   In legacy application-keypad mode it is `SS3 M` (`ESC O M`), which the SS3
   arm currently discards as `DecodeStatus::none`.

The legacy `\r` / `\n` bytes already decode to a bare `KeyStroke{Enter}`, so
those paths are correct today; only the Kitty and SS3 encodings have the gap.

## Goal

Every Enter/Return key — main Enter and numpad Enter, with **any** combination
of modifier keys held — inserts a newline in the editor (and activates
tree/prompt in those contexts), exactly as an unmodified Enter does.

## Non-goals

- No new bindings, commands, or settings.
- No change to how any other key's modifiers are decoded or matched.
- Alt+Enter etc. gain no distinct behavior; they are deliberately folded onto
  plain Enter. (Confirmed safe: a repository-wide search finds no binding on a
  modified Enter, and the design pins that invariant with a test.)

## Design

### Normalize Enter at one decoder choke point

`decode_input` is the sole public entry that turns input bytes into a
`Decoded`. Introduce one normalization seam there so that **whatever byte
encoding produced it**, a decoded key whose `code == KeyCode::Enter` is
returned with all modifier flags cleared (`shift = alt = control = meta =
false`).

Implementation: rename the current `decode_input` body to an internal
`decodeInputRaw(bytes, inputExhausted, consumed)` and make `decode_input` a
thin wrapper that calls it once and, when the result is
`DecodeStatus::key && stroke.code == KeyCode::Enter`, zeroes the modifier
flags before returning. This makes "all Enter is bare Enter" true for every
present and future decoder arm in exactly one place, rather than sprinkling
flag-clearing across arms. The recursive alt-chord call at the top of the
current body calls the raw inner function (its own Enter results are then
normalized by the outer wrapper once, at the boundary the caller sees).

### Map the numpad-Enter encodings to `KeyCode::Enter`

- **Kitty**: add `case 57414: return ssg::KeyCode::Enter;` to `kittyKeyCode`
  so `CSI 57414 u` decodes to Enter (then the choke point strips any modifiers).
- **SS3**: in the `ESC O <final>` arm, decode final byte `M` to a
  `KeyStroke{Enter}` (numpad Enter in application-keypad mode) instead of
  discarding it. Other SS3 finals keep today's `DecodeStatus::none`.

### Close the keymap seam: reject modified-Enter bindings (pit of success)

Because the decoder folds every Enter to a bare stroke, a user keymap binding
on `Shift+Enter` / `Ctrl+Enter` / `Alt+Enter` / `Meta+Enter` (or a modified
numpad Enter) can never fire — it would be accepted-but-dead. To keep the
keymap hard to misuse, `KeymapMatcher::validate` gains a rule: a binding whose
single stroke has `code == KeyCode::Enter` **and any modifier flag set**
(`shift || control || alt || meta`) is rejected with a new
`KeymapErrorCode::ModifiedEnterBinding` ("Enter cannot be combined with a
modifier; every Enter inserts a newline"). Bare `Enter` bindings remain valid.
This makes the decoder-normalization invariant enforceable at the layer where
users author bindings, so misconfiguration is a validation error rather than a
silent no-op. `applyKeymapBind` (the runtime rebind path) surfaces the same
error, so a live `keymap.bind Shift+Enter ...` is refused.

## Oracles (tests, in `tests/test_ssg_app.cpp` decoder suite)

All assert on `ssg::app::decode_input(...)`'s `Decoded`:

1. **Plain Enter unchanged**: `"\r"` and `"\n"` → key, code Enter, no
   modifiers (guards against regression).
2. **Kitty modified Enter is bare Enter**:
   - `"\x1b[13;2u"` (Shift+Enter) → key, code Enter, `shift == false`.
   - `"\x1b[13;5u"` (Ctrl+Enter) → key, code Enter, `control == false`.
   - `"\x1b[13;3u"` (Alt+Enter) → key, code Enter, `alt == false`.
   - `"\x1b[13;8u"` (Ctrl+Alt+Enter) → key, code Enter, all flags false.
   - `"\x1b[13;33u"` (Meta+Enter, Kitty meta bit5) → key, code Enter, all
     flags false (locks the meta path, which only the Kitty mode sets).
3. **Kitty numpad Enter**: `"\x1b[57414u"` → key, code Enter, no modifiers;
   and modified `"\x1b[57414;2u"` → key, code Enter, no modifiers.
4. **SS3 numpad Enter**: `"\x1bOM"` → key, code Enter, no modifiers, consumed 3.
   Also **Alt-prefixed SS3 numpad Enter** `"\x1b\x1bOM"` (the recursive
   alt-chord path) → key, code Enter, no modifiers — proving normalization
   still applies through the alt-chord recursion.
5. **Invariant guard — no default binding on a modified Enter**: iterate the
   default keymap; assert no binding's single stroke is `KeyCode::Enter` with
   any modifier flag set. Lives in `tests/test_input.cpp`.
6. **Validator rejects a modified-Enter binding**: build a keymap that binds
   `Shift+Enter` (and separately `Ctrl+Enter`) to a command; assert
   `validate` reports `KeymapErrorCode::ModifiedEnterBinding`, while a bare
   `Enter` binding validates clean. Assert `applyKeymapBind` refuses a live
   modified-Enter rebind with the same code. Lives in `tests/test_input.cpp`.

## Plan

1. Add `doc/spec-enter-newline.md` (this file); review; fold findings.
2. `kittyKeyCode`: map `57414 -> Enter`.
3. SS3 arm: final `M` -> `KeyStroke{Enter}`.
4. Split `decode_input` into `decodeInputRaw` + a normalizing wrapper that
   clears modifier flags on an Enter key result.
5. Add `KeymapErrorCode::ModifiedEnterBinding`; enforce it in
   `KeymapMatcher::validate` (and thereby `applyKeymapBind`).
6. Add oracles 1-6.
7. Gate (`bash scripts/check.sh`); code review; fold; merge.
