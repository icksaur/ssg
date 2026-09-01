#pragma once

// Wire codec for the palette section: PaletteViewState <-> ProtocolValue.
//
// Kept separate from the large Protocol.cpp codec and written against
// ProtocolValue's PUBLIC api alone, so the palette encoding -- the finder's full
// candidate universe AND the matcher parameters that score it -- is a small,
// standalone, round-trippable unit, mirroring UiStateProtocol and PresenceProtocol.
//
// encodePalette/decodePalette round-trip exactly. decode returns nullopt on
// malformed input (a candidate missing id/label/detail, or a MatcherParameters
// missing any weight/cap field), never a partial. Publishing the parameters ON this
// channel is the contract that a client can never score candidates without the
// parameters that score them -- they arrive atomically with the candidate universe.

#include <ssg/PaletteSearcher.h>
#include <ssg/Protocol.h>

#include <optional>

namespace ssg {

[[nodiscard]] ProtocolValue encodePalette(const PaletteViewState& palette);
[[nodiscard]] std::optional<PaletteViewState> decodePalette(
    const ProtocolValue& value);

}  // namespace ssg
