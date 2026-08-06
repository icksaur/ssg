# Spec: find the word under the cursor (leader,8)

## Goal

A new command, `find.word_under_cursor`, bound by default to the leader chord
`leader,8` (`{Escape, Digit8}`). It seeds the find query with the word the
primary caret is on and opens the find prompt over the whole document, so the
user can jump between occurrences of the identifier under the cursor without
typing it.

## Behaviour

Given the active document's text and the primary selection:

- If the primary selection is **not** a caret (it covers a range), the needle is
  the selected text verbatim. This matches `select.add_next_occurrence`, which
  already treats a non-empty selection as the thing to search for.
- If the primary selection **is** a caret, the needle is the word covering the
  caret. A "word" is the maximal run of word bytes, where a word byte is an
  ASCII letter, digit, `_`, or any non-ASCII byte (`>= 0x80`) — the exact
  classification the existing word motions and `select.word_*` use
  (`Selection.cpp` `categoryAt`). Because a caret sits *between* bytes, the word
  is the one the caret is inside; if the caret sits immediately past a word's
  last byte (the common "cursor at end of identifier" case) that trailing word
  is used.
- If no word is found (the caret is in whitespace or punctuation and not
  adjacent to a word) the command is a **no-op that reports success** — it does
  not open an empty find prompt. Opening an empty find on a mis-stroke would be
  more surprising than doing nothing.

When a needle is found the command behaves like `find.open` with that query,
but it always searches **literally over the whole document**: it opens the find
controller with the current options except `regex` and `selectionOnly` forced
**off**, and with **no range restriction** (`std::nullopt`). "Find this exact
word" is inherently a literal, whole-document search; forcing `regex` off means
a needle containing regex metacharacters (or, for a selection needle, a newline)
can never be silently reinterpreted as a pattern, and the prompt shows the clean
word rather than an escaped form. `caseSensitive` and `wholeWord` are inherited
unchanged. Turning `regex`/`selectionOnly` off is a visible toggle change the
user can see and undo. The active match is revealed and the find prompt opens
with the query pre-filled.

Note: a word byte is any ASCII letter, digit, `_`, or any non-ASCII byte
(`>= 0x80`). For a literal seed this is byte-exact and encoding-agnostic, but it
means a multi-byte UTF-8 glyph embedded in an identifier is glued into the
needle — a known, intended outcome, consistent with the existing word motions.

## Ownership and placement

- `FindReplaceCommand::FindWordUnderCursor` is added to the enum in
  `include/ssg/FindReplace.h`.
- The word-byte predicate is centralised. Today it is copied in three places
  (`Selection.cpp` `categoryAt`, `TextInputCommands.cpp` `category`, and would be
  a fourth copy here). A single `inline bool ssg::isWordByte(unsigned char)` in a
  new header `include/ssg/WordClassification.h` becomes the one definition, and
  the two existing copies are routed through it so the new command cannot drift
  from what `select.word_*` / `cursor.word_*` / `select.add_next_occurrence`
  consider a word.
- The handler lives in `src/runtime/editing.cpp` `bindFindReplace`, reusing the
  `find.open` open/reveal/prompt sequence with the computed needle, forced
  literal options, and a null range. The needle is computed by
  `Selection::wordOrCoveredText(text)`: the selected substring for a range, or
  the word the caret sits in (or touches on its trailing side) for a caret.
  (Originally a file-local `wordUnderCaret` helper, argued to not belong on
  `Selection`; the R4 refactor — doc/features/r4-runtime-collaborators.md —
  moved it onto `Selection` as a pure value transform, since word-run extraction
  over a selection is the same module that already owns `selectWordAtPosition`,
  and it is unit-tested there in isolation.)
  - **Bounds:** the caret byte offset can equal `text.size()` and the document
    can be empty. The method reads `text[i]` only when `i < text.size()`, reads
    the "word to the left" as byte `i-1` only when `i > 0`, and returns an empty
    string (no word) otherwise. It never indexes out of range.
  - The needle source is the **primary** selection (`selections.primary()`),
    matching `find.open`; this differs from `select.add_next_occurrence`'s
    `back()` and is the right choice for a find seed under multiple carets.
- The command is registered as a bare (payload-free) command in
  `registerFindReplaceCommands`, alongside `find.open`.
- The default binding `{Escape, Digit8}` in `defaultTerminalKeymap`
  (`src/EditorRuntime.cpp`) is bound in the **`editor`** context: it acts on the
  editor's caret and document, so the narrower context is the truer one and
  avoids hijacking a literal `8` typed after Escape while a prompt owns the
  keyboard. This mirrors the existing editor-context clipboard bindings; the
  user-visible asymmetry with the global `find.open` (leader,8 does nothing while
  a prompt or panel is focused) is deliberate and acceptable.

## Tests / oracles

1. **Binding resolves.** From the published keymap, `{Escape, Digit8}` resolves
   to `find.word_under_cursor` in the `editor` context (mirrors the existing
   curated-keymap resolve test).
2. **Caret in a word seeds that word.** Placing the caret inside an identifier
   and dispatching the command opens find with the query equal to the whole
   identifier and reveals a match; the find prompt is open with that query.
3. **Caret at the end of a word seeds that word.** Caret immediately after the
   last byte of an identifier yields the same needle.
4. **Selection wins over the caret word.** With a non-caret selection, the
   needle is the selected text, even when it spans a word boundary or multiple
   lines.
5. **No word is a no-op.** Caret in whitespace opens no find prompt and reports
   success; the find controller stays closed. An empty document is a no-op.
6. **Multi-occurrence navigation.** After seeding, `find.next` moves to the next
   occurrence of the word (proves the query, not just the prompt text, is live).
7. **Forced-literal search.** With `regex` and `selectionOnly` toggled ON
   beforehand, seeding a needle that contains a regex metacharacter (e.g. a
   selection of `a.b`) still finds the literal text, and the published find
   options show `regex` and `selectionOnly` OFF.

Each guard is mutation-tested: breaking the expansion bounds, the
selection-wins branch, the no-word guard, the forced-literal options, or the
null-range must fail a test.
