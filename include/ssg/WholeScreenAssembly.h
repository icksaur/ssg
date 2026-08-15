#pragma once

// The whole-screen tree assembly: from the built-in status-field projection AND an
// optional ssg.chrome composition, build the canonical whole-screen UiComposition
//
//   root
//   ├─ header            (composed override, else built-in from headerFields)
//   ├─ body   Row Flex
//   │  ├─ panel   Col Exact(kPanelWidth)   [ filetree(view), gitstatus(view) ]
//   │  └─ content Col Flex                 [ tabview(view),  findresults(view) ]
//   └─ footer            (composed override, else built-in from footerFields)
//
// This function OWNS the fallback/override rule: a ssg.chrome-composed header or
// footer REPLACES the corresponding built-in area; an omitted one is synthesized from
// the status fields. body/panel/content and the four view leaves are always built-in.
// It is a PURE function -- the runtime calls it to produce the schema it publishes,
// but nothing about geometry or presence is decided here.
//
// The header/footer subtrees use the shared canonical region shape (ChromeRegionShape),
// so a built-in and a composed region are indistinguishable in shape to a consumer.
// A built-in region's leaves are provider-backed Fields keyed by the status field id,
// resolved by the same ChromeProviderResolver the composed path uses.

#include <ssg/ShellState.h>  // StatusField
#include <ssg/UiTree.h>      // UiComposition

#include <optional>
#include <vector>

namespace ssg {

// The abstract-unit extents the whole-screen tree reserves. Named here (not literals
// in the spec); a grid client maps a unit to a cell, the web to a ch/row. The panel
// width is the successor to today's sidebar column count; the header/footer are one
// row tall.
inline constexpr int kPanelWidth = 24;
inline constexpr int kHeaderRows = 1;
inline constexpr int kFooterRows = 1;

[[nodiscard]] UiComposition assembleWholeScreen(
    const std::vector<StatusField>& headerFields,
    const std::vector<StatusField>& footerFields,
    const std::optional<UiComposition>& composedOverride);

}  // namespace ssg
