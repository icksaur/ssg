#pragma once

// The whole-screen interaction aggregate, built as a PURE function of semantic truth.
// Presence and focus over the whole-screen schema are not owned as loose state that a
// caller mutates; they are recomputed from WholeScreenTruth -- the panel's presence,
// whether the finder is open, and the base focus -- so the tree
// the client lays out always reflects the authoritative subsystems, and a schema
// generation change is handled by simply rebuilding from the same truth over the new
// schema (the migration contract: recompute presence from truth, reconcile captures,
// reset the basis, and never strand base focus on an absent panel).

#include <cstdint>
#include <optional>

#include <ssg/InteractionState.h>  // UiInteractionState
#include <ssg/KeyboardFocus.h>     // BaseFocus
#include <ssg/Picker.h>            // PickerKind
#include <ssg/PaletteSearcher.h>   // PalettePresenceOverlay
#include <ssg/PromptSurface.h>     // PromptRegion

namespace ssg {

// The closed set of panel providers -- the only surfaces that can occupy the side panel.
// A dedicated domain (not ViewSurface, which also admits Document/FindResults) makes an
// out-of-domain selection unconstructable rather than silently coerced.
enum class PanelProvider : std::uint8_t { FileTree, GitStatus, Symbols };

// The semantic inputs that determine whole-screen presence and focus, owned by the
// interaction aggregate (not read from scattered subsystems). A plain value;
// buildWholeScreenInteraction is a pure function of it.
struct WholeScreenTruth {
    // Whether the side panel is shown.
    bool panelPresent = false;
    // Whether chrome, panels, and tabs are hidden so only the document remains.
    bool distractionFree = false;
    // The open picker, if any -- the aggregate owns the picker IDENTITY, not merely a
    // finder-open bit, so truth distinguishes command from file candidates. Both kinds
    // show the findresults content surface.
    std::optional<PickerKind> openPicker;
    // The current base focus. Panel is honored only when the panel is present; else it
    // falls back to Editor, so base focus never strands on an absent panel.
    BaseFocus baseFocus = BaseFocus::Editor;
    // The base focus to restore when the panel hides -- retained by the aggregate across
    // a panel show so hiding the panel returns focus where it was, not blindly to Editor.
    BaseFocus panelReturnFocus = BaseFocus::Editor;
    // Whether the active document raises a draft-conflict notice. Unlike the prompt
    // region (derived from the PromptSurface at build time), the notice's source is
    // per-document runtime state the interaction aggregate does not otherwise hold,
    // so the runtime reconciles it into truth after each dispatch. Gates the notice
    // region's presence only.
    bool noticePresent = false;

    // Whether any file is externally modified (the external-modification section is
    // non-empty). Like noticePresent, this is per-flow runtime state the aggregate
    // does not otherwise hold, reconciled into truth after each dispatch AND in the
    // watcher drain. Gates the external-modification node's presence and, with it,
    // whether the external focus capture can anchor.
    bool externalModificationPresent = false;
    // Whether the user has focused the external-modification bar. The capture is
    // DERIVED from this each rebuild (never pushed imperatively), so it survives
    // unrelated rebuilds and can never be stacked twice; it is pushed only when the
    // node is also present. Cleared whenever presence drops, so a later disk event
    // that re-raises the bar never reactively steals focus.
    bool externalFocusHeld = false;

    friend bool operator==(const WholeScreenTruth&, const WholeScreenTruth&) = default;
};

// Build the interaction aggregate for `schema` from `truth` and the active prompt's region.
// Presence hides the panel when absent and whichever editor/find-results branch the open
// picker excludes; base focus is Editor unless the panel is present and focused.
// `promptRegion` is
// DERIVED from the authority-owned PromptSurface at build time (not stored in truth, so it
// cannot drift): when set, a single prompt-focus capture is anchored on the region's host
// node -- header for a Palette prompt (command palette, file finder), footer otherwise --
// so keystrokes route to that input line while findresults stays displayed content.
// Rebuilding with the same inputs over a new schema generation IS the migration.
[[nodiscard]] UiInteractionState buildWholeScreenInteraction(
    ValidatedSchema schema, const WholeScreenTruth& truth,
    std::optional<PromptRegion> promptRegion = std::nullopt);

// Derive the local picker layer from the same closed/open projections used by
// authoritative interaction, preserving every non-picker semantic input.
[[nodiscard]] PalettePresenceOverlay derivePickerPresenceOverlay(
    const ValidatedSchema& schema, const WholeScreenTruth& truth);

}  // namespace ssg
