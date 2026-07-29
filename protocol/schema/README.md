# Protocol wire schema (Plan 6: complete codec)

This directory documents the wire formats implemented by
`include/ssg/protocol.h` / `src/protocol.cpp`. All formats are little-endian,
socket-free, and enforce the bounds in `ProtocolLimits` on decode.

## `ProtocolValue` tree encoding

Every command argument, snapshot/delta field, and auxiliary message field is
first converted to a `ProtocolValue` (a bounded, versioned tree: null, bool,
signed/unsigned 64-bit integer, UTF-8 text, raw bytes, array, or object of
`(string key, ProtocolValue)` pairs), then serialized with one leading tag
byte per node:

| Tag | Kind             | Body                                                        |
|-----|------------------|--------------------------------------------------------------|
| 0   | null             | (none)                                                        |
| 1   | bool             | `u8` (0 or 1; any other value is malformed)                   |
| 2   | signed integer   | `i64` (8 bytes, two's complement)                             |
| 3   | unsigned integer | `u64` (8 bytes)                                               |
| 4   | text             | `u32` byte length, then that many UTF-8 bytes                 |
| 5   | bytes            | `u32` byte length, then that many raw bytes                   |
| 6   | array            | `u32` item count, then that many tagged values                |
| 7   | object           | `u32` field count, then `(u32 key length, key bytes, tagged value)` per field |

Decoding enforces, in this order: recursion depth (`max_value_depth`),
collection/field counts (`max_collection_length`), text byte length
(`max_text_bytes` — also used for object field keys), and bytes length
(`max_bytes_length`). Any violation reports
`ProtocolError::value_bounds_exceeded` rather than growing memory without
bound.

## Message envelope

Every one of the seven message kinds shares one envelope:

```
[u8 wire_version][u8 message_kind][tagged ProtocolValue payload]
```

`wire_version` is currently always `1`; a mismatch reports
`ProtocolError::unsupported_version`. `message_kind` matches
`ProtocolMessageKind` (`command_request = 0`, `session_snapshot = 1`,
`session_delta = 2` (3 and 4 are retired clipboard kinds, permanently
reserved so surviving kinds keep their wire values),
`status_action_invocation = 5`, `command_result = 6`); decoding with the wrong `decode_*` function
for a message reports `ProtocolError::unsupported_message_kind`. Trailing
bytes after a fully-decoded payload are rejected as
`ProtocolError::malformed_message`; a buffer exceeding
`ProtocolLimits::max_message_bytes` is rejected up front as
`ProtocolError::message_too_large` before any decoding is attempted.

Payload shapes (object field names, all required unless noted optional):

- `command_request`: `{id: text, base_revision: uint, payload: <argument wire
  value, or null for commands with no arguments>}`. `payload` is converted
  through the `CommandArgumentCodecRegistry` entry for `id`; an `id` outside
  the registry reports `ProtocolError::unsupported_command`.
- `command_result`: `{error: uint, revision: uint, message: text}`. This carries
  failure-atomic dispatch rejection such as stale revision or denied
  capability without disconnecting a valid connection.
- `session_snapshot`: `{revision, topology, client, sections}` — one field
  per `SessionSnapshot` accessor, each recursively encoded.
- `session_delta`: one field per `SessionDelta` accessor (`base_revision`,
  `revision`, `client_id`, `view_id`, `capabilities`, `topology` (optional),
  `document` (optional), `document_caret` (optional), `selection`,
  `history`, `clipboard`, `prompt_status`, `search`, `find_replace`,
  `settings`, `keymap`, `text_encoding` (optional), `tabs`, `diff`,
  `external_modification`, `follow_edits`, `tree`, `syntax`, `lsp_sync`,
  `lsp_features`, `theme`, `shell`, `viewport`). Decoding reconstructs the
  aggregate via the `decode_wire_session_delta` friend factory declared in
  `session_snapshot.h`, so this is the only construction path outside
  `derive_session_delta`.
- `status_action_invocation`: encodes `StatusActionInvocation` directly
  (`status_id`, `action_id`, `generation`).

## Command argument codec registry

`build_command_argument_codec_registry()` binds every ID returned by
`p0_command_descriptors()` (the assembled P0 catalog) to exactly one wire
adapter. There is no untyped fallback: `CommandArgumentCodecRegistry`'s
constructor throws `std::invalid_argument` unless the supplied entries cover
the catalog exactly (no missing, extra, or duplicate IDs). Six command IDs
carry a typed payload; every other command's payload is the wire null:

| Command ID(s)                                   | Payload type               |
|---------------------------------------------------|-----------------------------|
| every `text.*` ID (`text_input_command_set()`)     | `TextInputArguments`        |
| every selection-navigation ID (`selection_navigation_command_set()`) | `SelectionCommandArguments` |
| `view.scroll_lines`                                | `ScrollLinesArguments`      |
| `view.scroll_pages`                                | `ScrollPagesArguments`      |
| `view.scroll_to_fraction`                          | `ScrollFractionArguments`   |
| `file.open_dropped_content`                        | `DroppedContentArguments`   |
| everything else                                    | none (wire null)            |

## Binary-frame envelope

The only P0 binary-payload support (e.g. carrying `file.open_dropped_content`
bytes out of band of the tagged-value tree):

```
[u8 version][u8 payload_kind][u64 request_id][u32 declared_length][declared_length bytes]
```

`payload_kind` matches `BinaryPayloadKind` (currently only
`dropped_content = 0`; any other value is `ProtocolError::malformed_message`).
`declared_length` and the overall buffer are both checked against
`ProtocolLimits::max_binary_frame_bytes`
(`ProtocolError::binary_frame_too_large`); a short buffer is
`ProtocolError::truncated_message`; trailing bytes past `declared_length` are
`ProtocolError::malformed_message`. Decoded bytes are copied into an owned
`std::vector<std::uint8_t>` independent of the input buffer, so the input may
be destroyed immediately after `decode_binary_frame` returns.

## Fixtures

`tests/fixtures/protocol/` holds one hex-encoded canonical wire message per
message kind plus one binary frame, generated once against the current codec
and asserted stable by `tests/test_protocol.cpp`'s
`canonical_fixtures_decode_to_the_expected_values` test. A fixture failing to
decode, or decoding to different field values, signals an unintended wire
format change.
