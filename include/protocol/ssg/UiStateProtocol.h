#pragma once

// Wire codec for the dynamic node state: UiStateSection <-> ProtocolValue.
//
// Kept separate from the large Protocol.cpp codec and written against
// ProtocolValue's PUBLIC api alone, so the state encoding is a small, standalone,
// round-trippable unit. The snapshot-sections codec calls these to publish the
// `ui_state` section alongside the `ui` schema.
//
// encodeUiState/decodeUiState round-trip exactly. decode returns nullopt on
// malformed input (no generation, a duplicate node id, a malformed leaf field
// type, or a present malformed focus path), never a partial. An absent/null focus
// path is the compatibility representation of a frame from an older host. The
// schema-relative checks a UiStateSection cannot carry on its own (node-id set
// matching the schema, container/leaf shape agreement, focus-path membership) are
// a client's reconciliation concern, not this codec's.

#include <ssg/Protocol.h>
#include <ssg/UiNodeState.h>

#include <optional>

namespace ssg {

[[nodiscard]] ProtocolValue encodeUiState(const UiStateSection& section);
[[nodiscard]] std::optional<UiStateSection> decodeUiState(
    const ProtocolValue& value);

}  // namespace ssg
