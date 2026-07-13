# Code review — d134f8d "Palette step 2: PromptKind::palette and results-pane projection"

Reviewer: code-review agent

Scope: the diff for commit d134f8d. Build + `test_render` + `ssg_prompt_status_tests`
were run and pass. No non-exhaustive `switch` was introduced (`prompt_row_count`
is the only `switch` on `PromptKind`; the codec uses value-arrays, and
`decode_present(PromptKind)` round-trips because `decode_enum` matches by
underlying value, so array order is irrelevant and the new value 5 decodes).
`valid_request` correctly treats `palette` as 1 input / no options.

## SHOULD — palette open is not client-scoped; suppresses the document for every client

**Where:** `src/runtime/snapshot.cpp` `shell_view` (lines ~72-78) and
`src/render.cpp` (the `if (shell.palette)` branch, ~384-386).

`shell_view` decides the palette is open purely from the shared prompt surface:

```cpp
bool const palette_open = prompt.active() && prompt.request() &&
                          prompt.request()->kind == PromptKind::palette;
```

`prompt` is a single `PromptSurface` member of `Impl` (editor_runtime_internal.h:70),
not per-client, and neither `snapshot()` nor `shell_view()` is passed the
`client_id`. But `doc/spec-palette.md` states the authoritative shared palette
state is "the per-mode candidate list plus **which client (if any) has the palette
open**." Because the new projection *replaces* the document (render suppresses
`paint_document`/`paint_scrollbar`/caret whenever `shell.palette` is set), opening
the palette on one client now blanks the editor pane for **all** attached clients —
and any client that did not send a `PaletteReport` gets an empty pane
(`palette_report.rows` empty → projection with no rows). This is the first prompt
that fully blanks the pane, so the pre-existing "prompt is global" limitation
becomes a visible cross-client regression. The projection/suppression should key
off which client owns the open palette, not a global flag.

(If step 2 intentionally defers per-client prompt ownership, this is still worth
tracking, since the render-side suppression is what makes it user-visible.)

## SHOULD — selection role lands only on non-text cells of the selected row

**Where:** `src/render.cpp` `paint_palette` (~215-231).

`fill_rect` tags the whole selected row with `SemanticRole::selection`, but the
subsequent `paint_text` calls overwrite the glyph cells with
`SemanticRole::foreground` (label) and `SemanticRole::line_number` (detail). So on
the selected row only the *gap/trailing* cells actually carry
`SemanticRole::selection`. If a label fills the row width (no gap, no detail),
the selected row ends up with **zero** `SemanticRole::selection` cells, even
though it is selected. The new test only passes because its labels are short and
leave trailing filled cells; it gives false confidence in the
"selected row uses the selection role" contract. The background *color* is still
correct (selected cells keep `selected_bg`), so this is a semantic-role
robustness gap rather than a visual one — but the codebase (and this test) treat
the role as the selection contract.

## NIT — `PaletteReport::query` is threaded through the API but never consumed

`query` is carried on `PaletteReport` and plumbed through
`EditorRuntime::snapshot` → `Impl::sections` → `Impl::shell_view`, but nothing in
this commit reads it (only `rows` and `selected` are projected). The commit
message says "query renders in the header," but no header projection of the
client's query exists here. Either the header wiring is deferred to a later step
(then the field is dead for now) or the commit message overstates what landed.
