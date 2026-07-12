const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", {fatal: true});
const limits = Object.freeze({
  messageBytes: 32 * 1024 * 1024,
  valueDepth: 32,
  collectionLength: 65536,
  textBytes: 8 * 1024 * 1024,
  bytesLength: 16 * 1024 * 1024,
  binaryFrameBytes: 16 * 1024 * 1024,
});

class SignedInteger {
  constructor(value) {
    this.value = BigInt(value);
  }
}

const messageKinds = [
  "command_request",
  "session_snapshot",
  "session_delta",
  "clipboard_request",
  "clipboard_response",
  "status_action_invocation",
  "command_result",
];

class Writer {
  bytes = [];

  u8(value) {
    if (!Number.isInteger(value) || value < 0 || value > 0xff) {
      throw new RangeError("protocol byte is out of range");
    }
    this.bytes.push(value);
  }
  u32(value) {
    if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
      throw new RangeError("protocol length is out of range");
    }
    for (let shift = 0; shift < 32; shift += 8) {
      this.bytes.push((value >>> shift) & 0xff);
    }
  }
  u64(value) {
    let remaining = BigInt.asUintN(64, BigInt(value));
    for (let index = 0; index < 8; ++index) {
      this.bytes.push(Number(remaining & 0xffn));
      remaining >>= 8n;
    }
  }
  data(value) { this.bytes.push(...value); }
  finish() { return Uint8Array.from(this.bytes); }
}

class Reader {
  constructor(bytes) {
    this.bytes = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    this.offset = 0;
  }
  take(length) {
    if (this.offset + length > this.bytes.length) {
      throw new RangeError("truncated protocol message");
    }
    const value = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return value;
  }
  u8() { return this.take(1)[0]; }
  u32() {
    const bytes = this.take(4);
    return (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) |
      (bytes[3] * 0x1000000)) >>> 0;
  }
  u64(signed = false) {
    const bytes = this.take(8);
    let value = 0n;
    for (let index = 7; index >= 0; --index) {
      value = (value << 8n) | BigInt(bytes[index]);
    }
    return signed ? BigInt.asIntN(64, value) : value;
  }
}

export function encodeSessionAttach(credential, lastAppliedRevision) {
  const revision = lastAppliedRevision === undefined ||
    lastAppliedRevision === null ? "-" : BigInt(lastAppliedRevision).toString();
  return `SSG1 ATTACH ${revision} ${toHex(encoder.encode(credential))}`;
}

export function encodeCommandRequest(command) {
  const payload = command.arguments === undefined
    ? null
    : commandPayload(command.command_id, command.arguments);
  return encodeEnvelope(0, {
    id: command.command_id,
    base_revision: BigInt(command.base_revision),
    payload,
  });
}

function commandPayload(commandId, payload) {
  if (commandId.startsWith("cursor.") || commandId.startsWith("select.") ||
      commandId === "view.reveal_caret" ||
      commandId === "view.center_caret") {
    return payload ?? {position: null, selection: null};
  }
  if (commandId === "view.scroll_lines") {
    return {...payload, rows: new SignedInteger(payload.rows)};
  }
  if (commandId === "view.scroll_pages") {
    return {...payload, pages: new SignedInteger(payload.pages)};
  }
  return payload;
}

export function encodeClipboardResponse(response) {
  return encodeEnvelope(4, response);
}

export function encodeStatusActionInvocation(invocation) {
  return encodeEnvelope(5, invocation);
}

export function encodeBinaryFrame({kind, request_id, bytes}) {
  if (kind !== "dropped_content") {
    throw new RangeError("unsupported binary payload kind");
  }
  const payload = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  if (payload.length + 14 > limits.binaryFrameBytes) {
    throw new RangeError("binary frame is too large");
  }
  const writer = new Writer();
  writer.u8(1);
  writer.u8(0);
  writer.u64(request_id);
  writer.u32(payload.length);
  writer.data(payload);
  return writer.finish();
}

export function encodeEnvelope(kind, value) {
  const writer = new Writer();
  writer.u8(1);
  writer.u8(kind);
  writeValue(writer, value);
  return writer.finish();
}

export function decodeEnvelope(bytes) {
  const reader = new Reader(bytes);
  if (reader.bytes.length > limits.messageBytes) {
    throw new RangeError("protocol message is too large");
  }
  if (reader.u8() !== 1) throw new RangeError("unsupported protocol version");
  const kindIndex = reader.u8();
  if (kindIndex >= messageKinds.length) {
    throw new RangeError("unsupported protocol message kind");
  }
  const value = readValue(reader, 0);
  if (reader.offset !== reader.bytes.length) {
    throw new RangeError("trailing protocol bytes");
  }
  return {kind: messageKinds[kindIndex], value};
}

function writeValue(writer, value) {
  if (value === null || value === undefined) {
    writer.u8(0);
  } else if (typeof value === "boolean") {
    writer.u8(1);
    writer.u8(value ? 1 : 0);
  } else if (value instanceof SignedInteger) {
    writer.u8(2);
    writer.u64(value.value);
  } else if (typeof value === "bigint") {
    writer.u8(value < 0n ? 2 : 3);
    writer.u64(value);
  } else if (typeof value === "number") {
    if (!Number.isSafeInteger(value)) {
      throw new TypeError("protocol numbers must be safe integers");
    }

    writer.u8(value < 0 ? 2 : 3);
    writer.u64(BigInt(value));
  } else if (typeof value === "string") {
    const bytes = encoder.encode(value);
    if (bytes.length > limits.textBytes) {
      throw new RangeError("protocol text is too large");
    }
    writer.u8(4);
    writer.u32(bytes.length);
    writer.data(bytes);
  } else if (value instanceof Uint8Array) {
    if (value.length > limits.bytesLength) {
      throw new RangeError("protocol bytes are too large");
    }
    writer.u8(5);
    writer.u32(value.length);
    writer.data(value);
  } else if (Array.isArray(value)) {
    if (value.length > limits.collectionLength) {
      throw new RangeError("protocol collection is too large");
    }
    writer.u8(6);
    writer.u32(value.length);
    for (const item of value) writeValue(writer, item);
  } else if (typeof value === "object") {
    const fields = Object.entries(value);
    if (fields.length > limits.collectionLength) {
      throw new RangeError("protocol object is too large");
    }
    writer.u8(7);
    writer.u32(fields.length);
    for (const [key, field] of fields) {
      const bytes = encoder.encode(key);
      if (bytes.length > limits.textBytes) {
        throw new RangeError("protocol field name is too large");
      }
      writer.u32(bytes.length);
      writer.data(bytes);
      writeValue(writer, field);
    }
  } else {
    throw new TypeError(`unsupported protocol value: ${typeof value}`);
  }
}

function readValue(reader, depth) {
  if (depth > limits.valueDepth) {
    throw new RangeError("protocol value depth exceeded");
  }
  switch (reader.u8()) {
    case 0: return null;
    case 1: {
      const value = reader.u8();
      if (value > 1) throw new RangeError("invalid protocol boolean");
      return value === 1;
    }
    case 2: return reader.u64(true);
    case 3: return reader.u64();
    case 4: {
      const length = reader.u32();
      if (length > limits.textBytes) throw new RangeError("protocol text is too large");
      return decoder.decode(reader.take(length));
    }
    case 5: {
      const length = reader.u32();
      if (length > limits.bytesLength) {
        throw new RangeError("protocol bytes are too large");
      }
      return reader.take(length).slice();
    }
    case 6: {
      const count = reader.u32();
      if (count > limits.collectionLength) {
        throw new RangeError("protocol collection is too large");
      }
      return Array.from({length: count}, () => readValue(reader, depth + 1));
    }
    case 7: {
      const count = reader.u32();
      if (count > limits.collectionLength) {
        throw new RangeError("protocol object is too large");
      }
      const object = {};
      for (let index = 0; index < count; ++index) {
        const length = reader.u32();
        if (length > limits.textBytes) {
          throw new RangeError("protocol field name is too large");
        }
        const key = decoder.decode(reader.take(length));
        if (Object.hasOwn(object, key)) {
          throw new RangeError(`duplicate protocol field: ${key}`);
        }
        object[key] = readValue(reader, depth + 1);
      }
      return object;
    }
    default: throw new RangeError("invalid protocol value tag");
  }
}

function toHex(bytes) {
  return [...bytes].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}
