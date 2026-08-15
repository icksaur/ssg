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

// The fully-formed tree-provider state a commit installs. `create`, when set, is a
// complete snapshot the commit installs (a Git/Symbols provider that was absent, or one
// present under the wrong kind); `activate` names the provider that becomes active.
// Preflight rejects a missing Filesystem provider, so a create snapshot is never one.
struct TreeBackingPlan {
    TreeProviderId activate;
    std::optional<TreeProviderSnapshot> create;
};

// --- The inputs preflight reads (all by value; no callbacks) ------------------------

// An existing tree provider, as its binding plus current revision. Preflight needs the
// revision: replacing a provider requires a strictly greater revision, so a wrong-kind
// recreate must be stamped above the one it replaces for the commit to be infallible.
struct TreeProviderPresence {
    TreeProviderBinding binding;
    TreeRevision revision{0};
};

struct TransitionInputs {
    WholeScreenTruth truth;
    ValidatedSchema schema;                    // to rebuild the replacement aggregate
    PromptSurface prompt;                       // copied; preflight opens/cancels on it
    // The tree providers that already exist. Matching needs id+kind (activating by id
    // alone cannot prove the kind) and the revision (to stamp a valid recreate).
    std::vector<TreeProviderPresence> presentProviders;
    TreeRevision nextTreeRevision{0};           // revision stamped on a fresh provider
};

// --- The prepared, fully-formed replacement state -----------------------------------

// An opaque bundle of fully-formed replacement values, constructed ONLY by
// prepareTransition so a caller cannot assemble an inconsistent (truth, interaction,
// prompt, tree) combination. The runtime installs it as one atomic owner swap; it
// computes nothing and calls nothing fallible. File-finder candidate content is NOT here:
// the candidate list is picker content on its own data channel, refreshed by the runtime
// when the open picker becomes File, not part of the interaction aggregate's truth.
class PreparedTransition {
public:
    [[nodiscard]] const WholeScreenTruth& truth() const noexcept { return truth_; }
    [[nodiscard]] const UiInteractionState& interaction() const noexcept {
        return interaction_;
    }
    [[nodiscard]] const PromptSurface& prompt() const noexcept { return prompt_; }
    [[nodiscard]] const std::optional<TreeBackingPlan>& tree() const noexcept {
        return tree_;
    }

private:
    PreparedTransition(WholeScreenTruth truth, UiInteractionState interaction,
                       PromptSurface prompt, std::optional<TreeBackingPlan> tree)
        : truth_{std::move(truth)},
          interaction_{std::move(interaction)},
          prompt_{std::move(prompt)},
          tree_{std::move(tree)} {}

    friend struct TransitionBuilder;

    WholeScreenTruth truth_;
    UiInteractionState interaction_;
    PromptSurface prompt_;
    std::optional<TreeBackingPlan> tree_;
};

// Preflight a transition against `inputs`. Returns nullopt on rejection (an out-of-domain
// resource: a missing Filesystem tree provider, an unknown picker, or a prompt that
// refuses to open/cancel), having touched nothing. On success the returned bundle carries
// the fully-formed replacement state.
[[nodiscard]] std::optional<PreparedTransition> prepareTransition(
    const CommandTransition& transition, const TransitionInputs& inputs);

}  // namespace ssg
