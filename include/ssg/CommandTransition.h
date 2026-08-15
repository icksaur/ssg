#pragma once

// The closed set of multi-subsystem interaction transitions and their two-phase
// application. A transition is the sole WRITER of whole-screen truth;
// buildWholeScreenInteraction is the READER. prepareTransition is a fallible preflight
// that MUTATES NOTHING and, on success, yields a PreparedTransition -- a data-only bundle
// of fully-formed replacement values (next truth, the rebuilt interaction aggregate, the
// post-open/cancel prompt surface, and any tree-provider backing). The runtime installs
// the bundle as one atomic owner swap; because every fallible step happened in preflight,
// the install is infallible. A rejected preflight returns nullopt and leaves the caller's
// subsystems untouched.
//
// Simple pane/editor focus changes are NOT transitions here -- they are typed methods on
// the interaction aggregate. Only changes that touch several subsystems at once (panel +
// tree provider, or picker + prompt) are transitions.

#include <optional>
#include <string_view>
#include <variant>

#include <ssg/Picker.h>          // PickerKind, PickerCatalog
#include <ssg/PromptSurface.h>   // PromptSurface
#include <ssg/TreeModel.h>       // TreeProviderBinding, TreeProviderSnapshot, TreeRevision
#include <ssg/UiTree.h>          // ValidatedSchema, UiSchema, node id constants
#include <ssg/InteractionState.h>       // UiInteractionState
#include <ssg/WholeScreenInteraction.h> // PanelProvider, WholeScreenTruth

namespace ssg {

// --- The panel-provider domain ------------------------------------------------------

// The presentation label a panel provider is known by ("files"/"git"/"symbols").
[[nodiscard]] std::string_view panelProviderLabel(PanelProvider provider);

// The tree-provider backing a panel provider activates. This is the one place the
// PanelProvider -> tree-provider correspondence lives.
[[nodiscard]] TreeProviderBinding panelProviderTreeBinding(PanelProvider provider);

enum class CycleDirection : std::uint8_t { Next, Previous };

// The next provider when cycling the panel selection. Cycling belongs to the provider
// domain, so next/previous-provider commands resolve to a ShowPanelProvider transition
// rather than being transition variants of their own.
[[nodiscard]] PanelProvider cyclePanelProvider(PanelProvider provider,
                                               CycleDirection direction);

// --- The transition requests --------------------------------------------------------

struct TogglePanel {};
struct ShowPanelProvider {
    PanelProvider provider = PanelProvider::FileTree;
};
struct OpenFinder {
    PickerKind picker = PickerKind::Command;
};
struct CloseFinder {};

using CommandTransition =
    std::variant<TogglePanel, ShowPanelProvider, OpenFinder, CloseFinder>;

// --- The tree-provider backing a commit installs ------------------------------------

// How the tree provider is made active on commit. `create`, when set, is a fully-formed
// snapshot the commit installs before activating (a Git/Symbols provider that was absent);
// otherwise the provider is already present and is merely activated. Preflight rejects a
// missing Filesystem provider outright, so a create snapshot is never a Filesystem one.
struct TreeBackingPlan {
    TreeProviderId activate;
    std::optional<TreeProviderSnapshot> create;
};

// --- The inputs preflight reads (all by value; no callbacks) ------------------------

struct TransitionInputs {
    WholeScreenTruth truth;
    ValidatedSchema schema;                    // to rebuild the replacement aggregate
    PromptSurface prompt;                       // copied; preflight opens/cancels on it
    std::vector<TreeProviderId> presentProviders;  // which tree providers already exist
    TreeRevision nextTreeRevision{0};           // revision stamped on a created provider
};

// --- The prepared, fully-formed replacement state -----------------------------------

// A data-only bundle. The runtime installs it as one atomic owner swap; it computes
// nothing and calls nothing fallible.
struct PreparedTransition {
    WholeScreenTruth truth;
    UiInteractionState interaction;
    PromptSurface prompt;
    std::optional<TreeBackingPlan> tree;
    bool rebuildFileCandidates = false;
};

// Preflight a transition against `inputs`. Returns nullopt on rejection (an out-of-domain
// resource: a missing Filesystem tree provider, an unknown picker, or a prompt that
// refuses to open/cancel), having touched nothing. On success the returned bundle carries
// the fully-formed replacement state.
[[nodiscard]] std::optional<PreparedTransition> prepareTransition(
    const CommandTransition& transition, const TransitionInputs& inputs);

}  // namespace ssg
