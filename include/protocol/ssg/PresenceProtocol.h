#pragma once

// Wire codec for the presence section: UiPresenceSection <-> ProtocolValue.
//
// Kept separate from the large Protocol.cpp codec and written against
// ProtocolValue's PUBLIC api alone, so the presence encoding is a small,
// standalone, round-trippable unit, mirroring UiStateProtocol.
//
// encodeUiPresence/decodeUiPresence round-trip exactly. decode returns nullopt on
// malformed input (no generation/basis, a record without an id or a boolean
// present, or a duplicate node id), never a partial. The schema-relative check
// (the record id-set equals the schema's node-id set at the same generation) is
// validateUiPresenceAgainstSchema, applied where the schema is available.

#include <ssg/Protocol.h>
#include <ssg/UiPresence.h>

#include <optional>

namespace ssg {

[[nodiscard]] ProtocolValue encodeUiPresence(const UiPresenceSection& section);
[[nodiscard]] std::optional<UiPresenceSection> decodeUiPresence(
    const ProtocolValue& value);

// True iff `section` corresponds to `schema`: same generation, and its record
// id-set is exactly the schema's node-id set (one record per node, no missing,
// duplicate, or foreign records). A client checks this before laying out a frame,
// so a presence section from a different generation or a drifted tree is refused
// rather than applied to the wrong schema.
[[nodiscard]] bool uiPresenceCorrespondsToSchema(const UiPresenceSection& section,
                                                 const ValidatedSchema& schema);

}  // namespace ssg
