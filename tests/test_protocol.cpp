#include "test_helpers.h"

#include <ssg/editor_session_builder.h>
#include <ssg/file_commands.h>
#include <ssg/find_replace.h>
#include <ssg/protocol.h>
#include <ssg/session_snapshot.h>

#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef SSG_REQUIRED_COMMANDS_PATH
#error "SSG_REQUIRED_COMMANDS_PATH must name the accepted catalog"
#endif

#ifndef SSG_PROTOCOL_FIXTURES_DIR
#error "SSG_PROTOCOL_FIXTURES_DIR must name the fixtures directory"
#endif

namespace {

// ---------------------------------------------------------------------------
// Fixture builders. Mirrors tests/test_editor_session_assembly.cpp's
// SessionSnapshotSections fixture so this file exercises the same complete,
// every-section-populated snapshot shape through the wire codec.

std::vector<std::string> catalogIds() {
    std::ifstream input{SSG_REQUIRED_COMMANDS_PATH};
    std::string json{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
    std::regex const idPattern{R"json("id"\s*:\s*"([^"]+)")json"};
    std::vector<std::string> ids;
    for (std::sregex_iterator it{json.begin(), json.end(), idPattern}, end;
         it != end; ++it) {
        ids.push_back((*it)[1].str());
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
    theme.palette[0].red = static_cast<std::uint8_t>(marker.size());

    ssg::ShellViewState shell;
    shell.viewport = {static_cast<int>(20 + marker.size()), 8};

    return {
        {revision, marker, ssg::ByteOffset{marker.size()}},
        selection(marker.size(), static_cast<std::uint32_t>(marker.size())),
        {true, false, marker.size()},
        {{marker}, marker, std::nullopt, std::nullopt},
        {std::nullopt, {{}, marker.size()}},
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
            {firstRow + 8, 8, firstRow, firstRow, 0, 8}};
}

// ---------------------------------------------------------------------------
// Registry coverage and construction validation.

TEST(registryCoversEveryP0CommandAndRejectsUnknownIds) {
    auto registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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

TEST(registryRejectsMissingEntries) {
    std::vector<std::pair<std::string, ssg::CommandArgumentCodec>> entries;
    auto const ids = ssg::p0CommandDescriptors();
    for (std::size_t index = 1; index < ids.size(); ++index) {
        entries.emplace_back(ids[index].id, makeProbeCodec());
    }
    ASSERT_THROWS(ssg::CommandArgumentCodecRegistry{std::move(entries)},
                 std::invalid_argument);
}

TEST(registryRejectsExtraEntries) {
    std::vector<std::pair<std::string, ssg::CommandArgumentCodec>> entries;
    for (auto const& descriptor : ssg::p0CommandDescriptors()) {
        entries.emplace_back(descriptor.id, makeProbeCodec());
    }
    entries.emplace_back("not.p0", makeProbeCodec());
    ASSERT_THROWS(ssg::CommandArgumentCodecRegistry{std::move(entries)},
                 std::invalid_argument);
}

TEST(registryRejectsDuplicateEntries) {
    std::vector<std::pair<std::string, ssg::CommandArgumentCodec>> entries;
    auto const ids = ssg::p0CommandDescriptors();
    for (auto const& descriptor : ids) {
        entries.emplace_back(descriptor.id, makeProbeCodec());
    }
    entries.emplace_back(ids.front().id, makeProbeCodec());
    ASSERT_THROWS(ssg::CommandArgumentCodecRegistry{std::move(entries)},
                 std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Command request round trips: one canonical fixture per argument shape.

TEST(commandRequestRoundTripsWithPaletteExecuteArguments) {
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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


TEST(commandRequestRoundTripsWithTreeSelectArguments) {
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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

TEST(commandRequestRoundTripsWithScrollLinesArguments) {
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
    ASSERT_THROWS(registry.encodeArgument("not.a.command", std::any{}),
                 std::invalid_argument);

    auto const bytes = buildCommandRequestMessage("not.a.command", 1);
    auto const decoded = ssg::ProtocolCodec{}.decodeCommandRequest(bytes, registry);
    ASSERT_FALSE(decoded.accepted());
    ASSERT_EQ(decoded.error, ssg::ProtocolError::UnsupportedCommand);
}

TEST(decodeCommandRequestRejectsMalformedPayload) {
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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

TEST(clipboardRequestRoundTripsThroughTheWire) {
    ssg::ClipboardRequest const request{
        42, ssg::ClipboardRequestKind::Write, ssg::Revision{6}, "copied text"};
    auto const bytes = ssg::ProtocolCodec{}.encodeClipboardRequest(request);
    auto const decoded = ssg::ProtocolCodec{}.decodeClipboardRequest(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.request.has_value());
    ASSERT_EQ(*decoded.request, request);
}

TEST(clipboardResponseRoundTripsThroughTheWire) {
    ssg::ClipboardResponse const response{
        42, ssg::Revision{6}, ssg::Revision{7},
        ssg::ClipboardResponseStatus::Success, "pasted text"};
    auto const bytes = ssg::ProtocolCodec{}.encodeClipboardResponse(response);
    auto const decoded = ssg::ProtocolCodec{}.decodeClipboardResponse(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.response.has_value());
    ASSERT_EQ(*decoded.response, response);
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
    ssg::ClipboardRequest const request{
        1, ssg::ClipboardRequestKind::Read, ssg::Revision{1}, "x"};
    auto const canonical = ssg::ProtocolCodec{}.encodeClipboardRequest(request);
    ASSERT_TRUE(canonical.size() > 3);

    // Empty buffer: missing version byte.
    {
        auto const decoded = ssg::ProtocolCodec{}.decodeClipboardRequest(std::string_view{});
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::TruncatedMessage);
    }

    // Single byte: missing kind byte.
    {
        auto const decoded =
            ssg::ProtocolCodec{}.decodeClipboardRequest(canonical.substr(0, 1));
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::TruncatedMessage);
    }

    // Wrong version byte.
    {
        auto corrupted = canonical;
        corrupted[0] = static_cast<char>(0xFF);
        auto const decoded = ssg::ProtocolCodec{}.decodeClipboardRequest(corrupted);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::UnsupportedVersion);
    }

    // Wrong kind byte (decoded with the wrong expected-kind decoder).
    {
        auto const decoded = ssg::ProtocolCodec{}.decodeClipboardResponse(canonical);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::UnsupportedMessageKind);
    }

    // Oversized: buffer larger than the configured message-byte limit.
    {
        ssg::ProtocolLimits limits;
        limits.maxMessageBytes = canonical.size() - 1;
        auto const decoded = ssg::ProtocolCodec{}.decodeClipboardRequest(canonical, limits);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::MessageTooLarge);
    }

    // Truncated payload: valid header, body cut short.
    {
        auto const decoded = ssg::ProtocolCodec{}.decodeClipboardRequest(
            canonical.substr(0, canonical.size() - 2));
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::TruncatedMessage);
    }

    // Trailing garbage bytes appended after an otherwise-valid message.
    {
        auto padded = canonical;
        padded.push_back('\x7f');
        auto const decoded = ssg::ProtocolCodec{}.decodeClipboardRequest(padded);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::MalformedMessage);
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
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();

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
        auto decoded = ssg::ProtocolCodec{}.decodeClipboardRequest(
            readFixtureBytes("clipboard_request.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.request->id, std::uint64_t{42});
        ASSERT_EQ(decoded.request->text, std::string{"copied text"});
    }
    {
        auto decoded = ssg::ProtocolCodec{}.decodeClipboardResponse(
            readFixtureBytes("clipboard_response.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.response->id, std::uint64_t{42});
        ASSERT_EQ(decoded.response->text, std::string{"pasted text"});
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

TEST(commandRequestRoundTripsWithReplaceReplacementArguments) {
    auto const registry = ssg::ProtocolCodec{}.buildCommandArgumentCodecRegistry();
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

int main() {
    RUN(registryCoversEveryP0CommandAndRejectsUnknownIds);
    RUN(registryRejectsMissingEntries);
    RUN(registryRejectsExtraEntries);
    RUN(registryRejectsDuplicateEntries);
    RUN(commandRequestRoundTripsWithNoPayload);
    RUN(commandRequestRoundTripsWithPaletteExecuteArguments);
    RUN(commandRequestRoundTripsWithTreeSelectArguments);
    RUN(viewportFirstVisualColumnSurvivesTheWire);
    RUN(commandRequestRoundTripsWithReplaceReplacementArguments);
    RUN(findReplaceViewStateRoundTripsReplacementThroughTheWire);
    RUN(commandRequestRoundTripsWithTextInputArguments);
    RUN(commandRequestRoundTripsWithSelectionCommandArguments);
    RUN(commandRequestRoundTripsWithEmptySelectionCommandArguments);
    RUN(commandRequestRoundTripsWithScrollLinesArguments);
    RUN(commandRequestRoundTripsWithScrollPagesArguments);
    RUN(commandRequestRoundTripsWithScrollFractionArguments);
    RUN(commandRequestRoundTripsWithDroppedContentArguments);
    RUN(decodeCommandRequestRejectsUnknownCommandId);
    RUN(decodeCommandRequestRejectsMalformedPayload);
    RUN(decodeCommandRequestMapsDomainInvariantFailuresToMalformed);
    RUN(sessionSnapshotRoundTripsThroughTheWire);
    RUN(sessionDeltaRoundTripsAndReplayMatchesTheDecodedDelta);
    RUN(twoClientCapabilityAndViewportIsolationSurvivesTheWire);
    RUN(sessionSnapshotRoundTripsTreeScrollFields);
    RUN(clipboardRequestRoundTripsThroughTheWire);
    RUN(commandResultRoundTripsThroughTheWire);
    RUN(clipboardResponseRoundTripsThroughTheWire);
    RUN(statusActionInvocationRoundTripsThroughTheWire);
    RUN(binaryFrameRoundTripsThroughTheWire);
    RUN(binaryFrameDecodedBytesOutliveTheInputBuffer);
    RUN(binaryFrameRejectsAnUnsupportedPayloadKind);
    RUN(binaryFrameRejectsOversizedDeclaredLengthAndFrame);
    RUN(binaryFrameRejectsTruncatedInput);
    RUN(malformedAndTruncatedAndOversizedAndUnknownVersionCorpus);
    RUN(valueBoundsAreEnforcedOnDecode);
    RUN(regenerateCanonicalFixtures);
    RUN(canonicalFixturesDecodeToTheExpectedValues);
    return failed == 0 ? 0 : 1;
}
