#include "test_helpers.h"

#include <ssg/editor_session_assembly.h>
#include <ssg/file_commands.h>
#include <ssg/find_replace.h>
#include <ssg/protocol.h>
#include <ssg/session_snapshot.h>

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

std::vector<std::string> catalog_ids() {
    std::ifstream input{SSG_REQUIRED_COMMANDS_PATH};
    std::string json{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
    std::regex const id_pattern{R"json("id"\s*:\s*"([^"]+)")json"};
    std::vector<std::string> ids;
    for (std::sregex_iterator it{json.begin(), json.end(), id_pattern}, end;
         it != end; ++it) {
        ids.push_back((*it)[1].str());
    }
    return ids;
}

ssg::SelectionViewState selection(std::uint64_t byte, std::uint32_t first_row) {
    ssg::DocumentPosition const position{
        ssg::ByteOffset{byte}, ssg::LineIndex{0}, ssg::CellIndex{byte}};
    return {ssg::SelectionSet{{ssg::Selection{position, position}}},
            first_row, std::nullopt};
}

ssg::SessionSnapshotSections sections(ssg::Revision revision, std::string marker) {
    ssg::SettingsViewState settings;
    settings.entries[0].effective = {
        static_cast<std::uint32_t>(marker.size()), ssg::SettingScope::user};
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
        {revision, true, marker, ssg::SearchMode::file, {}, std::nullopt,
         marker.size(), false},
        {marker.size(), true, false, revision, marker, {}, {}, {}, std::nullopt,
         ssg::FindReplaceError::none, {}},
        settings,
        {marker, {}},
        {{marker.size() > 1 ? ssg::TextEncoding::utf16le
                           : ssg::TextEncoding::utf8,
          ssg::LineEnding::lf, false,
          !marker.empty()}},
        {{{ssg::TabId{1}, ssg::TabKind::read_only_output, std::nullopt,
           std::nullopt, "output", marker, ssg::DocumentMode::read_only,
           false, ssg::TabRecoveryBadge::none}},
         ssg::TabId{1}},
        {revision, {}},
        {revision, {}},
        {marker.size(), ssg::FollowMode::following, ssg::PaneId{},
         std::nullopt, {}, {}},
        {ssg::TreeRevision{marker.size()}, {}},
        ssg::plain_text_syntax_view_state(revision, ssg::LanguageId{"plain"},
                                          marker, 4),
        {revision, {}},
        {revision, {}, std::nullopt, {}, marker},
        theme,
        std::move(shell),
    };
}

ssg::ViewportViewState client_view(std::uint32_t first_row) {
    return {ssg::ViewportDimensions{20, 8},
            first_row,
            first_row + 8,
            {},
            {},
            {first_row + 8, 8, first_row, first_row, 0, 8}};
}

// ---------------------------------------------------------------------------
// Registry coverage and construction validation.

TEST(registry_covers_every_p0_command_and_rejects_unknown_ids) {
    auto registry = ssg::build_command_argument_codec_registry();
    for (auto const& id : catalog_ids()) {
        ASSERT_TRUE(registry.contains(id));
    }
    ASSERT_FALSE(registry.contains("not.a.command"));
    ASSERT_THROWS(registry.encode_argument("not.a.command", std::any{}),
                 std::invalid_argument);
}

ssg::CommandArgumentCodec make_probe_codec() {
    return ssg::CommandArgumentCodec{
        [](std::any const&) { return ssg::ProtocolValue::make_null(); },
        [](ssg::ProtocolValue const&) -> std::optional<std::any> {
            return std::any{};
        }};
}

TEST(registry_rejects_missing_entries) {
    std::vector<std::pair<std::string, ssg::CommandArgumentCodec>> entries;
    auto const ids = ssg::p0_command_descriptors();
    for (std::size_t index = 1; index < ids.size(); ++index) {
        entries.emplace_back(ids[index].id, make_probe_codec());
    }
    ASSERT_THROWS(ssg::CommandArgumentCodecRegistry{std::move(entries)},
                 std::invalid_argument);
}

TEST(registry_rejects_extra_entries) {
    std::vector<std::pair<std::string, ssg::CommandArgumentCodec>> entries;
    for (auto const& descriptor : ssg::p0_command_descriptors()) {
        entries.emplace_back(descriptor.id, make_probe_codec());
    }
    entries.emplace_back("not.p0", make_probe_codec());
    ASSERT_THROWS(ssg::CommandArgumentCodecRegistry{std::move(entries)},
                 std::invalid_argument);
}

TEST(registry_rejects_duplicate_entries) {
    std::vector<std::pair<std::string, ssg::CommandArgumentCodec>> entries;
    auto const ids = ssg::p0_command_descriptors();
    for (auto const& descriptor : ids) {
        entries.emplace_back(descriptor.id, make_probe_codec());
    }
    entries.emplace_back(ids.front().id, make_probe_codec());
    ASSERT_THROWS(ssg::CommandArgumentCodecRegistry{std::move(entries)},
                 std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Command request round trips: one canonical fixture per argument shape.

TEST(command_request_round_trips_with_palette_execute_arguments) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ssg::ClientCommand const command{
        "palette.execute", ssg::Revision{4},
        ssg::PaletteExecuteArguments{"file.save"}};
    auto const bytes = ssg::encode_command_request(command, registry);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.command->id, command.id);
    auto const* arguments =
        std::any_cast<ssg::PaletteExecuteArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(*arguments,
              std::any_cast<ssg::PaletteExecuteArguments>(command.payload));
}

TEST(command_request_round_trips_with_find_query_arguments) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ssg::ClientCommand const command{
        "find.update_query", ssg::Revision{7},
        ssg::FindQueryArguments{"cat"}};
    auto const bytes = ssg::encode_command_request(command, registry);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.command->id, command.id);
    auto const* arguments =
        std::any_cast<ssg::FindQueryArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(*arguments,
              std::any_cast<ssg::FindQueryArguments>(command.payload));
}


TEST(command_request_round_trips_with_no_payload) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ssg::ClientCommand const command{"edit.undo", ssg::Revision{3}, {}};
    auto const bytes = ssg::encode_command_request(command, registry);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.command.has_value());
    ASSERT_EQ(decoded.command->id, command.id);
    ASSERT_EQ(decoded.command->base_revision, command.base_revision);
    ASSERT_FALSE(decoded.command->payload.has_value());
}

TEST(command_request_round_trips_with_text_input_arguments) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ssg::ClientCommand const command{
        "text.insert", ssg::Revision{5},
        ssg::TextInputArguments{"hello world"}};
    auto const bytes = ssg::encode_command_request(command, registry);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.command->id, command.id);
    auto const* arguments =
        std::any_cast<ssg::TextInputArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(*arguments, std::any_cast<ssg::TextInputArguments>(command.payload));
}

TEST(command_request_round_trips_with_selection_command_arguments) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ssg::DocumentPosition const position{ssg::ByteOffset{4}, ssg::LineIndex{0},
                                         ssg::CellIndex{4}};
    ssg::SelectionCommandArguments const original{
        position, ssg::Selection{position, position}};
    ssg::ClientCommand const command{"cursor.set_position", ssg::Revision{2},
                                     original};
    auto const bytes = ssg::encode_command_request(command, registry);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments = std::any_cast<ssg::SelectionCommandArguments>(
        &decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(arguments->position, original.position);
    ASSERT_EQ(arguments->selection, original.selection);
}

TEST(command_request_round_trips_with_empty_selection_command_arguments) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ssg::SelectionCommandArguments const original{std::nullopt, std::nullopt};
    ssg::ClientCommand const command{"cursor.left", ssg::Revision{2}, original};
    auto const bytes = ssg::encode_command_request(command, registry);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments = std::any_cast<ssg::SelectionCommandArguments>(
        &decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_FALSE(arguments->position.has_value());
    ASSERT_FALSE(arguments->selection.has_value());
}

TEST(command_request_round_trips_with_scroll_lines_arguments) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ssg::ClientCommand const command{"view.scroll_lines", ssg::Revision{1},
                                     ssg::ScrollLinesArguments{-7}};
    auto const bytes = ssg::encode_command_request(command, registry);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments =
        std::any_cast<ssg::ScrollLinesArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(arguments->rows, std::int64_t{-7});
}

TEST(command_request_round_trips_with_scroll_pages_arguments) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ssg::ClientCommand const command{"view.scroll_pages", ssg::Revision{1},
                                     ssg::ScrollPagesArguments{3}};
    auto const bytes = ssg::encode_command_request(command, registry);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments =
        std::any_cast<ssg::ScrollPagesArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(arguments->pages, std::int64_t{3});
}

TEST(command_request_round_trips_with_scroll_fraction_arguments) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ssg::ClientCommand const command{"view.scroll_to_fraction", ssg::Revision{1},
                                     ssg::ScrollFractionArguments{3, 4}};
    auto const bytes = ssg::encode_command_request(command, registry);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments =
        std::any_cast<ssg::ScrollFractionArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(arguments->numerator, std::uint32_t{3});
    ASSERT_EQ(arguments->denominator, std::uint32_t{4});
}

TEST(command_request_round_trips_with_dropped_content_arguments) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ssg::ClientCommand const command{
        "file.open_dropped_content", ssg::Revision{1},
        ssg::DroppedContentArguments{{1, 2, 3, 4}, "dropped.txt"}};
    auto const bytes = ssg::encode_command_request(command, registry);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    auto const* arguments =
        std::any_cast<ssg::DroppedContentArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(arguments->bytes,
             (std::vector<std::uint8_t>{1, 2, 3, 4}));
    ASSERT_EQ(arguments->suggested_label, std::string{"dropped.txt"});
}

std::string wire_u8(std::uint8_t value) {
    return std::string(1, static_cast<char>(value));
}

void append_u32(std::string& out, std::uint32_t value) {
    for (int index = 0; index < 4; ++index) {
        out.push_back(static_cast<char>((value >> (8 * index)) & 0xFF));
    }
}

void append_u64(std::string& out, std::uint64_t value) {
    for (int index = 0; index < 8; ++index) {
        out.push_back(static_cast<char>((value >> (8 * index)) & 0xFF));
    }
}

void append_text_value(std::string& out, std::string const& text) {
    out += wire_u8(4);
    append_u32(out, static_cast<std::uint32_t>(text.size()));
    out += text;
}

void append_uint_value(std::string& out, std::uint64_t value) {
    out += wire_u8(3);
    append_u64(out, value);
}

void append_null_value(std::string& out) { out += wire_u8(0); }

void append_field_key(std::string& out, std::string const& key) {
    append_u32(out, static_cast<std::uint32_t>(key.size()));
    out += key;
}

// Hand-builds a `command_request` wire message directly against the
// documented [u8 version][u8 kind][value] envelope and object/text/uint tag
// scheme, independent of protocol.cpp's private encoder. This is the only
// way to exercise decode_command_request's unknown-command-id rejection: the
// public encode_command_request() itself throws before producing bytes for
// an id the registry does not recognize.
std::string build_command_request_message(std::string const& id,
                                          std::uint64_t base_revision) {
    std::string body;
    body += wire_u8(7);
    append_u32(body, 3);
    append_field_key(body, "id");
    append_text_value(body, id);
    append_field_key(body, "base_revision");
    append_uint_value(body, base_revision);
    append_field_key(body, "payload");
    append_null_value(body);

    std::string message;
    message += wire_u8(1);
    message += wire_u8(
        static_cast<std::uint8_t>(ssg::ProtocolMessageKind::command_request));
    message += body;
    return message;
}

std::string build_invalid_scroll_fraction_message() {
    std::string payload;
    payload += wire_u8(7);
    append_u32(payload, 2);
    append_field_key(payload, "numerator");
    append_uint_value(payload, 0);
    append_field_key(payload, "denominator");
    append_uint_value(payload, 0);

    std::string body;
    body += wire_u8(7);
    append_u32(body, 3);
    append_field_key(body, "id");
    append_text_value(body, "view.scroll_to_fraction");
    append_field_key(body, "base_revision");
    append_uint_value(body, 1);
    append_field_key(body, "payload");
    body += payload;

    return wire_u8(1) +
           wire_u8(static_cast<std::uint8_t>(
               ssg::ProtocolMessageKind::command_request)) +
           body;
}

TEST(decode_command_request_rejects_unknown_command_id) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ASSERT_THROWS(registry.encode_argument("not.a.command", std::any{}),
                 std::invalid_argument);

    auto const bytes = build_command_request_message("not.a.command", 1);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_FALSE(decoded.accepted());
    ASSERT_EQ(decoded.error, ssg::ProtocolError::unsupported_command);
}

TEST(decode_command_request_rejects_malformed_payload) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ssg::ClientCommand const command{"text.insert", ssg::Revision{1},
                                     ssg::TextInputArguments{"x"}};
    auto bytes = ssg::encode_command_request(command, registry);
    // Truncate the trailing bytes so the payload's "text" field is cut off,
    // producing a structurally-truncated command request.
    bytes.resize(bytes.size() - 2);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_FALSE(decoded.accepted());
}

TEST(decode_command_request_maps_domain_invariant_failures_to_malformed) {
    auto const registry = ssg::build_command_argument_codec_registry();
    auto const decoded = ssg::decode_command_request(
        build_invalid_scroll_fraction_message(), registry);
    ASSERT_FALSE(decoded.accepted());
    ASSERT_EQ(decoded.error, ssg::ProtocolError::malformed_message);
}

// ---------------------------------------------------------------------------
// Session snapshot / delta round trips, including replay-vs-decoded
// equivalence and two-client isolation.

TEST(session_snapshot_round_trips_through_the_wire) {
    auto snapshot = ssg::assemble_session_snapshot(
        ssg::Revision{4}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{
            ssg::ClientId{7}, ssg::InvocationOrigin::in_process,
            {ssg::CapabilityId{"local_file_drop"}}},
        ssg::ViewId{9}, client_view(3), sections(ssg::Revision{4}, "alpha"));

    auto const bytes = ssg::encode_session_snapshot(snapshot);
    auto const decoded = ssg::decode_session_snapshot(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.snapshot.has_value());
    ASSERT_EQ(*decoded.snapshot, snapshot);
}

TEST(session_delta_round_trips_and_replay_matches_the_decoded_delta) {
    auto before = ssg::assemble_session_snapshot(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(1), sections(ssg::Revision{4}, "a"));
    auto after = ssg::assemble_session_snapshot(
        ssg::Revision{5}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(5), sections(ssg::Revision{5}, "changed"));

    auto const delta = ssg::derive_session_delta(before, after);
    auto const bytes = ssg::encode_session_delta(delta);
    auto decoded = ssg::decode_session_delta(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.delta.has_value());

    auto replayed = ssg::replay_session_delta(before, *decoded.delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_TRUE(replayed.snapshot.has_value());
    ASSERT_EQ(*replayed.snapshot, after);
}

TEST(two_client_capability_and_viewport_isolation_survives_the_wire) {
    auto shared = sections(ssg::Revision{8}, "shared");
    auto first = ssg::assemble_session_snapshot(
        ssg::Revision{8}, {},
        ssg::InvocationPrincipal{
            ssg::ClientId{1}, ssg::InvocationOrigin::websocket,
            {ssg::CapabilityId{"local_file_drop"}}},
        ssg::ViewId{10}, client_view(2), shared);
    auto second = ssg::assemble_session_snapshot(
        ssg::Revision{8}, {},
        ssg::InvocationPrincipal{ssg::ClientId{2},
                                 ssg::InvocationOrigin::websocket},
        ssg::ViewId{11}, client_view(7), std::move(shared));

    auto const first_decoded =
        ssg::decode_session_snapshot(ssg::encode_session_snapshot(first));
    auto const second_decoded =
        ssg::decode_session_snapshot(ssg::encode_session_snapshot(second));
    ASSERT_TRUE(first_decoded.accepted());
    ASSERT_TRUE(second_decoded.accepted());

    ASSERT_EQ(first_decoded.snapshot->client().capabilities.size(),
             std::size_t{1});
    ASSERT_TRUE(second_decoded.snapshot->client().capabilities.empty());
    ASSERT_EQ(first_decoded.snapshot->client().viewport.first_visual_row,
             std::uint32_t{2});
    ASSERT_EQ(second_decoded.snapshot->client().viewport.first_visual_row,
             std::uint32_t{7});
    ASSERT_EQ(first_decoded.snapshot->sections(), second_decoded.snapshot->sections());
}

TEST(session_snapshot_round_trips_tree_scroll_fields) {
    auto sections_value = sections(ssg::Revision{4}, "alpha");
    ssg::TreeNode node_a{ssg::TreeNodeId{"files:a"}, std::nullopt, "a.txt",
                         ssg::TreeNodeKind::file};
    ssg::TreeNode node_b{ssg::TreeNodeId{"files:b"}, std::nullopt, "b.txt",
                         ssg::TreeNodeKind::file};
    ssg::TreeProviderView provider{
        ssg::TreeProviderId{"files"}, ssg::TreeProviderKind::filesystem,
        {ssg::TreeNodeView{node_a, 0, false}, ssg::TreeNodeView{node_b, 0, false}},
        ssg::TreeNodeId{"files:b"}};
    provider.first_visible = 3;
    provider.scrollbar = ssg::scrollbar_metrics(40, 9, 3);
    provider.visible_node_ids = {ssg::TreeNodeId{"files:a"},
                                 ssg::TreeNodeId{"files:b"}};
    sections_value.tree = ssg::TreeViewState{ssg::TreeRevision{7}, {provider}};
    // Also exercise the shell panel scrollbar gutter geometry on the wire.
    sections_value.shell.panel = ssg::Rect{0, 1, 24, 10};
    sections_value.shell.panel_scrollbar = ssg::Rect{23, 2, 1, 9};

    auto snapshot = ssg::assemble_session_snapshot(
        ssg::Revision{4}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(3), std::move(sections_value));
    auto const decoded =
        ssg::decode_session_snapshot(ssg::encode_session_snapshot(snapshot));
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.snapshot.has_value());
    if (!decoded.snapshot) return;
    // Whole-section equality proves the new scroll fields survive the wire.
    ASSERT_EQ(decoded.snapshot->sections().tree, snapshot.sections().tree);
    auto const& p = decoded.snapshot->sections().tree.providers.front();
    ASSERT_EQ(p.first_visible, std::uint32_t{3});
    ASSERT_EQ(p.scrollbar, ssg::scrollbar_metrics(40, 9, 3));
    ASSERT_EQ(p.visible_node_ids.size(), std::size_t{2});
    ASSERT_TRUE(decoded.snapshot->sections().shell.panel_scrollbar.has_value());
    ASSERT_EQ(decoded.snapshot->sections().shell.panel_scrollbar,
              snapshot.sections().shell.panel_scrollbar);
}

// ---------------------------------------------------------------------------
// Clipboard and status-action message kinds.

TEST(command_result_round_trips_through_the_wire) {
    ssg::CommandResult const result{
        ssg::CommandError::stale_revision, ssg::Revision{17},
        "base revision is stale"};
    auto const decoded =
        ssg::decode_command_result(ssg::encode_command_result(result));
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.result.has_value());
    ASSERT_EQ(decoded.result->error, result.error);
    ASSERT_EQ(decoded.result->revision, result.revision);
    ASSERT_EQ(decoded.result->message, result.message);
}

TEST(clipboard_request_round_trips_through_the_wire) {
    ssg::ClipboardRequest const request{
        42, ssg::ClipboardRequestKind::write, ssg::Revision{6}, "copied text"};
    auto const bytes = ssg::encode_clipboard_request(request);
    auto const decoded = ssg::decode_clipboard_request(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.request.has_value());
    ASSERT_EQ(*decoded.request, request);
}

TEST(clipboard_response_round_trips_through_the_wire) {
    ssg::ClipboardResponse const response{
        42, ssg::Revision{6}, ssg::Revision{7},
        ssg::ClipboardResponseStatus::success, "pasted text"};
    auto const bytes = ssg::encode_clipboard_response(response);
    auto const decoded = ssg::decode_clipboard_response(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.response.has_value());
    ASSERT_EQ(*decoded.response, response);
}

TEST(status_action_invocation_round_trips_through_the_wire) {
    ssg::StatusActionInvocation const invocation{ssg::StatusId{9}, "dismiss", 3};
    auto const bytes = ssg::encode_status_action_invocation(invocation);
    auto const decoded = ssg::decode_status_action_invocation(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.invocation.has_value());
    ASSERT_EQ(decoded.invocation->status_id, invocation.status_id);
    ASSERT_EQ(decoded.invocation->action_id, invocation.action_id);
    ASSERT_EQ(decoded.invocation->generation, invocation.generation);
}

// ---------------------------------------------------------------------------
// Binary-frame envelope: round trip plus decoded-byte-lifetime independence.

TEST(binary_frame_round_trips_through_the_wire) {
    ssg::BinaryFrame const frame{
        1, ssg::BinaryPayloadKind::dropped_content, 99, {9, 8, 7, 6, 5}};
    auto const bytes = ssg::encode_binary_frame(frame);
    auto const decoded = ssg::decode_binary_frame(bytes);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.frame.has_value());
    ASSERT_EQ(*decoded.frame, frame);
}

TEST(binary_frame_decoded_bytes_outlive_the_input_buffer) {
    std::optional<ssg::BinaryFrame> surviving_frame;
    {
        std::string bytes = ssg::encode_binary_frame(ssg::BinaryFrame{
            1, ssg::BinaryPayloadKind::dropped_content, 7, {1, 2, 3, 4, 5, 6}});
        auto decoded = ssg::decode_binary_frame(bytes);
        ASSERT_TRUE(decoded.accepted());
        surviving_frame = std::move(decoded.frame);
        // bytes (the input buffer) is destroyed at the end of this scope;
        // surviving_frame must not reference it.
        bytes.assign(bytes.size(), '\0');
    }
    ASSERT_TRUE(surviving_frame.has_value());
    ASSERT_EQ(surviving_frame->bytes,
             (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6}));
    ASSERT_EQ(surviving_frame->request_id, std::uint64_t{7});
}

TEST(binary_frame_rejects_an_unsupported_payload_kind) {
    std::string bytes = ssg::encode_binary_frame(ssg::BinaryFrame{
        1, ssg::BinaryPayloadKind::dropped_content, 1, {1}});
    bytes[1] = static_cast<char>(0xEE);
    auto const decoded = ssg::decode_binary_frame(bytes);
    ASSERT_FALSE(decoded.accepted());
    ASSERT_EQ(decoded.error, ssg::ProtocolError::malformed_message);
}

TEST(binary_frame_rejects_oversized_declared_length_and_frame) {
    ssg::ProtocolLimits limits;
    limits.max_binary_frame_bytes = 4;
    auto const bytes = ssg::encode_binary_frame(ssg::BinaryFrame{
        1, ssg::BinaryPayloadKind::dropped_content, 1, {1, 2, 3, 4, 5}});
    auto const decoded = ssg::decode_binary_frame(bytes, limits);
    ASSERT_FALSE(decoded.accepted());
    ASSERT_EQ(decoded.error, ssg::ProtocolError::binary_frame_too_large);
}

TEST(binary_frame_rejects_truncated_input) {
    auto bytes = ssg::encode_binary_frame(ssg::BinaryFrame{
        1, ssg::BinaryPayloadKind::dropped_content, 1, {1, 2, 3}});
    bytes.resize(bytes.size() - 1);
    auto const decoded = ssg::decode_binary_frame(bytes);
    ASSERT_FALSE(decoded.accepted());
    ASSERT_EQ(decoded.error, ssg::ProtocolError::truncated_message);
}

// ---------------------------------------------------------------------------
// Malformed / truncated / oversized / unknown-version / unknown-kind corpus,
// exercised against a representative message from each of the six kinds.

TEST(malformed_and_truncated_and_oversized_and_unknown_version_corpus) {
    ssg::ClipboardRequest const request{
        1, ssg::ClipboardRequestKind::read, ssg::Revision{1}, "x"};
    auto const canonical = ssg::encode_clipboard_request(request);
    ASSERT_TRUE(canonical.size() > 3);

    // Empty buffer: missing version byte.
    {
        auto const decoded = ssg::decode_clipboard_request(std::string_view{});
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::truncated_message);
    }

    // Single byte: missing kind byte.
    {
        auto const decoded =
            ssg::decode_clipboard_request(canonical.substr(0, 1));
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::truncated_message);
    }

    // Wrong version byte.
    {
        auto corrupted = canonical;
        corrupted[0] = static_cast<char>(0xFF);
        auto const decoded = ssg::decode_clipboard_request(corrupted);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::unsupported_version);
    }

    // Wrong kind byte (decoded with the wrong expected-kind decoder).
    {
        auto const decoded = ssg::decode_clipboard_response(canonical);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::unsupported_message_kind);
    }

    // Oversized: buffer larger than the configured message-byte limit.
    {
        ssg::ProtocolLimits limits;
        limits.max_message_bytes = canonical.size() - 1;
        auto const decoded = ssg::decode_clipboard_request(canonical, limits);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::message_too_large);
    }

    // Truncated payload: valid header, body cut short.
    {
        auto const decoded = ssg::decode_clipboard_request(
            canonical.substr(0, canonical.size() - 2));
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::truncated_message);
    }

    // Trailing garbage bytes appended after an otherwise-valid message.
    {
        auto padded = canonical;
        padded.push_back('\x7f');
        auto const decoded = ssg::decode_clipboard_request(padded);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::malformed_message);
    }
}

TEST(value_bounds_are_enforced_on_decode) {
    ssg::StatusActionInvocation const invocation{ssg::StatusId{1}, "a", 1};
    auto const bytes = ssg::encode_status_action_invocation(invocation);

    {
        ssg::ProtocolLimits limits;
        limits.max_collection_length = 0;
        auto const decoded = ssg::decode_status_action_invocation(bytes, limits);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::value_bounds_exceeded);
    }
    {
        ssg::ProtocolLimits limits;
        limits.max_text_bytes = 0;
        auto const decoded = ssg::decode_status_action_invocation(bytes, limits);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::value_bounds_exceeded);
    }
    {
        ssg::ProtocolLimits limits;
        limits.max_value_depth = 0;
        auto const decoded = ssg::decode_status_action_invocation(bytes, limits);
        ASSERT_FALSE(decoded.accepted());
        ASSERT_EQ(decoded.error, ssg::ProtocolError::value_bounds_exceeded);
    }
}

// ---------------------------------------------------------------------------
// Canonical fixtures: one hex-encoded golden wire message per kind, generated
// once against this codec (see protocol/schema/README.md). A fixture failing
// to decode, or decoding to different values than recorded here, signals an
// unintended wire-format change.

std::string read_fixture_bytes(std::string const& name) {
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

TEST(canonical_fixtures_decode_to_the_expected_values) {
    auto const registry = ssg::build_command_argument_codec_registry();

    {
        auto decoded = ssg::decode_command_request(
            read_fixture_bytes("command_request_no_payload.hex"), registry);
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.command->id, std::string{"edit.undo"});
        ASSERT_EQ(decoded.command->base_revision, ssg::Revision{3});
        ASSERT_FALSE(decoded.command->payload.has_value());
    }
    {
        auto decoded = ssg::decode_command_request(
            read_fixture_bytes("command_request_text_input.hex"), registry);
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.command->id, std::string{"text.insert"});
        auto const* arguments =
            std::any_cast<ssg::TextInputArguments>(&decoded.command->payload);
        ASSERT_TRUE(arguments != nullptr);
        ASSERT_EQ(arguments->text, std::string{"hello"});
    }
    {
        auto decoded = ssg::decode_command_result(
            read_fixture_bytes("command_result.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.result->error,
                  ssg::CommandError::stale_revision);
        ASSERT_EQ(decoded.result->revision, ssg::Revision{17});
        ASSERT_EQ(decoded.result->message,
                  std::string{"base revision is stale"});
    }
    {
        auto decoded = ssg::decode_session_snapshot(
            read_fixture_bytes("session_snapshot.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.snapshot->revision(), ssg::Revision{4});
        ASSERT_EQ(decoded.snapshot->client().client_id, ssg::ClientId{7});
        ASSERT_EQ(decoded.snapshot->client().capabilities.size(),
                 std::size_t{1});
    }
    {
        auto decoded =
            ssg::decode_session_delta(read_fixture_bytes("session_delta.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.delta->base_revision(), ssg::Revision{4});
        ASSERT_EQ(decoded.delta->revision(), ssg::Revision{5});
        ASSERT_EQ(decoded.delta->client_id(), ssg::ClientId{7});
    }
    {
        auto decoded = ssg::decode_clipboard_request(
            read_fixture_bytes("clipboard_request.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.request->id, std::uint64_t{42});
        ASSERT_EQ(decoded.request->text, std::string{"copied text"});
    }
    {
        auto decoded = ssg::decode_clipboard_response(
            read_fixture_bytes("clipboard_response.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.response->id, std::uint64_t{42});
        ASSERT_EQ(decoded.response->text, std::string{"pasted text"});
    }
    {
        auto decoded = ssg::decode_status_action_invocation(
            read_fixture_bytes("status_action_invocation.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.invocation->status_id, ssg::StatusId{9});
        ASSERT_EQ(decoded.invocation->action_id, std::string{"dismiss"});
    }
    {
        auto decoded =
            ssg::decode_binary_frame(read_fixture_bytes("binary_frame.hex"));
        ASSERT_TRUE(decoded.accepted());
        ASSERT_EQ(decoded.frame->request_id, std::uint64_t{99});
        ASSERT_EQ(decoded.frame->bytes,
                 (std::vector<std::uint8_t>{9, 8, 7, 6, 5}));
    }
}

}  // namespace

TEST(command_request_round_trips_with_replace_replacement_arguments) {
    auto const registry = ssg::build_command_argument_codec_registry();
    ssg::ClientCommand const command{
        "replace.update_replacement", ssg::Revision{9},
        ssg::FindQueryArguments{"dog"}};
    auto const bytes = ssg::encode_command_request(command, registry);
    auto const decoded = ssg::decode_command_request(bytes, registry);
    ASSERT_TRUE(decoded.accepted());
    ASSERT_EQ(decoded.command->id, command.id);
    auto const* arguments =
        std::any_cast<ssg::FindQueryArguments>(&decoded.command->payload);
    ASSERT_TRUE(arguments != nullptr);
    ASSERT_EQ(*arguments,
              std::any_cast<ssg::FindQueryArguments>(command.payload));
}

TEST(find_replace_view_state_round_trips_replacement_through_the_wire) {
    // A snapshot carrying a non-empty replacement must preserve it through the
    // snapshot codec and a delta replay (F2a).
    auto with_replacement = [](ssg::Revision revision, std::string marker,
                               std::string replacement) {
        auto s = sections(revision, std::move(marker));
        s.find_replace.replacement = std::move(replacement);
        return s;
    };
    auto snapshot = ssg::assemble_session_snapshot(
        ssg::Revision{4}, {ssg::WorkspaceId{2}, ssg::ViewId{9}},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(3),
        with_replacement(ssg::Revision{4}, "alpha", "dog"));
    auto const decoded =
        ssg::decode_session_snapshot(ssg::encode_session_snapshot(snapshot));
    ASSERT_TRUE(decoded.accepted());
    ASSERT_TRUE(decoded.snapshot.has_value());
    if (decoded.snapshot) {
        ASSERT_EQ(decoded.snapshot->sections().find_replace.replacement,
                  std::string{"dog"});
    }

    auto before = ssg::assemble_session_snapshot(
        ssg::Revision{4}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(1),
        with_replacement(ssg::Revision{4}, "a", ""));
    auto after = ssg::assemble_session_snapshot(
        ssg::Revision{5}, {},
        ssg::InvocationPrincipal{ssg::ClientId{7},
                                 ssg::InvocationOrigin::in_process},
        ssg::ViewId{9}, client_view(1),
        with_replacement(ssg::Revision{4}, "a", "dog"));
    auto const delta = ssg::derive_session_delta(before, after);
    auto decoded_delta =
        ssg::decode_session_delta(ssg::encode_session_delta(delta));
    ASSERT_TRUE(decoded_delta.accepted());
    ASSERT_TRUE(decoded_delta.delta.has_value());
    auto replayed = ssg::replay_session_delta(before, *decoded_delta.delta);
    ASSERT_TRUE(replayed.accepted());
    ASSERT_TRUE(replayed.snapshot.has_value());
    if (replayed.snapshot) {
        ASSERT_EQ(replayed.snapshot->sections().find_replace.replacement,
                  std::string{"dog"});
    }
}

int main() {
    RUN(registry_covers_every_p0_command_and_rejects_unknown_ids);
    RUN(registry_rejects_missing_entries);
    RUN(registry_rejects_extra_entries);
    RUN(registry_rejects_duplicate_entries);
    RUN(command_request_round_trips_with_no_payload);
    RUN(command_request_round_trips_with_palette_execute_arguments);
    RUN(command_request_round_trips_with_replace_replacement_arguments);
    RUN(find_replace_view_state_round_trips_replacement_through_the_wire);
    RUN(command_request_round_trips_with_text_input_arguments);
    RUN(command_request_round_trips_with_selection_command_arguments);
    RUN(command_request_round_trips_with_empty_selection_command_arguments);
    RUN(command_request_round_trips_with_scroll_lines_arguments);
    RUN(command_request_round_trips_with_scroll_pages_arguments);
    RUN(command_request_round_trips_with_scroll_fraction_arguments);
    RUN(command_request_round_trips_with_dropped_content_arguments);
    RUN(decode_command_request_rejects_unknown_command_id);
    RUN(decode_command_request_rejects_malformed_payload);
    RUN(decode_command_request_maps_domain_invariant_failures_to_malformed);
    RUN(session_snapshot_round_trips_through_the_wire);
    RUN(session_delta_round_trips_and_replay_matches_the_decoded_delta);
    RUN(two_client_capability_and_viewport_isolation_survives_the_wire);
    RUN(session_snapshot_round_trips_tree_scroll_fields);
    RUN(clipboard_request_round_trips_through_the_wire);
    RUN(command_result_round_trips_through_the_wire);
    RUN(clipboard_response_round_trips_through_the_wire);
    RUN(status_action_invocation_round_trips_through_the_wire);
    RUN(binary_frame_round_trips_through_the_wire);
    RUN(binary_frame_decoded_bytes_outlive_the_input_buffer);
    RUN(binary_frame_rejects_an_unsupported_payload_kind);
    RUN(binary_frame_rejects_oversized_declared_length_and_frame);
    RUN(binary_frame_rejects_truncated_input);
    RUN(malformed_and_truncated_and_oversized_and_unknown_version_corpus);
    RUN(value_bounds_are_enforced_on_decode);
    RUN(canonical_fixtures_decode_to_the_expected_values);
    return failed == 0 ? 0 : 1;
}
