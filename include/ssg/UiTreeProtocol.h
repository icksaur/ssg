#pragma once

// Wire codec for the UI-VM tree schema: UiSchema <-> ProtocolValue.
//
// Kept separate from the large Protocol.cpp codec and written against
// ProtocolValue's PUBLIC api alone (make*/as*/field), so the tree encoding is a
// small, standalone, round-trippable unit. The snapshot-sections codec calls
// these to publish the medium-agnostic `ui` section alongside the legacy path.
//
// encodeUiSchema/decodeUiSchema round-trip exactly: decodeUiSchema(encodeUiSchema(s))
// == s for any schema built by the bridge. decode returns nullopt on malformed
// input, never a partial.

#include <ssg/Protocol.h>
#include <ssg/UiTree.h>

#include <optional>

namespace ssg {

[[nodiscard]] ProtocolValue encodeUiSchema(const UiSchema& schema);
// CONTRACT: While the frozen legacy presentation wire path exists, decoding
// normalizes its exact preceding canonical whole-screen topology to the current
// topology. Other noncanonical arrangements remain malformed.
[[nodiscard]] std::optional<UiSchema> decodeUiSchema(const ProtocolValue& value);

}  // namespace ssg
