# Code review — objectify refactor (cpp-objects: Wave A/E/B)

Reviewer: gpt-5.5 code-review agent (diff master..cpp-objects) + primary-session
independent spot-check. Scope: HitTester, Viewport, PaletteSearcher, KeyCodec/
KeymapMatcher/SemanticInputRouter, the 3 git-mv noun renames, and the Wave B command
interpreters (TextInputInterpreter, EditInterpreter, FileCommandsCommandSet::pathPrompt,
EditHistoryCoordinator).

## Result: NO SIGNIFICANT ISSUES

The reviewer found no bugs, behavior changes, lifetime issues, or broken invariants.
Build is clean (0 warnings); all 71 non-perf tests pass on cpp-objects.

## Independent spot-check (primary session) — confirms clean

1. Behavior preservation: each free-fn -> method is a mechanical wrap (same body,
   args, result). Verified the moved bodies are unchanged.
2. std::move safety: EditHistoryCoordinator::applyTextInput/applyEditCommand move
   their OWN by-value params (settings/arguments) exactly once into the interpreter
   call — no double-move / use-after-move.
3. EditHistoryCoordinator lifetime: stores Document&/DocumentHistory& by reference;
   constructed only as a temporary at call sites
   (`EditHistoryCoordinator{document, history}.applyTextInput(...)`), so the refs
   never outlive their referents. No stored/escaping instance.
4. Call-site completeness: grep for old free-fn names returns only false positives
   (the `paletteGhost` struct FIELD `request.paletteGhost`; the new
   `EditHistoryCoordinator::applyTextInput/applyEditCommand` method decls). No missed
   caller, no mis-captured first-arg from the perl/sed transforms.
5. git mv: no dangling old headers; all include paths + cmake source paths updated.
6. Protocol/wire: protocol.cpp:319 changed only the call FORM (interpreter method);
   test_protocol/test_settings goldens pass unchanged.

## Verdict: cpp-objects Wave A/E/B is sound. Proceed to Wave C.

## Wave C + D review

Reviewer: gpt-5.5 code-review agent (diff master..cpp-objects) + primary-session
independent spot-check. Scope: TextCodec, Session/DocumentSnapshotCodec, GraphemeLayout,
Renderer, ProtocolCodec (Wave C); FindMatcher/WorkspaceReplacer/FindReplaceDeltaCodec,
WorkspaceSearcher/SearchNavigator/SearchDeltaCodec, SelectionNavigator,
TreeProviderSnapshot factories/TreeDeltaCodec, JournalCodec (Wave D); ccache + check.sh.

### Result: NO SIGNIFICANT ISSUES
The reviewer found no bugs, behavior changes, lifetime issues, or broken invariants.

### Independent spot-check (primary session) — confirms clean
1. SelectionNavigator::resolvePosition is called STATICALLY everywhere
   (SelectionNavigator::resolvePosition(...)); zero `{}.resolvePosition` instance
   misuse. apply() is the instance form. No static-vs-instance mismatch.
2. Zero old free-fn names remain as free calls repo-wide (method decls in the new
   classes are the only matches). The hand-resolved session_snapshot.cpp conflict
   (SearchDeltaCodec + FindReplaceDeltaCodec derive/replay) is correct.
3. spec.md remains a symlink (mode 120000) across all git-mv commits.
4. Wire goldens (test_protocol + test_settings) pass — ProtocolCodec wrap changed
   only the call form, not the bytes. Full gate: build clean (0 warnings), 71/71.
5. git mv correctness: text_encoding->text_codec, layout->grapheme_layout,
   render->renderer all clean; renderer.cpp carries the GraphemeLayout{}.computeRun
   calls (layout<->render cross-dependency resolved by git rename detection).

### Verdict: cpp-objects Waves C + D are sound. Merging to master.
