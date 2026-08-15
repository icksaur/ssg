#pragma once

// The authoritative-data contract for the opaque View surfaces: every ViewSurface
// names the snapshot section(s) that back it. The library owns each surface's
// placement in the tree AND its data on these sections; a client renders the
// section's data in its medium. This mapping makes the closed surface vocabulary
// carry an enforced data-channel contract -- a surface with no backing section is
// a compile error (the mapping switch is exhaustive, no default), and the mapping
// is total by construction.

#include <ssg/Widget.h>  // ViewSurface

#include <span>

namespace ssg {

// The authoritative snapshot sections a client reads to render a surface. These
// mirror SessionSnapshotSections' members; FileTree and GitStatus share the one
// `tree` section, distinguished by its active provider.
enum class SnapshotSection : std::uint8_t { Document, Tabs, Tree, Palette };

// The section(s) backing a surface, never empty. Total over ViewSurface: the
// switch has no default, so adding a surface without a backing fails to compile.
[[nodiscard]] std::span<const SnapshotSection> viewSurfaceBackingSections(
    ViewSurface surface);

}  // namespace ssg
