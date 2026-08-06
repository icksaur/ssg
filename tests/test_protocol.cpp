#include "test_helpers.h"

#include <ssg/EditorSessionBuilder.h>
#include <ssg/FileCommands.h>
#include <ssg/FindReplace.h>
#include <ssg/CommandCatalog.h>

#include "all_command_ids.h"
#include <ssg/EditorRuntime.h>

#include <unistd.h>

#include <filesystem>
#include <string>
#include <ssg/Protocol.h>
#include <ssg/session_snapshot.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef SSG_PROTOCOL_FIXTURES_DIR
#error "SSG_PROTOCOL_FIXTURES_DIR must name the fixtures directory"
#endif

namespace {

// ---------------------------------------------------------------------------
// Fixture builders. Mirrors tests/test_editor_session_assembly.cpp's
// SessionSnapshotSections fixture so this file exercises the same complete,
// every-section-populated snapshot shape through the wire codec.

// Every command the editor offers, asked of a real runtime: no single file
// lists them, because each is declared by the component that implements it.
std::vector<std::string> catalogIds() {
    std::vector<std::string> ids;
    for (auto const& facts : ssg::testing::allCommandFacts()) {
        ids.push_back(facts.id);
    }
    return ids;
}

ssg::SelectionViewState selection(std::uint64_t byte, std::uint32_t firstRow) {
    ssg::DocumentPosition const position{
        ssg::ByteOffset{byte}, ssg::LineIndex{0}, ssg::CellIndex{byte}};
    return {ssg::SelectionSet{{ssg::Selection{position, position}}},
            firstRow, 0, std::nullopt};
}

ssg::SessionSnapshotSections sections(ssg::Revision revision, std::string marker) {
    ssg::SettingsViewState settings;
    settings.entries[0].effective = {
        static_cast<std::uint32_t>(marker.size()), ssg::SettingScope::User};
    ssg::ThemeSnapshot theme;
    theme.roleColors[0].red = static_cast<std::uint8_t>(marker.size());
    theme.syntaxColors[0].green = static_cast<std::uint8_t>(marker.size());

    ssg::ShellViewState shell;
    shell.viewport = {static_cast<int>(20 + marker.size()), 8};

    return {
        {revision, marker, ssg::ByteOffset{marker.size()}},
        selection(marker.size(), static_cast<std::uint32_t>(marker.size())),
        {true, false, marker.size()},
        {{marker}, marker, std::nullopt},
        {std::nullopt, {{}, marker.size()}, ssg::PromptKind::Palette},
        {revision, true, marker, ssg::SearchMode::File, {}, std::nullopt,
         marker.size(), false},
        {marker.size(), true, false, revision, marker, {}, {}, {}, std::nullopt,
         ssg::FindReplaceError::None, {}},
        settings,
        {marker, {}},
        {{marker.size() > 1 ? ssg::TextEncoding::Utf16le
                           : ssg::TextEncoding::Utf8,
          ssg::LineEnding::Lf, false,
          !marker.empty()}},
        {{{ssg::TabId{1}, ssg::TabKind::ReadOnlyOutput, std::nullopt,
           std::nullopt, "output", marker, ssg::DocumentMode::ReadOnly,
           false, ssg::TabRecoveryBadge::None}},
         ssg::TabId{1}},
        {revision, {}},
        {revision, {}},
        {marker.size(), ssg::FollowMode::Following, ssg::PaneId{},
         std::nullopt, {}, {}},
        {ssg::TreeRevision{marker.size()}, {}},
        ssg::plainTextSyntaxViewState(revision, ssg::LanguageId{"plain"},
                                          marker, 4),
        {revision, {}},
        {revision, {}, std::nullopt, {}, marker},
        theme,
        ssg::Style{},
        std::move(shell),
    };
}

ssg::ViewportViewState clientView(std::uint32_t firstRow) {
    return {ssg::ViewportDimensions{20, 8},
            firstRow,
            0,  // first_visual_column
            firstRow + 8,
            {},
            {},
            {},
            {firstRow + 8, 8, firstRow, firstRow, 0, 8}};
}


// The catalog these wire tests encode against.
//
// It cannot be built from the static table alone: components are migrating off
// it, so a command already registered by its component would be missing here
// and encoding it would throw.  A real runtime is the only thing that knows the
// whole catalog, which is the point of the migration.  Handlers still come from
// the static rows because these tests exercise the WIRE, not dispatch.
//
// Deleted with the static table (doc/spec-command-registry.md, D5).
std::shared_ptr<ssg::CommandCatalog> staticTableCatalog() {
    static auto const catalog = [] {
        auto const root = std::filesystem::temp_directory_path() /
                          ("ssg-protocol-catalog-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        auto created = ssg::EditorRuntime::create({root});
        auto result = created.runtime ? created.runtime->commandCatalog()
                                      : nullptr;
        // The runtime owns the catalog; keep it alive for the test's lifetime.
        static auto keepAlive = std::move(created.runtime);
        std::filesystem::remove_all(root);
        return result;
    }();
    return catalog;
}


// ---------------------------------------------------------------------------
// Registry coverage and construction validation.

TEST(registryCoversEveryP0CommandAndRejectsUnknownIds) {
    auto registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    for (auto const& id : catalogIds()) {
        ASSERT_TRUE(registry.contains(id));
    }
    ASSERT_FALSE(registry.contains("not.a.command"));
    ASSERT_THROWS(registry.encodeArgument("not.a.command", std::any{}),
                 std::invalid_argument);
}

ssg::CommandArgumentCodec makeProbeCodec() {
    return ssg::CommandArgumentCodec{
        [](std::any const&) { return ssg::ProtocolValue::makeNull(); },
        [](ssg::ProtocolValue const&) -> std::optional<std::any> {
            return std::any{};
        }};
}

// The SettingKey decode array in Protocol.cpp is hand-maintained and separate
// from the enum, so a key added to Settings.h alone compiles and links but is
// undecodable over the wire.  Round-tripping EVERY key through a settings.set
// command makes that gap fail here rather than at runtime, for this key and any
// future one.
TEST(everySettingKeyRoundTripsThroughTheCommandCodec) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    for (std::size_t index = 0; index < ssg::kSettingKeyCount; ++index) {
        auto const key = static_cast<ssg::SettingKey>(index);
        ssg::SettingSetArguments arguments{ssg::SettingScope::User, key,
                                           ssg::SettingValue{true}};
        auto const encoded = ssg::ProtocolCodec{}.encodeCommandRequest(
            ssg::ClientCommand{"settings.set", ssg::Revision{1}, arguments},
            registry);
        auto decoded = ssg::ProtocolCodec{}.decodeCommandRequest(encoded, registry);
        ASSERT_TRUE(decoded.accepted());
        if (!decoded.accepted()) continue;
        auto const* roundTripped =
            std::any_cast<ssg::SettingSetArguments>(&decoded.command->payload);
        ASSERT_TRUE(roundTripped != nullptr);
        if (roundTripped != nullptr) ASSERT_TRUE(roundTripped->key == key);
    }
}

// registryRejectsMissingEntries / RejectsExtraEntries / RejectsDuplicateEntries
// are deleted with the registry constructor they exercised.  They checked that a
// hand-assembled list of codecs covered every command exactly once; the registry
// is now backed by the catalog itself, so a command's codec is found by looking
// the command up.  There is no list to get wrong (doc/spec-command-registry.md,
// R8).

// ---------------------------------------------------------------------------
// Command request round trips: one canonical fixture per argument shape.

TEST(commandRequestRoundTripsWithPaletteExecuteArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{
        "palette.execute", ssg::Revision{4},
        ssg::PaletteExecuteArguments{"file.save"}};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.command->id, command.id);
    auto const* arguments =
        std::any_cast<ssg::PaletteExecuteArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(*arguments,
              std::any_cast<ssg::PaletteExecuteArguments>(command.payload));
}

TEST(commandRequestRoundTripsWithFindQueryArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{
        "find.update_query", ssg::Revision{7},
        ssg::FindQueryArguments{"cat"}};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.command->id, command.id);
    auto const* arguments =
        std::any_cast<ssg::FindQueryArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(*arguments,
              std::any_cast<ssg::FindQueryArguments>(command.payload));
}


// The registry test only proves an entry EXISTS for each command, not that it
// is the right one -- a payload-bearing command wired to noneCodec passes it
// while silently dropping the payload for every protocol client. Round-tripping
// the actual value is what catches that.
TEST(commandRequestRoundTripsWithPromptValueArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{
        "prompt.update_value", ssg::Revision{11},
        ssg::PromptValueArguments{1, "notes/draft.txt"}};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.command->id, command.id);
    auto const* arguments =
        std::any_cast<ssg::PromptValueArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(*arguments,
              std::any_cast<ssg::PromptValueArguments>(command.payload));
}


// A payload-bearing command reaching the WRONG codec entry (or none) is
// invisible to the registry's exhaustiveness check, which only proves an entry
// exists. Each payload-bearing command therefore round-trips its own id.
TEST(commandRequestRoundTripsWithTreeScrollToFraction) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{
        "tree.scroll_to_fraction", ssg::Revision{5},
        ssg::ScrollFractionArguments{3, 8}};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.command->id, command.id);
    auto const* arguments =
        std::any_cast<ssg::ScrollFractionArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(*arguments,
              std::any_cast<ssg::ScrollFractionArguments>(command.payload));
}


TEST(commandRequestRoundTripsWithTreeSelectArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{
        "tree.select", ssg::Revision{9},
        ssg::TreeSelectArguments{ssg::TreeNodeId{"files:src/main.cpp"}}};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.command->id, command.id);
    auto const* arguments =
        std::any_cast<ssg::TreeSelectArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(*arguments,
              std::any_cast<ssg::TreeSelectArguments>(command.payload));
}

TEST(commandRequestRoundTripsWithNoPayload) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{"edit.undo", ssg::Revision{3}, {}};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.command.has_value());
    ASSERT_EQ(decoded.command->id, command.id);
    ASSERT_EQ(decoded.command->baseRevision, command.baseRevision);
    ASSERT_FALSE(decoded.command->payload.has_value());
}

TEST(commandRequestRoundTripsWithTextInputArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{
        "text.insert", ssg::Revision{5},
        ssg::TextInputArguments{"hello world"}};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.command->id, command.id);
    auto const* arguments =
        std::any_cast<ssg::TextInputArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(*arguments, std::any_cast<ssg::TextInputArguments>(command.payload));
}

TEST(commandRequestRoundTripsWithSelectionCommandArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::DocumentPosition const position{ssg::ByteOffset{4}, ssg::LineIndex{0},
                                         ssg::CellIndex{4}};
    ssg::SelectionCommandArguments const original{
        position, ssg::Selection{position, position}};
    ssg::ClientCommand const command{"cursor.set_position", ssg::Revision{2},
                                     original};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments = std::any_cast<ssg::SelectionCommandArguments>(
        &decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(arguments->position, original.position);
    ASSERT_EQ(arguments->selection, original.selection);
}

TEST(commandRequestRoundTripsWithEmptySelectionCommandArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::SelectionCommandArguments const original{std::nullopt, std::nullopt};
    ssg::ClientCommand const command{"cursor.left", ssg::Revision{2}, original};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments = std::any_cast<ssg::SelectionCommandArguments>(
        &decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_FALSE(arguments->position.has_value());
    ASSERT_FALSE(arguments->selection.has_value());
}

TEST(commandRequestRoundTripsWithSelectionsListSelectionCommandArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::DocumentPosition const a{ssg::ByteOffset{1}, ssg::LineIndex{0},
                                  ssg::CellIndex{1}};
    ssg::DocumentPosition const b{ssg::ByteOffset{4}, ssg::LineIndex{0},
                                  ssg::CellIndex{4}};
    ssg::DocumentPosition const c{ssg::ByteOffset{7}, ssg::LineIndex{1},
                                  ssg::CellIndex{0}};
    ssg::SelectionCommandArguments original;
    original.selections = {ssg::Selection{a, a}, ssg::Selection{b, c}};
    ssg::ClientCommand const command{"select.set_ranges", ssg::Revision{3},
                                     original};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments = std::any_cast<ssg::SelectionCommandArguments>(
        &decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_FALSE(arguments->position.has_value());
    ASSERT_FALSE(arguments->selection.has_value());
    ASSERT_EQ(arguments->selections.size(), original.selections.size());
    ASSERT_TRUE(arguments->selections == original.selections);
}

TEST(commandRequestRoundTripsWithScrollLinesArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{"view.scroll_lines", ssg::Revision{1},
                                     ssg::ScrollLinesArguments{-7}};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments =
        std::any_cast<ssg::ScrollLinesArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(arguments->rows, std::int64_t{-7});
}

TEST(commandRequestRoundTripsWithScrollPagesArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{"view.scroll_pages", ssg::Revision{1},
                                     ssg::ScrollPagesArguments{3}};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments =
        std::any_cast<ssg::ScrollPagesArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(arguments->pages, std::int64_t{3});
}

TEST(commandRequestRoundTripsWithScrollFractionArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{"view.scroll_to_fraction", ssg::Revision{1},
                                     ssg::ScrollFractionArguments{3, 4}};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments =
        std::any_cast<ssg::ScrollFractionArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(arguments->numerator, std::uint32_t{3});
    ASSERT_EQ(arguments->denominator, std::uint32_t{4});
}

TEST(commandRequestRoundTripsWithDroppedContentArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{
        "file.open_dropped_content", ssg::Revision{1},
        ssg::DroppedContentArguments{{1, 2, 3, 4}, "dropped.txt"}};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments =
        std::any_cast<ssg::DroppedContentArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(arguments->bytes,
             (std::vector<std::uint8_t>{1, 2, 3, 4}));
    ASSERT_EQ(arguments->suggestedLabel, std::string{"dropped.txt"});
}

std::string wireU8(std::uint8_t value) {
    return std::string(1, static_cast<char>(value));
}

void appendU32(std::string& out, std::uint32_t value) {
    for (int index = 0; index < 4; ++index) {
        out.push_back(static_cast<char>((value >> (8 * index)) & 0xFF));
    }
}

void appendU64(std::string& out, std::uint64_t value) {
    for (int index = 0; index < 8; ++index) {
        out.push_back(static_cast<char>((value >> (8 * index)) & 0xFF));
    }
}

void appendTextValue(std::string& out, std::string const& text) {
    out += wireU8(4);
    appendU32(out, static_cast<std::uint32_t>(text.size()));
    out += text;
}

void appendUintValue(std::string& out, std::uint64_t value) {
    out += wireU8(3);
    appendU64(out, value);
}

void appendNullValue(std::string& out) { out += wireU8(0); }

void appendFieldKey(std::string& out, std::string const& key) {
    appendU32(out, static_cast<std::uint32_t>(key.size()));
    out += key;
}

// Hand-builds a `command_request` wire message directly against the
// documented [u8 version][u8 kind][value] envelope and object/text/uint tag
// scheme, independent of protocol.cpp's private encoder. This is the only
// way to exercise decode_command_request's unknown-command-id rejection: the
// public encode_command_request() itself throws before producing bytes for
// an id the registry does not recognize.
std::string buildCommandRequestMessage(std::string const& id,
                                          std::uint64_t baseRevision) {
    std::string body;
    body += wireU8(7);
    appendU32(body, 3);
    appendFieldKey(body, "id");
    appendTextValue(body, id);
    appendFieldKey(body, "base_revision");
    appendUintValue(body, baseRevision);
    appendFieldKey(body, "payload");
    appendNullValue(body);

    std::string message;
    message += wireU8(1);
    message += wireU8(
        static_cast<std::uint8_t>(ssg::ProtocolMessageKind::CommandRequest));
    message += body;
    return message;
}

std::string buildInvalidScrollFractionMessage() {
    std::string payload;
    payload += wireU8(7);
    appendU32(payload, 2);
    appendFieldKey(payload, "numerator");
    appendUintValue(payload, 0);
    appendFieldKey(payload, "denominator");
    appendUintValue(payload, 0);

    std::string body;
    body += wireU8(7);
    appendU32(body, 3);
    appendFieldKey(body, "id");
    appendTextValue(body, "view.scroll_to_fraction");
    appendFieldKey(body, "base_revision");
    appendUintValue(body, 1);
    appendFieldKey(body, "payload");
    body += payload;

    return wireU8(1) +
           wireU8(static_cast<std::uint8_t>(
               ssg::ProtocolMessageKind::CommandRequest)) +
           body;
}

TEST(decodeCommandRequestRejectsUnknownCommandId) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ASSERT_THROWS(registry.encodeArgument("not.a.command", std::any{}),
                 std::invalid_argument);

    auto const bytes = buildCommandRequestMessage("not.a.command", 1);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_FALSE(decoded.accepted());
    ASSERT_EQ(decoded.error, ssg::ProtocolError::UnsupportedCommand);
}

TEST(decodeCommandRequestRejectsMalformedPayload) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{"text.insert", ssg::Revision{1},
                                     ssg::TextInputArguments{"x"}};
    auto bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    // Truncate the trailing bytes so the payload's "text" field is cut off,
    // producing a structurally-truncated command request.
    bytes.resize(bytes.size() - 2);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_FALSE(decoded.accepted());
}

TEST(decodeCommandRequestMapsDomainInvariantFailuresToMalformed) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(
        buildInvalidScrollFractionMessage(), registry);
    ASSERT_FALSE(decoded.accepted());
    ASSERT_EQ(decoded.error, ssg::ProtocolError::MalformedMessage);
}

// ---------------------------------------------------------------------------
// Session snapshot / delta round trips, including replay-vs-decoded
// equivalence and two-client isolation.

TEST(sessionSnapshotRoundTripsThroughTheWire) {
    auto snapshot = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{
            ssg::ClientId{7}, ssg::InvocationOrigin::InProcess,
            {ssg::CapabilityId{"local_file_drop"}}},
        ssg::ViewId{9}, clientView(3), sections(ssg::Revision{4}, "alpha"));

    auto const bytes = ssg::ProtocolCodec{}.encodeSessionSnapshot(snapshot);
    auto const decoded = ssg::ProtocolCodec{}.decodeSessionSnapshot(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.snapshot.has_value());
    ASSERT_EQ(*decoded.snapshot, snapshot);
}

// The round-trip above carries a DEFAULT style, so it proves the field is
// present but not that each of the ~29 hand-written codec fields maps to its
// own slot.  A copy-paste error (encoding `top` where `bottom` belongs) would
// survive a default round-trip.  This style makes every field distinct, so a
// mis-wired field decodes to the wrong value and equality fails.
TEST(sessionSnapshotRoundTripsANonDefaultStyle) {
    ssg::Style style;
    style.scrollbar = {"g", "r", "s", "t", "b", "e"};
    style.tree = {"x", "y", 7};
    style.tab = {"D1", "L1"};
    style.toggle = {"C1", "U1"};
    style.truncation = "T1";
    style.inputLineSigil = "S1";
    style.unrenderable = "R1";
    style.promptLabelSeparator = "P1";
    style.dimensions = {21, 5, 25, 13, 19, 2, 3, 4, 6, 7, 8, 9};

    auto snapshotSections = sections(ssg::Revision{4}, "alpha");
    snapshotSections.style = style;
    auto snapshot = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{
            ssg::ClientId{7}, ssg::InvocationOrigin::InProcess,
            {ssg::CapabilityId{"local_file_drop"}}},
        ssg::ViewId{9}, clientView(3), std::move(snapshotSections));

    auto const bytes = ssg::ProtocolCodec{}.encodeSessionSnapshot(snapshot);
    auto const decoded = ssg::ProtocolCodec{}.decodeSessionSnapshot(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.snapshot.has_value());
    if (!decoded.snapshot) return;
    ASSERT_TRUE(decoded.snapshot->sections().style == style);
}

TEST(sessionDeltaRoundTripsAndReplayMatchesTheDecodedDelta) {
    auto before = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), sections(ssg::Revision{4}, "a"));
    auto after = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{5}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(5), sections(ssg::Revision{5}, "changed"));

    auto const delta = ssg::SessionSnapshotCodec{}.deriveDelta(before, after);
    auto const bytes = ssg::ProtocolCodec{}.encodeSessionDelta(delta);
    auto decoded = ssg::ProtocolCodec{}.decodeSessionDelta(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.delta.has_value());

    auto replayed = ssg::SessionSnapshotCodec{}.replay(before, *decoded.delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_TRUE(replayed.snapshot.has_value());
    ASSERT_EQ(*replayed.snapshot, after);
}

TEST(phantomViewportProjectionRoundTripsThroughSnapshotAndDelta) {
    auto projectedView = clientView(0);
    projectedView.visibleRows = {
        ssg::VisualRow{1, 0, 0, ssg::CellIndex{0}, 7, 7, 4}};
    projectedView.rowProjection = {
        ssg::PhantomRow{3, "removed", 4}};
    projectedView.totalVisualRows = 4;
    projectedView.scrollbar.totalRows = 4;

    auto before = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(0), sections(ssg::Revision{4}, "text"));
    auto after = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, projectedView,
        sections(ssg::Revision{5}, "text"));

    const auto snapshotDecoded = ssg::ProtocolCodec{}.decodeSessionSnapshot(
        ssg::ProtocolCodec{}.encodeSessionSnapshot(after));
    ASSERT_TRUE(snapshotDecoded.accepted());
    ASSERT_TRUE(snapshotDecoded.snapshot.has_value());
    if (snapshotDecoded.snapshot) {
        ASSERT_EQ(snapshotDecoded.snapshot->client().viewport.rowProjection,
                  projectedView.rowProjection);
    }

    const auto delta = ssg::SessionSnapshotCodec{}.deriveDelta(before, after);
    const auto deltaDecoded = ssg::ProtocolCodec{}.decodeSessionDelta(
        ssg::ProtocolCodec{}.encodeSessionDelta(delta));
    ASSERT_TRUE(deltaDecoded.accepted());
    ASSERT_TRUE(deltaDecoded.delta.has_value());
    if (!deltaDecoded.delta) return;
    auto replayed =
        ssg::SessionSnapshotCodec{}.replay(before, *deltaDecoded.delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_TRUE(replayed.snapshot.has_value());
    if (replayed.snapshot) {
        ASSERT_EQ(replayed.snapshot->client().viewport.rowProjection,
                  projectedView.rowProjection);
    }
}

TEST(diffWordRangesRoundTripThroughSnapshotAndDelta) {
    auto withWordRanges = [](ssg::Revision revision, std::string marker) {
        auto result = sections(revision, std::move(marker));
        ssg::DiffFileView file{ssg::DiffFileId{"words.cpp"}};
        file.path = "words.cpp";
        file.currentContent = "int foo = 42;\n";
        file.changedLines = {{
            ssg::DiffLineKind::Modified,
            std::size_t{0},
            std::size_t{0},
            {},
            {{10, 1}},
            {{10, 2}},
        }};
        result.diff = {revision, {std::move(file)}};
        return result;
    };

    auto snapshot = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(0),
        withWordRanges(ssg::Revision{5}, "words"));
    const auto decodedSnapshot = ssg::ProtocolCodec{}.decodeSessionSnapshot(
        ssg::ProtocolCodec{}.encodeSessionSnapshot(snapshot));
    ASSERT_TRUE(decodedSnapshot.accepted());
    ASSERT_TRUE(decodedSnapshot.snapshot.has_value());
    if (!decodedSnapshot.snapshot) return;
    ASSERT_EQ(*decodedSnapshot.snapshot, snapshot);

    auto before = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(0), sections(ssg::Revision{4}, "words"));
    auto delta = ssg::SessionSnapshotCodec{}.deriveDelta(before, snapshot);
    const auto decodedDelta = ssg::ProtocolCodec{}.decodeSessionDelta(
        ssg::ProtocolCodec{}.encodeSessionDelta(delta));
    ASSERT_TRUE(decodedDelta.accepted());
    ASSERT_TRUE(decodedDelta.delta.has_value());
    if (!decodedDelta.delta) return;
    auto replayed =
        ssg::SessionSnapshotCodec{}.replay(before, *decodedDelta.delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_TRUE(replayed.snapshot.has_value());
    if (replayed.snapshot) {
        ASSERT_EQ(*replayed.snapshot, snapshot);
    }
}

TEST(twoClientCapabilityAndViewportIsolationSurvivesTheWire) {
    auto shared = sections(ssg::Revision{8}, "shared");
    auto first = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{8}, {},
        ssg::InvocationPrincipal{
            ssg::ClientId{1}, ssg::InvocationOrigin::Websocket,
            {ssg::CapabilityId{"local_file_drop"}}},
        ssg::ViewId{10}, clientView(2), shared);
    auto second = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{8}, {},
        ssg::InvocationPrincipal{ssg::ClientId{2},
                                 ssg::InvocationOrigin::Websocket},
        ssg::ViewId{11}, clientView(7), std::move(shared));

    auto const firstDecoded =
        ssg::ProtocolCodec{}.decodeSessionSnapshot(ssg::ProtocolCodec{}.encodeSessionSnapshot(first));
    auto const secondDecoded =
        ssg::ProtocolCodec{}.decodeSessionSnapshot(ssg::ProtocolCodec{}.encodeSessionSnapshot(second));
    ASSERT_TRUE(firstDecoded.accepted());
    ASSERT_TRUE(secondDecoded.accepted());

    ASSERT_EQ(firstDecoded.snapshot->client().capabilities.size(),
             std::size_t{1});
    ASSERT_TRUE(secondDecoded.snapshot->client().capabilities.empty());
    ASSERT_EQ(firstDecoded.snapshot->client().viewport.firstVisualRow,
             std::uint32_t{2});
    ASSERT_EQ(secondDecoded.snapshot->client().viewport.firstVisualRow,
             std::uint32_t{7});
    ASSERT_EQ(firstDecoded.snapshot->sections(), secondDecoded.snapshot->sections());
}

TEST(sessionSnapshotRoundTripsTreeScrollFields) {
    auto sectionsValue = sections(ssg::Revision{4}, "alpha");
    ssg::TreeNode nodeA{ssg::TreeNodeId{"files:a"}, std::nullopt, "a.txt",
                         ssg::TreeNodeKind::File};
    ssg::TreeNode nodeB{ssg::TreeNodeId{"files:b"}, std::nullopt, "b.txt",
                         ssg::TreeNodeKind::File};
    ssg::TreeProviderView provider{
        ssg::TreeProviderId{"files"}, ssg::TreeProviderKind::Filesystem,
        {ssg::TreeNodeView{nodeA, 0, false}, ssg::TreeNodeView{nodeB, 0, false}},
        ssg::TreeNodeId{"files:b"}};
    provider.firstVisible = 3;
    provider.scrollbar = ssg::Viewport{}.scrollbarMetrics(40, 9, 3);
    provider.visibleNodeIds = {ssg::TreeNodeId{"files:a"},
                                 ssg::TreeNodeId{"files:b"}};
    sectionsValue.tree = ssg::TreeViewState{ssg::TreeRevision{7}, {provider}};
    // Also exercise the shell panel scrollbar gutter geometry on the wire.
    sectionsValue.shell.panel = ssg::Rect{0, 1, 24, 10};
    sectionsValue.shell.panelScrollbar = ssg::Rect{23, 2, 1, 9};
    // And the typed per-tab hit map.
    sectionsValue.shell.tabHits = {ssg::TabHit{ssg::Rect{24, 0, 10, 1}, 0},
                                     ssg::TabHit{ssg::Rect{34, 0, 8, 1}, 1}};

    auto snapshot = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(3), std::move(sectionsValue));
    auto const decoded =
        ssg::ProtocolCodec{}.decodeSessionSnapshot(ssg::ProtocolCodec{}.encodeSessionSnapshot(snapshot));
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.snapshot.has_value());
    if (!decoded.snapshot) return;
    // Whole-section equality proves the new scroll fields survive the wire.
    ASSERT_EQ(decoded.snapshot->sections().tree, snapshot.sections().tree);
    auto const& p = decoded.snapshot->sections().tree.providers.front();
    ASSERT_EQ(p.firstVisible, std::uint32_t{3});
    ASSERT_EQ(p.scrollbar, ssg::Viewport{}.scrollbarMetrics(40, 9, 3));
    ASSERT_EQ(p.visibleNodeIds.size(), std::size_t{2});
    ASSERT_TRUE(decoded.snapshot->sections().shell.panelScrollbar.has_value());
    ASSERT_EQ(decoded.snapshot->sections().shell.panelScrollbar,
              snapshot.sections().shell.panelScrollbar);
    ASSERT_EQ(decoded.snapshot->sections().shell.tabHits,
              snapshot.sections().shell.tabHits);
}

// ---------------------------------------------------------------------------
// Clipboard and status-action message kinds.

TEST(commandResultRoundTripsThroughTheWire) {
    ssg::CommandResult const result{
        ssg::CommandError::StaleRevision, ssg::Revision{17},
        "base revision is stale"};
    auto const decoded =
        ssg::ProtocolCodec{}.decodeCommandResult(ssg::ProtocolCodec{}.encodeCommandResult(result));
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.result.has_value());
    ASSERT_EQ(decoded.result->error, result.error);
    ASSERT_EQ(decoded.result->revision, result.revision);
    ASSERT_EQ(decoded.result->message, result.message);
}


TEST(statusActionInvocationRoundTripsThroughTheWire) {
    ssg::StatusActionInvocation const invocation{ssg::StatusId{9}, "dismiss", 3};
    auto const bytes = ssg::ProtocolCodec{}.encodeStatusActionInvocation(invocation);
    auto const decoded = ssg::ProtocolCodec{}.decodeStatusActionInvocation(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.invocation.has_value());
    ASSERT_EQ(decoded.invocation->statusId, invocation.statusId);
    ASSERT_EQ(decoded.invocation->actionId, invocation.actionId);
    ASSERT_EQ(decoded.invocation->generation, invocation.generation);
}

// ---------------------------------------------------------------------------
// Binary-frame envelope: round trip plus decoded-byte-lifetime independence.

TEST(binaryFrameRoundTripsThroughTheWire) {
    ssg::BinaryFrame const frame{
        1, ssg::BinaryPayloadKind::DroppedContent, 99, {9, 8, 7, 6, 5}};
    auto const bytes = ssg::ProtocolCodec{}.encodeBinaryFrame(frame);
    auto const decoded = ssg::ProtocolCodec{}.decodeBinaryFrame(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.frame.has_value());
    ASSERT_EQ(*decoded.frame, frame);
}

TEST(binaryFrameDecodedBytesOutliveTheInputBuffer) {
    std::optional<ssg::BinaryFrame> survivingFrame;
    {
        std::string bytes = ssg::ProtocolCodec{}.encodeBinaryFrame(ssg::BinaryFrame{
            1, ssg::BinaryPayloadKind::DroppedContent, 7, {1, 2, 3, 4, 5, 6}});
        auto decoded = ssg::ProtocolCodec{}.decodeBinaryFrame(bytes);
        ASSERT_TRUE(decoded.accepted());
        survivingFrame = std::move(decoded.frame);
        // bytes (the input buffer) is destroyed at the end of this scope;
        // surviving_frame must not reference it.
        bytes.assign(bytes.size(), '\0');
    }
    ASSERT_TRUE(survivingFrame.has_value());
    ASSERT_EQ(survivingFrame->bytes,
             (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6}));
    ASSERT_EQ(survivingFrame->requestId, std::uint64_t{7});
}

TEST(binaryFrameRejectsAnUnsupportedPayloadKind) {
    std::string bytes = ssg::ProtocolCodec{}.encodeBinaryFrame(ssg::BinaryFrame{
        1, ssg::BinaryPayloadKind::DroppedContent, 1, {1}});
    bytes[1] = static_cast<char>(0xEE);
    auto const decoded = ssg::ProtocolCodec{}.decodeBinaryFrame(bytes);
    ASSERT_FALSE(decoded.accepted());
    ASSERT_EQ(decoded.error, ssg::ProtocolError::MalformedMessage);
}

TEST(binaryFrameRejectsOversizedDeclaredLengthAndFrame) {
    ssg::ProtocolLimits limits;
    limits.maxBinaryFrameBytes = 4;
    auto const bytes = ssg::ProtocolCodec{}.encodeBinaryFrame(ssg::BinaryFrame{
        1, ssg::BinaryPayloadKind::DroppedContent, 1, {1, 2, 3, 4, 5}});
    auto const decoded = ssg::ProtocolCodec{}.decodeBinaryFrame(bytes, limits);
    ASSERT_FALSE(decoded.accepted());
    ASSERT_EQ(decoded.error, ssg::ProtocolError::BinaryFrameTooLarge);
}

TEST(binaryFrameRejectsTruncatedInput) {
    auto bytes = ssg::ProtocolCodec{}.encodeBinaryFrame(ssg::BinaryFrame{
        1, ssg::BinaryPayloadKind::DroppedContent, 1, {1, 2, 3}});
    bytes.resize(bytes.size() - 1);
    auto const decoded = ssg::ProtocolCodec{}.decodeBinaryFrame(bytes);
    ASSERT_FALSE(decoded.accepted());
    ASSERT_EQ(decoded.error, ssg::ProtocolError::TruncatedMessage);
}

// ---------------------------------------------------------------------------
// Malformed / truncated / oversized / unknown-version / unknown-kind corpus,
// exercised against a representative message from each of the six kinds.

TEST(malformedAndTruncatedAndOversizedAndUnknownVersionCorpus) {
    ssg::StatusActionInvocation const invocation{ssg::StatusId{1}, "x", 1};
    auto const canonical =
        ssg::ProtocolCodec{}.encodeStatusActionInvocation(invocation);
    ASSERT_TRUE(canonical.size() > 3);

    // Empty buffer: missing version byte.
    {
        auto const decoded = ssg::ProtocolCodec{}.decodeStatusActionInvocation(std::string_view{});
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::TruncatedMessage);
    }

    // Single byte: missing kind byte.
    {
        auto const decoded =
            ssg::ProtocolCodec{}.decodeStatusActionInvocation(canonical.substr(0, 1));
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::TruncatedMessage);
    }

    // Wrong version byte.
    {
        auto corrupted = canonical;
        corrupted[0] = static_cast<char>(0xFF);
        auto const decoded = ssg::ProtocolCodec{}.decodeStatusActionInvocation(corrupted);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::UnsupportedVersion);
    }

    // Wrong kind byte (decoded with the wrong expected-kind decoder).
    {
        auto const decoded = ssg::ProtocolCodec{}.decodeSessionSnapshot(canonical);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::UnsupportedMessageKind);
    }

    // Oversized: buffer larger than the configured message-byte limit.
    {
        ssg::ProtocolLimits limits;
        limits.maxMessageBytes = canonical.size() - 1;
        auto const decoded = ssg::ProtocolCodec{}.decodeStatusActionInvocation(canonical, limits);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::MessageTooLarge);
    }

    // Truncated payload: valid header, body cut short.
    {
        auto const decoded = ssg::ProtocolCodec{}.decodeStatusActionInvocation(
            canonical.substr(0, canonical.size() - 2));
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::TruncatedMessage);
    }

    // Trailing garbage bytes appended after an otherwise-valid message.
    {
        auto padded = canonical;
        padded.push_back('\x7f');
        auto const decoded = ssg::ProtocolCodec{}.decodeStatusActionInvocation(padded);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::MalformedMessage);
    }
}

// Kinds 3 and 4 were clipboard_request/clipboard_response and are retired.  The
// ordinal is what goes on the wire, so their slots must stay dead rather than be
// reclaimed: a peer built against the old numbering must be told the kind is
// unsupported, never handed a message that now means something else.
TEST(retiredWireKindsAreNeverReclaimed) {
    ssg::StatusActionInvocation const invocation{ssg::StatusId{1}, "a", 1};
    auto const canonical =
        ssg::ProtocolCodec{}.encodeStatusActionInvocation(invocation);
    for (char const kind : {char{3}, char{4}}) {
        auto retired = canonical;
        retired[1] = kind;
        ASSERT_EQ(ssg::ProtocolCodec{}.decodeStatusActionInvocation(retired).error,
                  ssg::ProtocolError::UnsupportedMessageKind);
        ASSERT_EQ(ssg::ProtocolCodec{}.decodeSessionSnapshot(retired).error,
                  ssg::ProtocolError::UnsupportedMessageKind);
        ASSERT_EQ(ssg::ProtocolCodec{}.decodeCommandResult(retired).error,
                  ssg::ProtocolError::UnsupportedMessageKind);
    }
}

TEST(valueBoundsAreEnforcedOnDecode) {
    ssg::StatusActionInvocation const invocation{ssg::StatusId{1}, "a", 1};
    auto const bytes = ssg::ProtocolCodec{}.encodeStatusActionInvocation(invocation);

    {
        ssg::ProtocolLimits limits;
        limits.maxCollectionLength = 0;
        auto const decoded = ssg::ProtocolCodec{}.decodeStatusActionInvocation(bytes, limits);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::ValueBoundsExceeded);
    }
    {
        ssg::ProtocolLimits limits;
        limits.maxTextBytes = 0;
        auto const decoded = ssg::ProtocolCodec{}.decodeStatusActionInvocation(bytes, limits);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::ValueBoundsExceeded);
    }
    {
        ssg::ProtocolLimits limits;
        limits.maxValueDepth = 0;
        auto const decoded = ssg::ProtocolCodec{}.decodeStatusActionInvocation(bytes, limits);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::ValueBoundsExceeded);
    }
}

// ---------------------------------------------------------------------------
// Canonical fixtures: one hex-encoded golden wire message per kind, generated
// once against this codec (see protocol/schema/README.md). A fixture failing
// to decode, or decoding to different values than recorded here, signals an
// unintended wire-format change.

std::string readFixtureBytes(std::string const& name) {
    std::ifstream input{std::string{SSG_PROTOCOL_FIXTURES_DIR} + "/" + name};
    std::string hex{std::istreambuf_iterator<char>{input},
                    std::istreambuf_iterator<char>{}};
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r')) {
        hex.pop_back();
    }
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        bytes.push_back(static_cast<char>(
            std::stoul(hex.substr(index, 2), nullptr, 16)));
    }
    return bytes;
}

void writeFixtureHex(std::string const& name, std::string const& bytes) {
    std::string hex;
    hex.reserve(bytes.size() * 2);
    char const* digits = "0123456789abcdef";
    for (unsigned char byte : bytes) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0f]);
    }
    hex.push_back('\n');
    std::ofstream out{std::string{SSG_PROTOCOL_FIXTURES_DIR} + "/" + name};
    out << hex;
}

// Regenerate the canonical session_snapshot/session_delta wire goldens from the
// same objects the round-trip tests build.  Gated on SSG_REGEN_PROTOCOL_FIXTURES
// so a wire-format change (e.g. a new ViewportViewState field) can re-lock the
// goldens: `SSG_REGEN_PROTOCOL_FIXTURES=1 ./build/test_protocol`.
TEST(regenerateCanonicalFixtures) {
    if (std::getenv("SSG_REGEN_PROTOCOL_FIXTURES") == nullptr) return;
    auto snapshot = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{
            ssg::ClientId{7}, ssg::InvocationOrigin::InProcess,
            {ssg::CapabilityId{"local_file_drop"}}},
        ssg::ViewId{9}, clientView(3), sections(ssg::Revision{4}, "alpha"));
    writeFixtureHex("session_snapshot.hex",
                      ssg::ProtocolCodec{}.encodeSessionSnapshot(snapshot));

    auto before = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1), sections(ssg::Revision{4}, "a"));
    auto after = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{5}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(5), sections(ssg::Revision{5}, "changed"));
    writeFixtureHex("session_delta.hex",
                      ssg::ProtocolCodec{}.encodeSessionDelta(ssg::SessionSnapshotCodec{}.deriveDelta(before, after)));
}

TEST(canonicalFixturesDecodeToTheExpectedValues) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};

    {
        auto decoded = ssg::ProtocolCodec{}.decodeCommandRequest(
            readFixtureBytes("command_request_no_payload.hex"), registry);
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.command->id, std::string{"edit.undo"});
        ASSERT_EQ(decoded.command->baseRevision, ssg::Revision{3});
        ASSERT_FALSE(decoded.command->payload.has_value());
    }
    {
        auto decoded = ssg::ProtocolCodec{}.decodeCommandRequest(
            readFixtureBytes("command_request_text_input.hex"), registry);
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.command->id, std::string{"text.insert"});
        auto const* arguments =
            std::any_cast<ssg::TextInputArguments>(&decoded.command->payload);
        ASSERT_TRUE(arguments != nullptr);
        ASSERT_EQ(arguments->text, std::string{"hello"});
    }
    {
        auto decoded = ssg::ProtocolCodec{}.decodeCommandResult(
            readFixtureBytes("command_result.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.result->error,
                  ssg::CommandError::StaleRevision);
        ASSERT_EQ(decoded.result->revision, ssg::Revision{17});
        ASSERT_EQ(decoded.result->message,
                  std::string{"base revision is stale"});
    }
    {
        auto decoded = ssg::ProtocolCodec{}.decodeSessionSnapshot(
            readFixtureBytes("session_snapshot.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.snapshot->revision(), ssg::Revision{4});
        ASSERT_EQ(decoded.snapshot->client().clientId, ssg::ClientId{7});
        ASSERT_EQ(decoded.snapshot->client().capabilities.size(),
                 std::size_t{1});
    }
    {
        auto decoded =
            ssg::ProtocolCodec{}.decodeSessionDelta(readFixtureBytes("session_delta.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.delta->baseRevision(), ssg::Revision{4});
        ASSERT_EQ(decoded.delta->revision(), ssg::Revision{5});
        ASSERT_EQ(decoded.delta->clientId(), ssg::ClientId{7});
    }
    {
        auto decoded = ssg::ProtocolCodec{}.decodeStatusActionInvocation(
            readFixtureBytes("status_action_invocation.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.invocation->statusId, ssg::StatusId{9});
        ASSERT_EQ(decoded.invocation->actionId, std::string{"dismiss"});
    }
    {
        auto decoded =
            ssg::ProtocolCodec{}.decodeBinaryFrame(readFixtureBytes("binary_frame.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.frame->requestId, std::uint64_t{99});
        ASSERT_EQ(decoded.frame->bytes,
                 (std::vector<std::uint8_t>{9, 8, 7, 6, 5}));
    }
}

}  // namespace

TEST(viewportFirstVisualColumnSurvivesTheWire) {
    // VP-H (M12): the horizontal scroll offset is a wire field and must round-trip.
    auto view = clientView(3);
    view.firstVisualColumn = 7;
    auto snapshot = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, view, sections(ssg::Revision{4}, "alpha"));
    auto const decoded =
        ssg::ProtocolCodec{}.decodeSessionSnapshot(ssg::ProtocolCodec{}.encodeSessionSnapshot(snapshot));
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.snapshot.has_value());
    if (!decoded.snapshot) return;
    ASSERT_EQ(decoded.snapshot->client().viewport.firstVisualColumn,
              std::uint32_t{7});
    ASSERT_EQ(*decoded.snapshot, snapshot);
}

TEST(documentIdentitySurvivesSessionSnapshotAndDeltaWireRoundTrips) {
    auto withIdentity = [](ssg::Revision revision, std::string marker,
                           std::optional<std::string> identity) {
        auto s = sections(revision, std::move(marker));
        s.document.diffFileIdentity = std::move(identity);
        return s;
    };

    auto snapshot = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(3),
        withIdentity(ssg::Revision{4}, "alpha", std::string{"src/b.cpp"}));
    auto decodedSnapshot =
        ssg::ProtocolCodec{}.decodeSessionSnapshot(ssg::ProtocolCodec{}.encodeSessionSnapshot(snapshot));
    ASSERT_TRUE(decodedSnapshot.accepted());
    ASSERT_TRUE(decodedSnapshot.snapshot.has_value());
    ASSERT_EQ(decodedSnapshot.snapshot->sections().document.diffFileIdentity,
              std::optional<std::string>{"src/b.cpp"});

    auto before = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1),
        withIdentity(ssg::Revision{4}, "same", std::string{"src/a.cpp"}));
    auto after = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1),
        withIdentity(ssg::Revision{4}, "same", std::string{"src/b.cpp"}));
    auto delta = ssg::SessionSnapshotCodec{}.deriveDelta(before, after);
    auto decodedDelta =
        ssg::ProtocolCodec{}.decodeSessionDelta(ssg::ProtocolCodec{}.encodeSessionDelta(delta));
    ASSERT_TRUE(decodedDelta.accepted());
    ASSERT_TRUE(decodedDelta.delta.has_value());
    auto replayed = ssg::SessionSnapshotCodec{}.replay(before, *decodedDelta.delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_TRUE(replayed.snapshot.has_value());
    ASSERT_EQ(replayed.snapshot->sections().document.diffFileIdentity,
              std::optional<std::string>{"src/b.cpp"});
}

TEST(commandRequestRoundTripsWithReplaceReplacementArguments) {
    auto const registry = ssg::CommandArgumentCodecRegistry{staticTableCatalog()};
    ssg::ClientCommand const command{
        "replace.update_replacement", ssg::Revision{9},
        ssg::FindQueryArguments{"dog"}};
    auto const bytes = ssg::ProtocolCodec{}.encodeCommandRequest(command, registry);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.command->id, command.id);
    auto const* arguments =
        std::any_cast<ssg::FindQueryArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(*arguments,
              std::any_cast<ssg::FindQueryArguments>(command.payload));
}

TEST(findReplaceViewStateRoundTripsReplacementThroughTheWire) {
    // A snapshot carrying a non-empty replacement must preserve it through the
    // snapshot codec and a delta replay (F2a).
    auto withReplacement = [](ssg::Revision revision, std::string marker,
                               std::string replacement) {
        auto s = sections(revision, std::move(marker));
        s.findReplace.replacement = std::move(replacement);
        return s;
    };
    auto snapshot = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(3),
        withReplacement(ssg::Revision{4}, "alpha", "dog"));
    auto const decoded =
        ssg::ProtocolCodec{}.decodeSessionSnapshot(ssg::ProtocolCodec{}.encodeSessionSnapshot(snapshot));
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.snapshot.has_value());
    if (decoded.snapshot) {
        ASSERT_EQ(decoded.snapshot->sections().findReplace.replacement,
                  std::string{"dog"});
    }

    auto before = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1),
        withReplacement(ssg::Revision{4}, "a", ""));
    auto after = ssg::SessionSnapshotCodec{}.assemble(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::InProcess},
        ssg::ViewId{9}, clientView(1),
        withReplacement(ssg::Revision{4}, "a", "dog"));
    auto const delta = ssg::SessionSnapshotCodec{}.deriveDelta(before, after);
    auto decodedDelta =
        ssg::ProtocolCodec{}.decodeSessionDelta(ssg::ProtocolCodec{}.encodeSessionDelta(delta));
    ASSERT_TRUE(decodedDelta.accepted());
    ASSERT_TRUE(decodedDelta.delta.has_value());
    auto replayed = ssg::SessionSnapshotCodec{}.replay(before, *decodedDelta.delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_TRUE(replayed.snapshot.has_value());
    if (replayed.snapshot) {
        ASSERT_EQ(replayed.snapshot->sections().findReplace.replacement,
                  std::string{"dog"});
    }
}

// style.define and the wire codec each hand-maintain the Style field-name list,
// in different files.  A field added to one but not the other means a field
// settable over the wire but not by style.define (or vice versa).  This asserts
// the two lists are exactly equal, deriving the codec's names from the codec.
TEST(styleDefineKeysExactlyMatchTheWireCodecFields) {
    auto defineKeys = ssg::styleDefineKeys();
    auto wireKeys = ssg::styleWireFieldNames();
    std::sort(defineKeys.begin(), defineKeys.end());
    std::sort(wireKeys.begin(), wireKeys.end());
    ASSERT_EQ(defineKeys, wireKeys);
}

int main() {
    RUN(registryCoversEveryP0CommandAndRejectsUnknownIds);
    RUN(everySettingKeyRoundTripsThroughTheCommandCodec);
    RUN(commandRequestRoundTripsWithNoPayload);
    RUN(commandRequestRoundTripsWithPaletteExecuteArguments);
    RUN(commandRequestRoundTripsWithFindQueryArguments);
    RUN(commandRequestRoundTripsWithPromptValueArguments);
    RUN(commandRequestRoundTripsWithTreeScrollToFraction);
    RUN(commandRequestRoundTripsWithTreeSelectArguments);
    RUN(viewportFirstVisualColumnSurvivesTheWire);
    RUN(documentIdentitySurvivesSessionSnapshotAndDeltaWireRoundTrips);
    RUN(commandRequestRoundTripsWithReplaceReplacementArguments);
    RUN(findReplaceViewStateRoundTripsReplacementThroughTheWire);
    RUN(commandRequestRoundTripsWithTextInputArguments);
    RUN(commandRequestRoundTripsWithSelectionCommandArguments);
    RUN(commandRequestRoundTripsWithSelectionsListSelectionCommandArguments);
    RUN(commandRequestRoundTripsWithEmptySelectionCommandArguments);
    RUN(commandRequestRoundTripsWithScrollLinesArguments);
    RUN(commandRequestRoundTripsWithScrollPagesArguments);
    RUN(commandRequestRoundTripsWithScrollFractionArguments);
    RUN(commandRequestRoundTripsWithDroppedContentArguments);
    RUN(decodeCommandRequestRejectsUnknownCommandId);
    RUN(decodeCommandRequestRejectsMalformedPayload);
    RUN(decodeCommandRequestMapsDomainInvariantFailuresToMalformed);
    RUN(sessionSnapshotRoundTripsThroughTheWire);
    RUN(sessionSnapshotRoundTripsANonDefaultStyle);
    RUN(styleDefineKeysExactlyMatchTheWireCodecFields);
    RUN(sessionDeltaRoundTripsAndReplayMatchesTheDecodedDelta);
    RUN(phantomViewportProjectionRoundTripsThroughSnapshotAndDelta);
    RUN(diffWordRangesRoundTripThroughSnapshotAndDelta);
    RUN(twoClientCapabilityAndViewportIsolationSurvivesTheWire);
    RUN(sessionSnapshotRoundTripsTreeScrollFields);
    RUN(commandResultRoundTripsThroughTheWire);
    RUN(statusActionInvocationRoundTripsThroughTheWire);
    RUN(binaryFrameRoundTripsThroughTheWire);
    RUN(binaryFrameDecodedBytesOutliveTheInputBuffer);
    RUN(binaryFrameRejectsAnUnsupportedPayloadKind);
    RUN(binaryFrameRejectsOversizedDeclaredLengthAndFrame);
    RUN(binaryFrameRejectsTruncatedInput);
    RUN(malformedAndTruncatedAndOversizedAndUnknownVersionCorpus);
    RUN(retiredWireKindsAreNeverReclaimed);
    RUN(valueBoundsAreEnforcedOnDecode);
    RUN(regenerateCanonicalFixtures);
    RUN(canonicalFixturesDecodeToTheExpectedValues);
    return failed == 0 ? 0 : 1;
}
