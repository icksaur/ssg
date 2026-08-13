#pragma once

// Build the medium-agnostic UI-VM tree from the legacy ChromeComposition.
//
// A header/footer row becomes a Row container with three child group containers:
// a LEFT group (its `gap` carries the row separator), a CENTER group (zero or one
// leaf, whose Size carries the center width policy: Flex, or Exact for a fixed
// center), and a RIGHT group. The arrangement is expressed entirely through
// generic structure -- containers, gaps, and node sizes -- with NO grid sidecar,
// so the published tree is self-describing: a native client renders it from the
// tree alone, and the grid client lowers the same tree through lowerUiChromeRegion.
//
// Per-widget arrangement hints (rank, keep, overflow, sigil) already live on the
// WidgetDescriptor leaf, so they need no separate carrier.

#include <ssg/ChromeComposition.h>  // ChromeComposition, RowDescriptor
#include <ssg/RegionRoot.h>        // RegionRole
#include <ssg/UiTree.h>            // UiRegion, UiSchema, Generation

namespace ssg {

// Build one region (Top for a header, Bottom for a footer) from a row.
[[nodiscard]] UiRegion uiChromeRegionFromRow(const RowDescriptor& row,
                                             RegionRole role);

// Build the whole schema from a composition: header -> Top, footer -> Bottom.
// An absent region is simply not present in the schema.
[[nodiscard]] UiSchema uiSchemaFromChrome(const ChromeComposition& chrome,
                                          Generation generation);

}  // namespace ssg
