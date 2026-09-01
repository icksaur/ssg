#include <ssg/ChromeLowering.h>
#include <ssg/Protocol.h>
#include <ssg/UiStateResolver.h>
#include <ssg/detail/generated/semantic_wire_manifest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ssg::detail::generated::ManifestLifecycle;
using ssg::detail::generated::ReplayPolicy;
using ssg::detail::generated::SpecializedDecision;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kWarmupSamples = 256;
constexpr std::size_t kMeasuredSamples = 2048;
constexpr std::size_t kIndependentRuns = 5;
constexpr std::size_t kRequiredLatencyAgreements = 4;
constexpr double kMaximumGenericDeltaPayloadRatio = 2.0;
constexpr double kMaximumGenericReplayLatencyRatio = 2.0;
constexpr double kLatencyConfidenceMargin = 0.10;
volatile std::uintptr_t benchmarkSink{};

void consume(const void* address) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    benchmarkSink = benchmarkSink ^
                    reinterpret_cast<std::uintptr_t>(address);
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

struct Span {
    std::size_t offset{};
    std::size_t size{};
};

class WireWalker {
public:
    explicit WireWalker(std::string_view bytes) : bytes_{bytes} {}

    [[nodiscard]] std::size_t skip(std::size_t offset,
                                   std::size_t depth = 0) const {
        if (depth > ssg::ProtocolLimits{}.maxValueDepth) fail("depth");
        auto cursor = offset;
        auto const kind = byte(cursor++);
        switch (kind) {
            case 0:
                break;
            case 1:
                require(cursor, 1);
                ++cursor;
                break;
            case 2:
            case 3:
                require(cursor, 8);
                cursor += 8;
                break;
            case 4:
            case 5: {
                auto const length = u32(cursor);
                require(cursor, length);
                cursor += length;
                break;
            }
            case 6: {
                auto const count = u32(cursor);
                for (std::uint32_t index = 0; index < count; ++index)
                    cursor = skip(cursor, depth + 1);
                break;
            }
            case 7: {
                auto const count = u32(cursor);
                std::set<std::string> keys;
                for (std::uint32_t index = 0; index < count; ++index) {
                    auto const length = u32(cursor);
                    require(cursor, length);
                    std::string key{bytes_.substr(cursor, length)};
                    cursor += length;
                    if (!keys.insert(std::move(key)).second) fail("duplicate key");
                    cursor = skip(cursor, depth + 1);
                }
                break;
            }
            default:
                fail("kind");
        }
        return cursor;
    }

    [[nodiscard]] std::map<std::string, Span> object(
        std::size_t offset) const {
        auto cursor = offset;
        if (byte(cursor++) != 7) fail("expected object");
        auto const count = u32(cursor);
        std::map<std::string, Span> fields;
        for (std::uint32_t index = 0; index < count; ++index) {
            auto const length = u32(cursor);
            require(cursor, length);
            std::string key{bytes_.substr(cursor, length)};
            cursor += length;
            auto const begin = cursor;
            cursor = skip(cursor, 1);
            if (!fields.emplace(std::move(key),
                                Span{begin, cursor - begin}).second) {
                fail("duplicate key");
            }
        }
        return fields;
    }

private:
    [[noreturn]] static void fail(std::string_view reason) {
        throw std::runtime_error{"wire walker rejected " + std::string{reason}};
    }

    void require(std::size_t offset, std::size_t count) const {
        if (offset > bytes_.size() || count > bytes_.size() - offset)
            fail("truncated value");
    }

    [[nodiscard]] std::uint8_t byte(std::size_t offset) const {
        require(offset, 1);
        return static_cast<std::uint8_t>(bytes_[offset]);
    }

    [[nodiscard]] std::uint32_t u32(std::size_t& cursor) const {
        require(cursor, 4);
        std::uint32_t value{};
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(
                         static_cast<std::uint8_t>(bytes_[cursor++]))
                     << shift;
        return value;
    }

    std::string_view bytes_;
};

void appendU32(std::string& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<char>((value >> shift) & 0xffU));
}

std::string scalar(std::uint8_t kind, std::size_t payload = 0) {
    std::string bytes(1, static_cast<char>(kind));
    bytes.append(payload, '\0');
    return bytes;
}

void verifyWireWalker() {
    std::vector<std::string> values{
        scalar(0), scalar(1, 1), scalar(2, 8), scalar(3, 8)};
    for (auto kind : {std::uint8_t{4}, std::uint8_t{5}}) {
        std::string value(1, static_cast<char>(kind));
        appendU32(value, 3);
        value += std::string{"a\0b", 3};
        values.push_back(std::move(value));
    }
    std::string emptyArray(1, '\x06');
    appendU32(emptyArray, 0);
    values.push_back(emptyArray);
    std::string nestedArray(1, '\x06');
    appendU32(nestedArray, 2);
    nestedArray += scalar(1, 1);
    nestedArray += emptyArray;
    values.push_back(nestedArray);
    std::string emptyObject(1, '\x07');
    appendU32(emptyObject, 0);
    values.push_back(emptyObject);
    std::string nestedObject(1, '\x07');
    appendU32(nestedObject, 1);
    appendU32(nestedObject, 1);
    nestedObject += 'x';
    nestedObject += nestedArray;
    values.push_back(nestedObject);
    for (auto const& value : values) {
        if (WireWalker{value}.skip(0) != value.size())
            throw std::runtime_error{"wire walker did not consume a hand case"};
    }
    std::string truncated(1, '\x04');
    appendU32(truncated, 1);
    try {
        (void)WireWalker{truncated}.skip(0);
        throw std::runtime_error{"wire walker accepted a truncated value"};
    } catch (std::runtime_error const& error) {
        if (std::string_view{error.what()}.find("truncated") ==
            std::string_view::npos)
            throw;
    }
}

std::string readFile(std::string const& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error{"cannot read " + path};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

std::string readHex(std::string const& path) {
    auto const text = readFile(path);
    std::string bytes;
    int high = -1;
    for (unsigned char character : text) {
        unsigned value{};
        if (character >= '0' && character <= '9') value = character - '0';
        else if (character >= 'a' && character <= 'f')
            value = character - 'a' + 10;
        else if (character >= 'A' && character <= 'F')
            value = character - 'A' + 10;
        else
            continue;
        if (high < 0) high = static_cast<int>(value);
        else {
            bytes.push_back(static_cast<char>((high << 4) | value));
            high = -1;
        }
    }
    if (high >= 0) throw std::runtime_error{"odd hex digit count in " + path};
    return bytes;
}

std::map<std::string, Span> messageFields(std::string_view bytes) {
    if (bytes.size() < 3) throw std::runtime_error{"truncated message"};
    WireWalker walker{bytes};
    auto fields = walker.object(2);
    if (walker.skip(2) != bytes.size())
        throw std::runtime_error{"wire walker did not consume the message"};
    return fields;
}

std::map<std::string, Span> sectionFields(std::string_view bytes) {
    auto root = messageFields(bytes);
    auto found = root.find("sections");
    if (found == root.end()) throw std::runtime_error{"snapshot has no sections"};
    return WireWalker{bytes}.object(found->second.offset);
}

template <typename Range>
std::set<std::string> strings(Range const& range) {
    std::set<std::string> result;
    for (auto value : range) result.emplace(value);
    return result;
}

struct PayloadMeasurement {
    std::string name;
    std::size_t specializedBytes{};
    std::size_t genericBytes{};
    double specializedNanoseconds{};
    double genericNanoseconds{};
    bool latencyRetention{};
    std::string caseName;
};

std::vector<PayloadMeasurement> measurePayloads(
    std::string_view snapshotBytes, std::string_view deltaBytes) {
    auto const snapshots = sectionFields(snapshotBytes);
    auto const deltas = messageFields(deltaBytes);
    if (strings(ssg::detail::generated::kSemanticSnapshotFields) !=
        strings(snapshots | std::views::keys)) {
        throw std::runtime_error{"snapshot field inventory mismatch"};
    }
    for (auto field : ssg::detail::generated::kSemanticDeltaFields) {
        if (!deltas.contains(std::string{field}))
            throw std::runtime_error{"delta field inventory mismatch"};
    }

    std::vector<PayloadMeasurement> result;
    for (auto const& section : ssg::detail::generated::kSemanticSections) {
        if (section.lifecycle != ManifestLifecycle::Current ||
            section.replay != ReplayPolicy::Specialized)
            continue;
        auto const snapshot = snapshots.find(std::string{section.snapshotField});
        if (snapshot == snapshots.end())
            throw std::runtime_error{"missing snapshot field " +
                                     std::string{section.snapshotField}};
        std::size_t specialized{};
        for (std::size_t index = 0; index < section.deltaCount; ++index) {
            auto const field = std::string{
                ssg::detail::generated::kSemanticDeltaFields[
                    section.deltaOffset + index]};
            auto const delta = deltas.find(field);
            if (delta == deltas.end())
                throw std::runtime_error{"missing delta field " + field};
            specialized += delta->second.size;
        }
        constexpr std::size_t kReplacementEnvelopeBytes =
            1U + 4U + 4U + std::string_view{"replacement"}.size();
        result.push_back({
            std::string{section.symbol}, specialized,
            kReplacementEnvelopeBytes + snapshot->second.size});
    }
    if (strings(ssg::detail::generated::kSpecializedSemanticSections) !=
        strings(result | std::views::transform(
            [](auto const& item) -> std::string const& { return item.name; }))) {
        throw std::runtime_error{"specialized benchmark inventory mismatch"};
    }
    return result;
}

struct EncodedScenario {
    std::string target;
    std::string delta;
};

EncodedScenario encodeScenario(
    ssg::SessionSnapshot const& envelope,
    ssg::SessionSnapshotSections beforeSections,
    ssg::SessionSnapshotSections targetSections) {
    ssg::SessionSnapshot before{
        ssg::Revision{100}, envelope.topology(), envelope.client(),
        std::move(beforeSections)};
    ssg::SessionSnapshot target{
        ssg::Revision{101}, envelope.topology(), envelope.client(),
        std::move(targetSections)};
    auto delta = ssg::SessionSnapshotCodec{}.deriveDelta(before, target);
    ssg::ProtocolCodec codec;
    return {codec.encodeSessionSnapshot(target), codec.encodeSessionDelta(delta)};
}

PayloadMeasurement measurementFor(
    std::string_view name, EncodedScenario const& scenario) {
    auto measurements = measurePayloads(scenario.target, scenario.delta);
    auto found = std::ranges::find(measurements, name,
                                   &PayloadMeasurement::name);
    if (found == measurements.end())
        throw std::runtime_error{"missing scaled payload case"};
    return *found;
}

ssg::UiNode* findUiNode(ssg::UiNode& node, std::string_view id) {
    if (node.id.value() == id) return &node;
    auto* container = std::get_if<ssg::UiContainer>(&node.content);
    if (!container) return nullptr;
    for (auto& child : container->children) {
        if (auto* found = findUiNode(child, id)) return found;
    }
    return nullptr;
}

std::vector<PayloadMeasurement> scaledPayloadCases(
    ssg::SessionSnapshot const& envelope, std::size_t scaledItems,
    std::string caseName) {
    std::vector<PayloadMeasurement> result;

    {
        auto before = envelope.sections();
        before.document = {
            ssg::Revision{10}, std::string(scaledItems * 64U, 'a'),
            ssg::ByteOffset{0}, std::nullopt};
        auto target = before;
        target.document.revision = ssg::Revision{11};
        target.document.text[scaledItems * 32U] = 'b';
        result.push_back(measurementFor(
            "Document", encodeScenario(envelope, std::move(before),
                                       std::move(target))));
    }
    {
        auto before = envelope.sections();
        before.diff = {ssg::Revision{10}, {}};
        before.diff.files.reserve(scaledItems);
        for (std::size_t index = 0; index < scaledItems; ++index) {
            auto suffix = std::to_string(index);
            before.diff.files.push_back({
                ssg::DiffFileId{"diff:" + suffix}, "src/" + suffix + ".cpp",
                std::nullopt, false, ssg::DiffFileStatus::Modified,
                "baseline-" + suffix, "content-" + suffix, {}, {}});
        }
        auto target = before;
        target.diff.revision = ssg::Revision{11};
        target.diff.files.back().currentContent = "changed";
        result.push_back(measurementFor(
            "Diff", encodeScenario(envelope, std::move(before),
                                   std::move(target))));
    }
    {
        auto before = envelope.sections();
        before.externalModification = {ssg::Revision{10}, "changes", {},
                                       std::nullopt};
        before.externalModification.files.reserve(scaledItems);
        for (std::size_t index = 0; index < scaledItems; ++index) {
            auto suffix = std::to_string(index);
            before.externalModification.files.push_back({
                ssg::DiffFileId{"external:" + suffix},
                "src/" + suffix + ".cpp",
                ssg::ExternalDocumentStatus::ExternallyModified,
                "externally modified", "M",
                {ssg::externalActionAffordance(ssg::ExternalAction::Reload)}});
        }
        auto target = before;
        target.externalModification.revision = ssg::Revision{11};
        target.externalModification.files.back().statusLabel = "changed";
        result.push_back(measurementFor(
            "ExternalModification",
            encodeScenario(envelope, std::move(before), std::move(target))));
    }
    {
        auto before = envelope.sections();
        ssg::TreeProviderView provider{
            ssg::TreeProviderId{"files"}, ssg::TreeProviderKind::Filesystem,
            {}, std::nullopt};
        provider.nodes.reserve(scaledItems);
        for (std::size_t index = 0; index < scaledItems; ++index) {
            auto suffix = std::to_string(index);
            provider.nodes.push_back({
                {ssg::TreeNodeId{"node:" + suffix}, std::nullopt,
                 "file-" + suffix, ssg::TreeNodeKind::File, std::nullopt, {},
                 std::nullopt, "src/" + suffix, std::nullopt, false},
                0, false});
        }
        before.tree = {
            ssg::TreeRevision{10}, {provider},
            ssg::TreeProviderBinding{
                ssg::TreeProviderId{"files"},
                ssg::TreeProviderKind::Filesystem}};
        auto target = before;
        target.tree.revision = ssg::TreeRevision{11};
        target.tree.providers.front().nodes.back().node.label = "changed";
        result.push_back(measurementFor(
            "Tree", encodeScenario(envelope, std::move(before),
                                   std::move(target))));
    }
    {
        auto before = envelope.sections();
        auto schema = before.uiFrame.schema();
        auto* host = findUiNode(schema.root, "header.left");
        auto* container = host
            ? std::get_if<ssg::UiContainer>(&host->content)
            : nullptr;
        if (!container)
            throw std::runtime_error{"UI frame has no benchmark host"};
        container->children.reserve(container->children.size() + scaledItems);
        for (std::size_t index = 0; index < scaledItems; ++index) {
            auto id = "benchmark." + std::to_string(index);
            ssg::WidgetDescriptor widget;
            widget.kind = ssg::WidgetKind::Label;
            widget.id = id;
            widget.value = ssg::ValueSource{
                false, "value-" + std::to_string(index), {}};
            container->children.push_back({
                ssg::UiNodeId{id}, ssg::Size{}, ssg::UiLeaf{widget}, {},
                std::nullopt, std::nullopt});
        }
        auto validated = ssg::ValidatedSchema::validate(schema);
        if (!validated.ok())
            throw std::runtime_error{"scaled UI schema is invalid"};
        auto state = ssg::resolveUiState(
            validated.schema(), [](std::string_view) {
                return std::optional<ssg::ResolvedProvider>{};
            });
        for (auto& record : state.nodes) {
            auto original = std::ranges::find(
                before.uiFrame.state().nodes, record.id,
                &ssg::UiNodeState::id);
            if (original != before.uiFrame.state().nodes.end())
                record = *original;
        }
        state.focusPath = before.uiFrame.focusPath();
        auto presence = ssg::buildPresenceSection(
            validated.schema(),
            ssg::PresenceConfig::allPresent(validated.schema()));
        presence.basis = before.uiFrame.presence().basis;
        for (auto& record : presence.nodes) {
            auto original = std::ranges::find(
                before.uiFrame.presence().nodes, record.id,
                &ssg::UiPresenceRecord::id);
            if (original != before.uiFrame.presence().nodes.end())
                record.present = original->present;
        }
        before.uiFrame = ssg::UiFrame::require(
            std::move(schema), state, presence);
        auto target = before;
        auto changed = std::ranges::find_if(
            state.nodes.rbegin(), state.nodes.rend(),
            [](auto const& node) { return node.leaf.has_value(); });
        if (changed == state.nodes.rend())
            throw std::runtime_error{"scaled UI frame has no stateful leaf"};
        changed->leaf->value = "changed";
        target.uiFrame = ssg::UiFrame::require(
            before.uiFrame.schema(), std::move(state), std::move(presence));
        result.push_back(measurementFor(
            "UiFrame", encodeScenario(envelope, std::move(before),
                                      std::move(target))));
    }
    for (auto& measurement : result) measurement.caseName = caseName;
    return result;
}

struct TimingCase {
    std::string_view name;
    std::function<bool()> specialized;
    std::function<bool()> generic;
};

template <typename Operation>
double measure(Operation const& operation) {
    for (std::size_t index = 0; index < kWarmupSamples; ++index) {
        if (!operation()) throw std::runtime_error{"replay warmup diverged"};
    }
    auto const start = Clock::now();
    for (std::size_t index = 0; index < kMeasuredSamples; ++index) {
        if (!operation()) throw std::runtime_error{"replay sample diverged"};
    }
    return std::chrono::duration<double, std::nano>(
               Clock::now() - start).count() /
           static_cast<double>(kMeasuredSamples);
}

std::vector<TimingCase> timingCases(
    ssg::SessionSnapshot const& base, ssg::SessionSnapshot const& target,
    ssg::SessionDelta const& delta) {
    auto const& before = base.sections();
    auto const& after = target.sections();
    std::vector<TimingCase> cases;
    cases.push_back({
        "Document",
        [&] {
            auto value = before.document;
            if (delta.document()) {
                auto replayed = ssg::DocumentSnapshotCodec{}.replay(
                    value, *delta.document(),
                    delta.documentCaret().value_or(value.caret));
                if (!replayed) return false;
                value = std::move(*replayed);
            }
            else if (delta.documentCaret()) value.caret = *delta.documentCaret();
            consume(&value);
            return value == after.document;
        },
        [&] {
            auto value = before.document;
            value = after.document;
            consume(&value);
            return value == after.document;
        }});
    cases.push_back({
        "Settings",
        [&] {
            auto value = before.settings;
            for (auto const& change : delta.settings().changes) {
                auto const index = static_cast<std::size_t>(change.key);
                if (index >= value.entries.size() ||
                    value.entries[index].effective != change.before)
                    return false;
                value.entries[index].effective = change.after;
            }
            consume(&value);
            return value == after.settings;
        },
        [&] {
            auto value = before.settings;
            value = after.settings;
            consume(&value);
            return value == after.settings;
        }});
    cases.push_back({
        "Diff",
        [&] {
            auto replayed =
                ssg::DiffDeltaCodec{}.replay(before.diff, delta.diff());
            if (!replayed.accepted()) return false;
            consume(&*replayed.state);
            return *replayed.state == after.diff;
        },
        [&] {
            auto value = before.diff;
            value = after.diff;
            consume(&value);
            return value == after.diff;
        }});
    cases.push_back({
        "ExternalModification",
        [&] {
            auto replayed = ssg::ExternalModificationDeltaCodec{}.replay(
                before.externalModification, delta.externalModification());
            if (!replayed.accepted()) return false;
            consume(&*replayed.state);
            return *replayed.state == after.externalModification;
        },
        [&] {
            auto value = before.externalModification;
            value = after.externalModification;
            consume(&value);
            return value == after.externalModification;
        }});
    cases.push_back({
        "Tree",
        [&] {
            auto replayed =
                ssg::TreeDeltaCodec{}.replay(before.tree, delta.tree());
            if (!replayed.accepted()) return false;
            consume(&*replayed.state);
            return *replayed.state == after.tree;
        },
        [&] {
            auto value = before.tree;
            value = after.tree;
            consume(&value);
            return value == after.tree;
        }});
    cases.push_back({
        "UiFrame",
        [&] {
            auto replayed = ssg::UiFrameDeltaCodec{}.replay(
                before.uiFrame, delta.uiFrameDelta());
            if (!replayed.accepted()) return false;
            consume(&*replayed.frame);
            return *replayed.frame == after.uiFrame;
        },
        [&] {
            auto value = before.uiFrame;
            value = after.uiFrame;
            consume(&value);
            return value == after.uiFrame;
        }});
    return cases;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        bool const verifyOnly =
            argc == 2 && std::string_view{argv[1]} == "--verify-only";
        bool const enforce =
            argc == 2 && std::string_view{argv[1]} == "--enforce";
        if (argc > 2 || (argc == 2 && !verifyOnly && !enforce))
            throw std::runtime_error{
                "usage: protocol_delta_benchmark [--verify-only|--enforce]"};
        verifyWireWalker();
        auto const snapshot = readHex(
            std::string{SSG_PROTOCOL_FIXTURES_DIR} +
            "/session_semantic_target.hex");
        auto const delta = readHex(
            std::string{SSG_PROTOCOL_FIXTURES_DIR} +
            "/session_semantic_delta.hex");
        auto measurements = measurePayloads(snapshot, delta);
        if (measurements.size() !=
            ssg::detail::generated::kSpecializedSemanticSections.size()) {
            throw std::runtime_error{"incomplete specialized measurements"};
        }
        ssg::ProtocolCodec codec;
        auto decodedBase = codec.decodeSessionSnapshot(readHex(
            std::string{SSG_PROTOCOL_FIXTURES_DIR} +
            "/session_semantic_base.hex"));
        auto decodedTarget = codec.decodeSessionSnapshot(snapshot);
        auto decodedDelta = codec.decodeSessionDelta(delta);
        if (!decodedBase.accepted() || !decodedTarget.accepted() ||
            !decodedDelta.accepted()) {
            throw std::runtime_error{"canonical protocol fixture rejected"};
        }
        auto cases = timingCases(
            *decodedBase.snapshot, *decodedTarget.snapshot, *decodedDelta.delta);
        auto scaledSmall = scaledPayloadCases(
            *decodedTarget.snapshot, 64, "scaled-small");
        auto scaledLarge = scaledPayloadCases(
            *decodedTarget.snapshot, 512, "scaled-large");
        if (strings(ssg::detail::generated::kSpecializedSemanticSections) !=
            strings(cases | std::views::transform(
                [](auto const& item) { return item.name; }))) {
            throw std::runtime_error{"replay benchmark inventory mismatch"};
        }
        for (auto const& item : cases) {
            if (!item.specialized() || !item.generic())
                throw std::runtime_error{"replay equality failed for " +
                                         std::string{item.name}};
        }
        if (verifyOnly) {
            std::cout << "protocol delta benchmark correctness verified\n";
            return 0;
        }
        std::cout << std::fixed << std::setprecision(3);
        for (auto& measurement : measurements) {
            auto const item = std::ranges::find(
                cases, measurement.name, &TimingCase::name);
            if (item == cases.end())
                throw std::runtime_error{"missing timing case"};
            std::vector<double> specializedRuns;
            std::vector<double> genericRuns;
            std::size_t latencyAgreements{};
            for (std::size_t run = 0; run < kIndependentRuns; ++run) {
                specializedRuns.push_back(measure(item->specialized));
                genericRuns.push_back(measure(item->generic));
                auto const ratio =
                    genericRuns.back() / specializedRuns.back();
                if (ratio >
                    kMaximumGenericReplayLatencyRatio *
                        (1.0 + kLatencyConfidenceMargin)) {
                    ++latencyAgreements;
                }
            }
            std::ranges::sort(specializedRuns);
            std::ranges::sort(genericRuns);
            measurement.specializedNanoseconds =
                specializedRuns[specializedRuns.size() / 2U];
            measurement.genericNanoseconds =
                genericRuns[genericRuns.size() / 2U];
            measurement.latencyRetention =
                latencyAgreements >= kRequiredLatencyAgreements;
            std::cout << "section=" << measurement.name
                      << " specialized_bytes=" << measurement.specializedBytes
                      << " generic_bytes=" << measurement.genericBytes
                      << " payload_ratio="
                      << static_cast<double>(measurement.genericBytes) /
                             static_cast<double>(measurement.specializedBytes)
                      << " specialized_replay_ns="
                      << measurement.specializedNanoseconds
                      << " generic_replay_ns="
                      << measurement.genericNanoseconds
                      << " latency_ratio="
                      << measurement.genericNanoseconds /
                             measurement.specializedNanoseconds
                      << '\n';
        }
        for (auto const& measurement : scaledSmall) {
            std::cout << "section=" << measurement.name
                      << " case=" << measurement.caseName
                      << " specialized_bytes=" << measurement.specializedBytes
                      << " generic_bytes=" << measurement.genericBytes
                      << " payload_ratio="
                      << static_cast<double>(measurement.genericBytes) /
                             static_cast<double>(measurement.specializedBytes)
                      << '\n';
        }
        for (auto const& measurement : scaledLarge) {
            std::cout << "section=" << measurement.name
                      << " case=" << measurement.caseName
                      << " specialized_bytes=" << measurement.specializedBytes
                      << " generic_bytes=" << measurement.genericBytes
                      << " payload_ratio="
                      << static_cast<double>(measurement.genericBytes) /
                             static_cast<double>(measurement.specializedBytes)
                      << '\n';
        }
        for (auto const& measurement : measurements) {
            auto const payloadRatio =
                static_cast<double>(measurement.genericBytes) /
                static_cast<double>(measurement.specializedBytes);
            auto const latencyRatio =
                measurement.genericNanoseconds /
                measurement.specializedNanoseconds;
            auto const large = std::ranges::find(
                scaledLarge, measurement.name, &PayloadMeasurement::name);
            auto const small = std::ranges::find(
                scaledSmall, measurement.name, &PayloadMeasurement::name);
            auto const scaledPayload =
                large != scaledLarge.end() &&
                static_cast<double>(large->genericBytes) /
                        static_cast<double>(large->specializedBytes) >
                    kMaximumGenericDeltaPayloadRatio;
            auto const asymptotic =
                large != scaledLarge.end() && small != scaledSmall.end() &&
                large->genericBytes > small->genericBytes * 4U &&
                large->specializedBytes < small->specializedBytes * 2U;
            auto const reason =
                asymptotic ? SpecializedDecision::Asymptotic
                : payloadRatio > kMaximumGenericDeltaPayloadRatio ||
                        scaledPayload
                    ? SpecializedDecision::Payload
                : measurement.latencyRetention
                    ? SpecializedDecision::Latency
                    : SpecializedDecision::Ordinary;
            auto const fact = std::ranges::find(
                ssg::detail::generated::kSemanticSections,
                measurement.name,
                &ssg::detail::generated::SemanticSectionFact::symbol);
            if (fact == ssg::detail::generated::kSemanticSections.end())
                throw std::runtime_error{"missing generated section decision"};
            if (enforce && fact->retention != reason)
                throw std::runtime_error{
                    "specialized decision drift for " + measurement.name};
            std::cout << "decision section=" << measurement.name
                      << " retain=" << (payloadRatio >
                                               kMaximumGenericDeltaPayloadRatio ||
                                           scaledPayload ||
                                           measurement.latencyRetention ||
                                           asymptotic
                                       ? "true"
                                       : "false")
                      << " reason="
                      << (reason == SpecializedDecision::Asymptotic
                              ? "asymptotic"
                          : reason == SpecializedDecision::Payload ? "payload"
                          : reason == SpecializedDecision::Latency ? "latency"
                                                                  : "ordinary")
                      << '\n';
        }
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "protocol_delta_benchmark: " << error.what() << '\n';
        return 1;
    }
}
