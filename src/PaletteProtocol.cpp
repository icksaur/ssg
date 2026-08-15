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
         {"parameters", encodeParameters(palette.parameters)}});
}

std::optional<PaletteViewState> decodePalette(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const ProtocolValue* modeField = value.field("mode");
    const ProtocolValue* candidatesField = value.field("candidates");
    const ProtocolValue* parametersField = value.field("parameters");
    if (!modeField || !candidatesField || !candidatesField->asArray() ||
        !parametersField) {
        return std::nullopt;
    }
    auto mode = decodeMode(*modeField);
    auto parameters = decodeParameters(*parametersField);
    if (!mode || !parameters) return std::nullopt;

    PaletteViewState palette;
    palette.mode = *mode;
    palette.parameters = *parameters;
    for (const auto& candidateValue : *candidatesField->asArray()) {
        auto candidate = decodeCandidate(candidateValue);
        if (!candidate) return std::nullopt;
        palette.candidates.push_back(std::move(*candidate));
    }
    return palette;
}

}  // namespace ssg
