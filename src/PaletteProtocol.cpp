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

}  // namespace

ProtocolValue encodePalette(const PaletteViewState& palette) {
    ProtocolValue::Array candidates;
    candidates.reserve(palette.candidates.size());
    for (const auto& candidate : palette.candidates)
        candidates.push_back(encodeCandidate(candidate));
    return ProtocolValue::makeObject(
        {{"mode", ProtocolValue::makeUint(
                      static_cast<std::uint8_t>(palette.mode))},
         {"candidates", ProtocolValue::makeArray(std::move(candidates))},
         {"parameters", encodeParameters(palette.parameters)},
         {"picker_epoch", ProtocolValue::makeUint(palette.pickerEpoch)},
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
    const ProtocolValue* modeField = value.field("mode");
    const ProtocolValue* candidatesField = value.field("candidates");
    const ProtocolValue* parametersField = value.field("parameters");
    const ProtocolValue* magnitudeField = value.field("max_parameter_magnitude");
    const ProtocolValue* candidateBytesField = value.field("max_candidate_bytes");
    const ProtocolValue* pickerEpochField = value.field("picker_epoch");
    if (!modeField || !candidatesField || !candidatesField->asArray() ||
        !parametersField || !magnitudeField || !magnitudeField->asInt() ||
        !candidateBytesField || !candidateBytesField->asInt() ||
        (pickerEpochField && !pickerEpochField->asUint())) {
        return std::nullopt;
    }
    // The published bounds must equal the library's own -- a frame claiming a different
    // domain is a mismatch the C++ authority rejects (the C++ side scores with the
    // compiled constants, so it must not admit a frame stamped with other bounds).
    if (*magnitudeField->asInt() != kMaxMatcherParameterMagnitude) return std::nullopt;
    if (*candidateBytesField->asInt() != kMaxCandidateBytes) return std::nullopt;
    auto mode = decodeMode(*modeField);
    auto parameters = decodeParameters(*parametersField);
    if (!mode || !parameters) return std::nullopt;

    PaletteViewState palette;
    palette.mode = *mode;
    palette.parameters = *parameters;
    palette.pickerEpoch =
        pickerEpochField ? *pickerEpochField->asUint() : std::uint64_t{0};
    for (const auto& candidateValue : *candidatesField->asArray()) {
        auto candidate = decodeCandidate(candidateValue);
        if (!candidate) return std::nullopt;
        palette.candidates.push_back(std::move(*candidate));
    }
    return palette;
}

}  // namespace ssg
