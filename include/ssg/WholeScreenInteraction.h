#pragma once

// The whole-screen interaction aggregate, built as a PURE function of semantic truth.
// Presence and focus over the whole-screen schema are not owned as loose state that a
// caller mutates; they are recomputed from WholeScreenTruth -- the panel's presence, the
// selected panel provider, whether the finder is open, and the base focus -- so the tree
// the client lays out always reflects the authoritative subsystems, and a schema
// generation change is handled by simply rebuilding from the same truth over the new
// schema (the migration contract: recompute presence from truth, reconcile captures,
// reset the basis, and never strand base focus on an absent panel).

#include <cstdint>

#include <ssg/InteractionState.h>  // UiInteractionState
#include <ssg/KeyboardFocus.h>     // BaseFocus

namespace ssg {

// The closed set of panel providers -- the only surfaces that can occupy the side panel.
// A dedicated domain (not ViewSurface, which also admits TabView/FindResults) makes an
// out-of-domain selection unconstructable rather than silently coerced.
enum class PanelProvider : std::uint8_t { FileTree, GitStatus, Symbols };

// The semantic inputs that determine whole-screen presence and focus, read from the
// authoritative subsystems (ShellState panel state + active provider, the open picker).
// A plain value; buildWholeScreenInteraction is a pure function of it.
struct WholeScreenTruth {
    // Whether the side panel is shown (ShellState::panelRequested).
    bool panelPresent = false;
    // The selected panel provider. Its node is present only when the panel is present;
    // the persistent last-active choice lives in the separate provider hint, not here.
    PanelProvider selectedProvider = PanelProvider::FileTree;
    // Whether a picker (command palette or file finder) is open -- both publish through
    // the palette section and show the findresults content surface.
    bool finderOpen = false;
    // The persistent base focus. Panel is honored only when the panel is present; else
    // it falls back to Editor, so base focus never strands on an absent panel.
    BaseFocus baseFocus = BaseFocus::Editor;
};

// Build the interaction aggregate for `schema` from `truth`: presence hides the panel and
// ALL its provider children when the panel is absent, else the two non-selected panel
// providers, and whichever of tabview/findresults
// the finder state excludes; base focus is Editor unless the panel is present and focused;
// the finder capture is pushed on the findresults node when the finder is open. Rebuilding
// with the same truth over a new schema generation IS the migration.
[[nodiscard]] UiInteractionState buildWholeScreenInteraction(
    ValidatedSchema schema, const WholeScreenTruth& truth);

}  // namespace ssg
