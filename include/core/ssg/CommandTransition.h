#pragma once

// The closed set of multi-subsystem interaction transitions. InteractionAuthority owns
// their private preflight and atomic installation, so callers can request a transition
// without assembling mutable focus, presence, schema, or tree-provider state.
//
// Simple pane/editor focus changes are NOT transitions here -- they are typed methods on
// the interaction aggregate. Only changes that touch several subsystems at once (panel +
// tree provider, or picker + prompt) are transitions.

#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include <ssg/Picker.h>          // PickerKind, PickerCatalog
#include <ssg/PromptSurface.h>   // PromptSurface
#include <ssg/TreeModel.h>       // TreeProviderBinding, TreeProviderSnapshot, TreeRevision
#include <ssg/UiTree.h>          // ValidatedSchema, UiSchema, node id constants

namespace ssg {

// --- The panel-provider domain ------------------------------------------------------

// The only bindings accepted by panel-provider transitions, in cycle order.
[[nodiscard]] std::span<const TreeProviderBinding> builtInPanelTreeProviders();
[[nodiscard]] TreeProviderBinding builtInPanelTreeProvider(
    TreeProviderKind kind);

enum class CycleDirection : std::uint8_t { Next, Previous };

// The next provider when cycling the panel selection. Cycling belongs to the provider
// domain, so next/previous-provider commands resolve to a SwitchPanelProvider transition
// (which preserves panel visibility and focus) rather than being transition variants.
[[nodiscard]] TreeProviderBinding cyclePanelTreeProvider(
    const TreeProviderBinding& provider, CycleDirection direction);

// The region the active prompt's focus anchors on, derived from the prompt (never stored),
// or nullopt when no prompt is active. Shared by the transition builder and the authority.
[[nodiscard]] std::optional<PromptRegion> activePromptRegion(const PromptSurface& prompt);

// --- The transition requests --------------------------------------------------------

struct TogglePanel {};
// Show the panel on a chosen provider (clicking the path/branch): shows and focuses the
// panel, and reselecting the shown provider hides it.
struct ShowPanelProvider {
    TreeProviderBinding binding;
};
// Change the panel's provider backing WITHOUT changing its visibility or focus (cycling
// next/previous provider): the panel stays hidden if hidden, shown if shown, and focus is
// untouched. Never toggles the panel off.
struct SwitchPanelProvider {
    TreeProviderBinding binding;
};
struct OpenFinder {
    PickerKind picker = PickerKind::Command;
};
struct CloseFinder {};

using CommandTransition = std::variant<TogglePanel, ShowPanelProvider,
                                       SwitchPanelProvider, OpenFinder, CloseFinder>;

}  // namespace ssg
