#include <ssg/PaletteProtocol.h>
#include <ssg/detail/generated/wire_schema.h>

#include <set>
#include <string>

namespace ssg {

namespace {

ProtocolValue encodeCandidate(const PaletteCandidate& candidate) {
    return ProtocolValue::makeObject(
        {{"id", ProtocolValue::makeText(candidate.id)},
         {"label", ProtocolValue::makeText(candidate.label)},
         {"detail", ProtocolValue::makeText(candidate.detail)}});
}

std::optional<PaletteCandidate> decodeCandidate(const ProtocolValue& value) {
    const auto& id = *value.field("id")->asText();
    const auto& label = *value.field("label")->asText();
    const auto& detail = *value.field("detail")->asText();
    // A candidate scored by the matcher must be within the proven-safe byte length, or
    // the score's exactness (and cross-client parity) is not guaranteed. An oversized
    // candidate is rejected, never truncated -- the match contract is a full-byte
    // subsequence over the whole id/label.
    const std::size_t cap = static_cast<std::size_t>(kMaxCandidateBytes);
    if (id.size() > cap || label.size() > cap)
        return std::nullopt;
    return PaletteCandidate{id, label, detail};
}

ProtocolValue encodeParameters(const MatcherParameters& params) {
    return ProtocolValue::makeObject(
        {{"base_score", ProtocolValue::makeInt(params.baseScore)},
         {"word_boundary_bonus", ProtocolValue::makeInt(params.wordBoundaryBonus)},
         {"contiguity_bonus", ProtocolValue::makeInt(params.contiguityBonus)},
         {"exact_case_bonus", ProtocolValue::makeInt(params.exactCaseBonus)},
         {"length_cap", ProtocolValue::makeInt(params.lengthCap)}});
}

std::optional<MatcherParameters> decodeParameters(const ProtocolValue& value) {
    MatcherParameters params{
        static_cast<int>(*value.field("base_score")->asInt()),
        static_cast<int>(*value.field("word_boundary_bonus")->asInt()),
        static_cast<int>(*value.field("contiguity_bonus")->asInt()),
        static_cast<int>(*value.field("exact_case_bonus")->asInt()),
        static_cast<int>(*value.field("length_cap")->asInt())};
    if (!matcherParametersInDomain(params)) return std::nullopt;
    return params;
}

SearchMode decodeMode(const ProtocolValue& value) {
    return static_cast<SearchMode>(*value.asUint());
}

ProtocolValue encodePresenceOverlay(const PalettePresenceOverlay& overlay) {
    ProtocolValue::Array ops;
    ops.reserve(overlay.ops.size());
    for (const PalettePresenceOp& op : overlay.ops) {
        ops.push_back(ProtocolValue::makeObject(
            {{"kind", ProtocolValue::makeUint(
                          static_cast<std::uint8_t>(op.kind))},
             {"target", ProtocolValue::makeText(op.target.value())}}));
    }
    return ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(overlay.generation.value())},
         {"ops", ProtocolValue::makeArray(std::move(ops))}});
}

std::optional<PalettePresenceOverlay> decodePresenceOverlay(
    const ProtocolValue& value) {
    if (!detail::generated::validatePalettePresenceOverlayWire(value)) {
        return std::nullopt;
    }
    const ProtocolValue* generation = value.field("generation");
    const ProtocolValue* ops = value.field("ops");
    PalettePresenceOverlay overlay;
    overlay.generation = Generation{*generation->asUint()};
    std::set<std::string> targets;
    for (const ProtocolValue& encoded : *ops->asArray()) {
        const ProtocolValue* kind = encoded.field("kind");
        const ProtocolValue* target = encoded.field("target");
        if (!targets.insert(*target->asText()).second) {
            return std::nullopt;
        }
        const auto decodedKind =
            static_cast<PalettePresenceOpKind>(*kind->asUint());
        overlay.ops.push_back(
            {decodedKind, UiNodeId{*target->asText()}});
    }
    return overlay;
}

}  // namespace

ProtocolValue encodePalette(const PaletteViewState& palette) {
    const auto encodeCandidates = [](const auto& source) {
        ProtocolValue::Array candidates;
        candidates.reserve(source.size());
        for (const auto& candidate : source) {
            candidates.push_back(encodeCandidate(candidate));
        }
        return ProtocolValue::makeArray(std::move(candidates));
    };
    return ProtocolValue::makeObject(
        {{"active_mode",
          palette.activePicker
              ? ProtocolValue::makeUint(
                    static_cast<std::uint8_t>(palette.activePicker->mode))
              : ProtocolValue::makeNull()},
         {"activation_id",
          palette.activePicker
              ? ProtocolValue::makeUint(palette.activePicker->id.value())
              : ProtocolValue::makeNull()},
         {"command_candidates", encodeCandidates(palette.commandCandidates)},
         {"command_open_command_id",
          ProtocolValue::makeText(palette.commandOpenCommandId)},
         {"file_candidates", encodeCandidates(palette.fileCandidates)},
         {"file_open_command_id",
          ProtocolValue::makeText(palette.fileOpenCommandId)},
         {"presence_overlay",
          encodePresenceOverlay(palette.presenceOverlay)},
         {"parameters", encodeParameters(palette.parameters)},
         // The parameter magnitude domain AND the candidate byte-length bound,
         // published from the library-owned constants so a non-C++ client validates
         // against the SAME bounds the C++ decoder enforces (and can prove the score
         // stays exact) rather than hardcoding its own copies.
         {"max_parameter_magnitude",
          ProtocolValue::makeInt(kMaxMatcherParameterMagnitude)},
         {"max_candidate_bytes", ProtocolValue::makeInt(kMaxCandidateBytes)}});
}

std::optional<PaletteViewState> decodePalette(const ProtocolValue& value) {
    if (!detail::generated::validatePaletteViewStateWire(value)) {
        return std::nullopt;
    }
    const ProtocolValue* activeModeField = value.field("active_mode");
    const ProtocolValue* activationIdField = value.field("activation_id");
    const ProtocolValue* commandCandidatesField =
        value.field("command_candidates");
    const ProtocolValue* commandOpenCommandIdField =
        value.field("command_open_command_id");
    const ProtocolValue* fileCandidatesField = value.field("file_candidates");
    const ProtocolValue* fileOpenCommandIdField =
        value.field("file_open_command_id");
    const ProtocolValue* parametersField = value.field("parameters");
    const ProtocolValue* presenceOverlayField =
        value.field("presence_overlay");
    const ProtocolValue* magnitudeField = value.field("max_parameter_magnitude");
    const ProtocolValue* candidateBytesField = value.field("max_candidate_bytes");
    // The published bounds must equal the library's own -- a frame claiming a different
    // domain is a mismatch the C++ authority rejects (the C++ side scores with the
    // compiled constants, so it must not admit a frame stamped with other bounds).
    if (*magnitudeField->asInt() != kMaxMatcherParameterMagnitude) return std::nullopt;
    if (*candidateBytesField->asInt() != kMaxCandidateBytes) return std::nullopt;
    auto parameters = decodeParameters(*parametersField);
    auto presenceOverlay = decodePresenceOverlay(*presenceOverlayField);
    if (!parameters || !presenceOverlay) return std::nullopt;

    PaletteViewState palette;
    palette.commandOpenCommandId = *commandOpenCommandIdField->asText();
    palette.fileOpenCommandId = *fileOpenCommandIdField->asText();
    if (palette.commandOpenCommandId == palette.fileOpenCommandId) {
        return std::nullopt;
    }
    const bool hasMode =
        activeModeField->kind() != ProtocolValue::Kind::NullValue;
    const bool hasActivation =
        activationIdField->kind() != ProtocolValue::Kind::NullValue;
    if (hasMode != hasActivation) return std::nullopt;
    if (hasMode) {
        const auto activeMode = decodeMode(*activeModeField);
        const auto activationValue = *activationIdField->asUint();
        if ((activeMode != SearchMode::Command &&
             activeMode != SearchMode::File) ||
            activationValue == 0) {
            return std::nullopt;
        }
        palette.activePicker = PickerActivation{
            activeMode, PickerActivationId{activationValue}};
    }
    palette.parameters = *parameters;
    palette.presenceOverlay = std::move(*presenceOverlay);
    const auto decodeCandidates = [](const ProtocolValue& field,
                                     auto& destination) {
        for (const auto& candidateValue : *field.asArray()) {
            auto candidate = decodeCandidate(candidateValue);
            if (!candidate) return false;
            destination.push_back(std::move(*candidate));
        }
        return true;
    };
    if (!decodeCandidates(*commandCandidatesField, palette.commandCandidates) ||
        !decodeCandidates(*fileCandidatesField, palette.fileCandidates)) {
        return std::nullopt;
    }
    return palette;
}

}  // namespace ssg
