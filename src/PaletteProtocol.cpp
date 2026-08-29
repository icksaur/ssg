#include <ssg/PaletteProtocol.h>

#include <array>
#include <limits>
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
    if (!value.asObject()) return std::nullopt;
    const ProtocolValue* id = value.field("id");
    const ProtocolValue* label = value.field("label");
    const ProtocolValue* detail = value.field("detail");
    if (!id || !id->asText()) return std::nullopt;
    if (!label || !label->asText()) return std::nullopt;
    if (!detail || !detail->asText()) return std::nullopt;
    // A candidate scored by the matcher must be within the proven-safe byte length, or
    // the score's exactness (and cross-client parity) is not guaranteed. An oversized
    // candidate is rejected, never truncated -- the match contract is a full-byte
    // subsequence over the whole id/label.
    const std::size_t cap = static_cast<std::size_t>(kMaxCandidateBytes);
    if (id->asText()->size() > cap || label->asText()->size() > cap)
        return std::nullopt;
    return PaletteCandidate{*id->asText(), *label->asText(), *detail->asText()};
}

ProtocolValue encodeParameters(const MatcherParameters& params) {
    return ProtocolValue::makeObject(
        {{"base_score", ProtocolValue::makeInt(params.baseScore)},
         {"word_boundary_bonus", ProtocolValue::makeInt(params.wordBoundaryBonus)},
         {"contiguity_bonus", ProtocolValue::makeInt(params.contiguityBonus)},
         {"exact_case_bonus", ProtocolValue::makeInt(params.exactCaseBonus)},
         {"length_cap", ProtocolValue::makeInt(params.lengthCap)}});
}

std::optional<int> intField(const ProtocolValue& object, const char* key) {
    const ProtocolValue* field = object.field(key);
    if (!field || !field->asInt()) return std::nullopt;
    std::int64_t raw = *field->asInt();
    if (raw < std::numeric_limits<int>::min() ||
        raw > std::numeric_limits<int>::max()) {
        return std::nullopt;  // out of int range is malformed, not silently narrowed
    }
    return static_cast<int>(raw);
}

std::optional<MatcherParameters> decodeParameters(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    auto base = intField(value, "base_score");
    auto word = intField(value, "word_boundary_bonus");
    auto contiguity = intField(value, "contiguity_bonus");
    auto exact = intField(value, "exact_case_bonus");
    auto cap = intField(value, "length_cap");
    if (!base || !word || !contiguity || !exact || !cap) return std::nullopt;
    MatcherParameters params{*base, *word, *contiguity, *exact, *cap};
    if (!matcherParametersInDomain(params)) return std::nullopt;
    return params;
}

std::optional<SearchMode> decodeMode(const ProtocolValue& value) {
    // Wire form mirrors the shared enum codec: the SearchMode underlying value,
    // validated against Search.h's single closed domain (kAllSearchModes).
    auto raw = value.asUint();
    if (!raw) return std::nullopt;
    for (SearchMode mode : kAllSearchModes) {
        if (static_cast<std::uint64_t>(static_cast<std::uint8_t>(mode)) == *raw)
            return mode;
    }
    return std::nullopt;
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
    const ProtocolValue* generation = value.field("generation");
    const ProtocolValue* ops = value.field("ops");
    if (!value.asObject() || !generation || !generation->asUint() || !ops ||
        !ops->asArray()) {
        return std::nullopt;
    }
    PalettePresenceOverlay overlay;
    overlay.generation = Generation{*generation->asUint()};
    std::set<std::string> targets;
    for (const ProtocolValue& encoded : *ops->asArray()) {
        const ProtocolValue* kind = encoded.field("kind");
        const ProtocolValue* target = encoded.field("target");
        if (!encoded.asObject() || !kind || !kind->asUint() || !target ||
            !target->asText() || target->asText()->empty() ||
            !targets.insert(*target->asText()).second) {
            return std::nullopt;
        }
        PalettePresenceOpKind decodedKind;
        switch (*kind->asUint()) {
        case static_cast<std::uint8_t>(PalettePresenceOpKind::Show):
            decodedKind = PalettePresenceOpKind::Show;
            break;
        case static_cast<std::uint8_t>(PalettePresenceOpKind::Hide):
            decodedKind = PalettePresenceOpKind::Hide;
            break;
        default:
            return std::nullopt;
        }
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
    if (!value.asObject()) return std::nullopt;
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
    if (!activeModeField || !activationIdField ||
        (!activeModeField->asUint() &&
         activeModeField->kind() != ProtocolValue::Kind::NullValue) ||
        (!activationIdField->asUint() &&
         activationIdField->kind() != ProtocolValue::Kind::NullValue) ||
        !commandOpenCommandIdField || !commandOpenCommandIdField->asText() ||
        !commandCandidatesField || !commandCandidatesField->asArray() ||
        !fileOpenCommandIdField || !fileOpenCommandIdField->asText() ||
        !fileCandidatesField || !fileCandidatesField->asArray() ||
        !parametersField || !presenceOverlayField || !magnitudeField ||
        !magnitudeField->asInt() ||
        !candidateBytesField || !candidateBytesField->asInt()) {
        return std::nullopt;
    }
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
    if (palette.commandOpenCommandId.empty() ||
        palette.fileOpenCommandId.empty() ||
        palette.commandOpenCommandId == palette.fileOpenCommandId) {
        return std::nullopt;
    }
    const bool hasMode =
        activeModeField->kind() != ProtocolValue::Kind::NullValue;
    const bool hasActivation =
        activationIdField->kind() != ProtocolValue::Kind::NullValue;
    if (hasMode != hasActivation) return std::nullopt;
    if (hasMode) {
        auto activeMode = decodeMode(*activeModeField);
        auto const activationValue = activationIdField->asUint();
        if (!activeMode ||
            (*activeMode != SearchMode::Command &&
             *activeMode != SearchMode::File) ||
            !activationValue || *activationValue == 0) {
            return std::nullopt;
        }
        palette.activePicker = PickerActivation{
            *activeMode, PickerActivationId{*activationValue}};
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
