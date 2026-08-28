#pragma once

// The whole-screen tree assembly: from the built-in status projection AND an optional
// ssg.chrome composition, build the canonical whole-screen UiComposition
//
//   root
//   ├─ header            (composed override, else built-in from header)
//   ├─ body   Row Flex
//   │  ├─ panel   Col Exact(dimensions.panelTargetWidth) [ filetree, gitstatus ]
//   │  └─ content Col Flex [ editor>[tabbar, document.viewport>document],
//   │                        findresults.viewport>findresults ]
//   └─ footer            (composed override, else built-in from footer + hint)
//
// This function OWNS the fallback/override rule: a ssg.chrome-composed header or footer
// REPLACES the corresponding built-in area; an omitted one is synthesized from the
// STABLE status-field catalog superset; body/panel/content and the four view leaves are
// always built-in. It is a PURE function -- the runtime calls it to produce the schema
// it publishes; nothing about geometry policy or presence is decided here (geometry
// EXTENTS come from the caller's StyleDimensions, the single configurable source the
// grid path also uses).
//
// header/footer subtrees use the shared canonical region shape (ChromeRegionShape), so a
// built-in and a composed region are indistinguishable in shape to a consumer. The tree
// is STRUCTURALLY STABLE: it is built from the stable status-field CATALOG (accepted as
// StatusFieldCatalogEntry, so a dynamic projected subset is not even representable), a
// built-in field carries only its id and collapse rank, and its value/label/command all
// ride uiState (resolved per frame by id); the footer hint is likewise a provider-backed
// Field whose label rides uiState. So a field's value, command, or provider PRESENCE
// changing (a branch appearing/disappearing) is value-state, never a structure change,
// and never advances the schema generation. The catalog is split into header/footer by
// each entry's own region.
//
// The override is a ValidatedComposition (not a raw UiComposition), so only a
// decoder-validated tree can reach the assembly -- a malformed override is
// unrepresentable here, not silently copied into the result.

#include <ssg/ChromeDecode.h>   // ValidatedComposition
#include <ssg/StatusFields.h>   // StatusFieldCatalogEntry
#include <ssg/Style.h>          // StyleDimensions
#include <ssg/PromptSurface.h>  // PromptRequest
#include <ssg/UiTree.h>         // UiComposition

#include <optional>
#include <string_view>
#include <vector>

namespace ssg {

// CONTRACT
// assembleWholeScreen: the built-in footer's status-actions affordance is a stable
//   StatusActions widget whose DATA rides the section statusActionsBackingSection()
//   names (promptStatus: the selected status item's actions, status id, and
//   generation), never the schema -- the schema is generation-stable while the actions
//   vary on that section's cadence. A client renders the actions from that section and
//   dispatches the existing StatusActionInvocation (by status/action id + generation);
//   it never reinterprets an action as a commandId click. A composed ssg.chrome footer
//   replaces the whole built-in footer and so omits the affordance, matching the grid.
// assembleWholeScreen: the prompt query line is a built-in TextInput leaf the library
//   always places right after the header's left (status-fields) group, so tree order
//   matches the visual order (a tree-order client renders it after the fields, not
//   past the flex middle); it is presence-gated (visible only for a header-region
//   prompt) rather than added or removed, so the schema stays generation-stable. This
//   is the only state-free TextInput the tree carries: footer prompt inputs are
//   request-derived, provider-backed leaves. An ssg.chrome author cannot contribute
//   either form (the decoder refuses text_input).
[[nodiscard]] UiComposition assembleWholeScreen(
    const std::vector<StatusFieldCatalogEntry>& catalog,
    std::string_view hintCommandId,
    const StyleDimensions& dimensions,
    std::string_view promptSigil,
    const std::optional<ValidatedComposition>& composedOverride);

[[nodiscard]] UiComposition withFooterPrompt(UiComposition base,
                                             const PromptSurface& prompt);

// Build the authoritative footer.prompt subtree for the active footer request.
// This is the single source consumed by whole-screen schema overlay and grid
// prompt layout; client code must not regroup controls by PromptKind.
[[nodiscard]] UiNode assembleFooterPrompt(const PromptSurface& prompt);

}  // namespace ssg
