# Simplification starting points

## Guardrail

Simplification must preserve the library/client boundary. `EditorSession`,
`SessionSnapshot`, and the medium-neutral UI-VM remain the contract for the
terminal application and future in-memory native GUI clients. Presentation
geometry, pixels, toolkit objects, and device I/O stay outside core. Do not
move editor behavior or product policy into a client to make core smaller.

## Ordered work

| Priority | Task | Intended result |
|---|---|---|
| Completed | Remove stale transport vocabulary | Deleted `OptionalSubsystem::Http` and removed stale transport wording. Command argument metadata remains because it still distinguishes documented from internal payloads. |
| Completed | Put pane/view behavior in core | Pane topology and active-pane policy now live per attachment in core; grid projects the published topology and resolves directional neighbors. |
| Completed | Remove grid leakage from core | Replaced `GridActionResult` with the medium-neutral `ViewActionResult` at the ScriptHost/client seam. |
| Completed | Reduce `GridFrame` | `GridPresentation` now owns the matched semantic snapshot and reduced grid result; `GridFrame` retains only grid projection products and `GridProjectionState` remains presenter-private. |
| Completed | Remove premature client-profile policy | Deleted `ClientUiProfile` and its unsupported-profile solver branch; no current client requires capability negotiation. |
| Completed | Privatize interaction intermediates | Kept `UiFrame` public while moving mutable/intermediate types such as `WholeScreenTruth`, `UiInteractionState`, and `WholeScreenSchema` behind `InteractionAuthority` where native clients do not need them. |
| Completed | Collapse publication delta surface | Removed the unused `UiFrame` delta/replay protocol while retaining `UiFrameVersion` for stale UI-activation validation. Complete immutable `UiFrame` publications remain the client contract. |
| Completed | Remove custom chrome composition | Deleted `ssg.chrome`, its decoder and composition plumbing. Core now publishes fixed semantic header/footer `UiNode` subtrees, while terminal row packing and glyph projection remain in `ssg_grid` behind neutral UI-region APIs. |
| Completed | Separate UI-VM from grid helpers | `Widget.h` now contains only medium-neutral widget vocabulary. Terminal row fitting, cell measurement, glyph/text composition, input-line layout, and `WidgetStack` live in `ssg_grid::WidgetLayout`; `ViewSurface` remains the required opaque data-channel identity. |
| Later | Unify input and view-action vocabularies | Consolidate overlapping scroll, selection, pane-focus, and client-owned input forms into a smaller medium-neutral typed action model. Preserve authoritative editor handling and local client responsiveness. |

## Execution rule

Each task is a separate design and implementation phase: write a temporary
spec for the affected public ownership boundary, review it, make the smallest
coherent cut, run the appropriate gate, then review the code before starting
the next task.
