#pragma once

// The authoritative-data contract for the opaque tree nodes -- the View surfaces AND
// the StatusActions widget: each names the snapshot section(s) that back it. The
// library owns each node's placement in the tree AND its data on these sections; a
// client renders the section's data in its medium. This mapping makes the closed
// opaque-node vocabulary carry an ENFORCED data-channel contract -- a node with no
// backing section is a compile error (each mapping switch is exhaustive, no default),
// and the mapping is total by construction.

#include <ssg/Widget.h>  // ViewSurface

#include <span>

namespace ssg {

// The authoritative snapshot sections a client reads to render an opaque node. These
// mirror SessionSnapshotSections' members; FileTree and GitStatus share the one
// `tree` section, distinguished by its active provider. PromptStatus backs the
// StatusActions widget (the selected status item, its actions, and generation).
enum class SnapshotSection : std::uint8_t {
    Document,
    Tabs,
    Tree,
    Palette,
    PromptStatus,
};

// The section(s) backing a surface, never empty. Total over ViewSurface: the
// switch has no default, so adding a surface without a backing fails to compile.
[[nodiscard]] std::span<const SnapshotSection> viewSurfaceBackingSections(
    ViewSurface surface);

// The section backing the StatusActions widget: the promptStatus section, which
// publishes the selected status item, its actions, and generation atomically. A
// client renders the actions from it and dispatches StatusActionInvocation. Encoded
// as a value (not prose) so the backing cannot drift silently.
[[nodiscard]] SnapshotSection statusActionsBackingSection();

}  // namespace ssg
