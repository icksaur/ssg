#pragma once

// The whole-screen tree assembly: from the built-in status-field catalog, build
// the canonical whole-screen UiComposition.
//
//   root
//   ├─ header            (built-in status fields and picker input)
//   ├─ body   Row Flex
//   │  ├─ panel   Col OptionalPreferred(panel target/minimum) [ filetree, gitstatus ]
//   │  └─ content Col MinimumFlex(editor minimum) [ tabbar, notice, externalmod,
//   │                        editor>document.viewport>document,
//   │                        findresults.viewport>findresults ]
//   └─ footer            (built-in status fields, hint, and actions)
//
// This pure function owns the built-in whole-screen structure it publishes. Nothing
// about geometry policy or presence is decided here (geometry EXTENTS come from the
// caller's StyleDimensions, the single configurable source the grid path also uses).
//
// The tree is STRUCTURALLY STABLE: it is built from the stable status-field CATALOG (accepted as
// StatusFieldCatalogEntry, so a dynamic projected subset is not even representable), a
// built-in field carries only its id and collapse rank, and its value/label/command all
// ride uiState (resolved per frame by id); the footer hint is likewise a provider-backed
// Field whose label rides uiState. So a field's value, command, or provider PRESENCE
// changing (a branch appearing/disappearing) is value-state, never a structure change,
// and never advances the schema generation. The catalog is split into header/footer by
// each entry's own region.
//
#include <ssg/StatusFields.h>   // StatusFieldCatalogEntry
#include <ssg/Style.h>          // StyleDimensions
#include <ssg/PromptSurface.h>  // PromptRequest
#include <ssg/StatusQueue.h>
#include <ssg/UiTree.h>         // UiComposition

#include <optional>
#include <string_view>
#include <vector>

namespace ssg {

// CONTRACT
// assembleWholeScreen: the prompt query line is a built-in TextInput leaf the library
//   always places right after the header's left (status-fields) group, so tree order
//   matches the visual order (a tree-order client renders it after the fields, not
//   past the flex middle); it is presence-gated (visible only for a header-region
//   prompt) rather than added or removed, so the schema stays generation-stable. This
//   is the only state-free TextInput the tree carries: footer prompt inputs are
//   request-derived, provider-backed leaves.
[[nodiscard]] UiComposition assembleWholeScreen(
    const std::vector<StatusFieldCatalogEntry>& catalog,
    std::string_view hintCommandId,
    const StyleDimensions& dimensions,
    std::string_view promptSigil);

[[nodiscard]] UiComposition withFooterPrompt(UiComposition base,
                                             const PromptSurface& prompt);
[[nodiscard]] UiComposition withStatusActions(
    UiComposition base, const std::vector<StatusActionNode>& actions);

// Build the authoritative footer.prompt subtree for the active footer request.
// This is the single source consumed by whole-screen schema overlay and grid
// prompt layout; client code must not regroup controls by PromptKind.
[[nodiscard]] UiNode assembleFooterPrompt(const PromptSurface& prompt);

}  // namespace ssg
