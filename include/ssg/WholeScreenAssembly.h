#pragma once

// The whole-screen tree assembly: from the built-in status projection AND an optional
// ssg.chrome composition, build the canonical whole-screen UiComposition
//
//   root
//   ├─ header            (composed override, else built-in from headerFields)
//   ├─ body   Row Flex
//   │  ├─ panel   Col Exact(dimensions.panelTargetWidth) [ filetree, gitstatus ]
//   │  └─ content Col Flex                               [ tabview,  findresults ]
//   └─ footer            (composed override, else built-in from footerFields + hint)
//
// This function OWNS the fallback/override rule: a ssg.chrome-composed header or footer
// REPLACES the corresponding built-in area; an omitted one is synthesized from the
// status projection; body/panel/content and the four view leaves are always built-in.
// It is a PURE function -- the runtime calls it to produce the schema it publishes;
// nothing about geometry policy or presence is decided here (geometry EXTENTS come from
// the caller's StyleDimensions, the single configurable source the grid path also uses).
//
// header/footer subtrees use the shared canonical region shape (ChromeRegionShape), so a
// built-in and a composed region are indistinguishable in shape to a consumer. A built-in
// field is a provider-backed Field keyed by the status field id (carrying its collapse
// rank), resolved by the same ChromeProviderResolver the composed path uses. The footer's
// help hint becomes a right-group Field carrying its click command.
//
// The override is a ValidatedComposition (not a raw UiComposition), so only a
// decoder-validated tree can reach the assembly -- a malformed override is
// unrepresentable here, not silently copied into the result.

#include <ssg/ChromeDecode.h>  // ValidatedComposition
#include <ssg/ShellState.h>    // StatusField, ShellFooterHint
#include <ssg/Style.h>         // StyleDimensions
#include <ssg/UiTree.h>        // UiComposition

#include <optional>
#include <vector>

namespace ssg {

// CONTRACT
// assembleWholeScreen: the built-in footer synthesis deliberately carries status
//   FIELDS (with collapse rank) and the help hint, but NOT status actions. A footer
//   status action dispatches through StatusActionInvocation (by status/action id), a
//   channel the semantic tree has no widget for; representing it must be a typed
//   invocation target, never a commandId reinterpretation. This omission is
//   intentional for the staging step and is NOT a license to activate: the runtime
//   must not publish this assembled tree until that typed status-action affordance
//   (and its oracle) exist, or a published built-in footer would silently lose the
//   actions the grid still shows.
[[nodiscard]] UiComposition assembleWholeScreen(
    const std::vector<StatusField>& headerFields,
    const std::vector<StatusField>& footerFields,
    const std::optional<ShellFooterHint>& footerHint,
    const StyleDimensions& dimensions,
    const std::optional<ValidatedComposition>& composedOverride);

}  // namespace ssg
