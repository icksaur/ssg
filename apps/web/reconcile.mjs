// Pure client logic with no DOM dependency, so a node test can exercise the wire
// decode, the UTF-8 <-> UTF-16 offset mapping, and (from M3) the local-echo
// reconciliation exactly as the browser runs them. Nothing here touches
// document/window; client.mjs owns all rendering and I/O.

const PROTOCOL_LIMITS = {
  messageBytes: 32 * 1024 * 1024,
  valueDepth: 32,
  collectionLength: 65536,
  textBytes: 8 * 1024 * 1024,
  bytesLength: 16 * 1024 * 1024,
};
const wireTextDecoder = new TextDecoder('utf-8', { fatal: true });

function requireWireBytes(dv, p, count) {
  if (!Number.isSafeInteger(count) || count < 0 ||
      p < 0 || p > dv.byteLength || count > dv.byteLength - p) {
    throw new Error('truncated protocol value');
  }
}

// Decode one bounded tagged ProtocolValue from a DataView at {p}. Returns
// [value, next] and mirrors protocol/schema/README.md's tag encoding.
export function decodeValue(dv, p, depth = 0) {
  if (depth >= PROTOCOL_LIMITS.valueDepth) {
    throw new Error('protocol value depth exceeded');
  }
  requireWireBytes(dv, p, 1);
  const tag = dv.getUint8(p); p += 1;
  switch (tag) {
    case 0: return [null, p];
    case 1: {
      requireWireBytes(dv, p, 1);
      const value = dv.getUint8(p);
      if (value > 1) throw new Error('malformed protocol bool');
      return [value !== 0, p + 1];
    }
    case 2: {
      requireWireBytes(dv, p, 8);
      const v = dv.getBigInt64(p, true);
      return [v, p + 8];
    }
    case 3: {
      requireWireBytes(dv, p, 8);
      const v = dv.getBigUint64(p, true);
      return [v, p + 8];
    }
    case 4: case 5: {
      requireWireBytes(dv, p, 4);
      const n = dv.getUint32(p, true); p += 4;
      const limit = tag === 4 ? PROTOCOL_LIMITS.textBytes
                              : PROTOCOL_LIMITS.bytesLength;
      if (n > limit) throw new Error('protocol value length exceeded');
      requireWireBytes(dv, p, n);
      const bytes = new Uint8Array(dv.buffer, dv.byteOffset + p, n); p += n;
      if (tag === 4) return [wireTextDecoder.decode(bytes), p];
      return [bytes.slice(), p];
    }
    case 6: {
      requireWireBytes(dv, p, 4);
      const n = dv.getUint32(p, true); p += 4; const arr = [];
      if (n > PROTOCOL_LIMITS.collectionLength) {
        throw new Error('protocol collection length exceeded');
      }
      for (let i = 0; i < n; i++) {
        const [v, np] = decodeValue(dv, p, depth + 1);
        arr.push(v); p = np;
      }
      return [arr, p];
    }
    case 7: {
      requireWireBytes(dv, p, 4);
      const n = dv.getUint32(p, true); p += 4; const obj = {};
      if (n > PROTOCOL_LIMITS.collectionLength) {
        throw new Error('protocol collection length exceeded');
      }
      for (let i = 0; i < n; i++) {
        requireWireBytes(dv, p, 4);
        const kl = dv.getUint32(p, true); p += 4;
        if (kl > PROTOCOL_LIMITS.textBytes) {
          throw new Error('protocol object key length exceeded');
        }
        requireWireBytes(dv, p, kl);
        const key = wireTextDecoder.decode(
          new Uint8Array(dv.buffer, dv.byteOffset + p, kl));
        p += kl;
        const [v, np] = decodeValue(dv, p, depth + 1);
        obj[key] = v; p = np;
      }
      return [obj, p];
    }
    default: throw new Error('bad tag ' + tag + ' at ' + (p - 1));
  }
}

// The first node with a document.text string is the sections object; its
// siblings (selection, syntax, tabs, theme, focus) are the semantic model.
export function findSections(node) {
  if (Array.isArray(node)) {
    for (const item of node) { const f = findSections(item); if (f) return f; }
  } else if (node && typeof node === 'object') {
    if (node.document && typeof node.document === 'object' &&
        typeof node.document.text === 'string') {
      return normalizeUiFrameSections(node) ? node : null;
    }
    for (const k of Object.keys(node)) { const f = findSections(node[k]); if (f) return f; }
  }
  return null;
}

export const num = (v) => typeof v === 'bigint' ? Number(v) : v;
export const hex2 = (n) => (n & 255).toString(16).padStart(2, '0');
export const cssColor = (c) => c ? ('#' + hex2(num(c.red)) + hex2(num(c.green)) + hex2(num(c.blue))) : '';

export function roleColor(ordinal, theme) {
  const rc = (theme && Array.isArray(theme.role_colors)) ? theme.role_colors : [];
  return ordinal != null && ordinal >= 0 && ordinal < rc.length
    ? cssColor(rc[ordinal])
    : '';
}

export function applyNodeSemanticStyle(target, node, theme) {
  const style = node && node.style ? node.style : {};
  const foreground = node && node.role != null ? node.role : style.foreground;
  target.color = roleColor(foreground, theme);
  target.backgroundColor = roleColor(style.background, theme);
}

export function getOrCreateStyledNode(cache, node, theme, create) {
  const element = cache.getOrCreate(node.id, create);
  applyNodeSemanticStyle(element.style, node, theme);
  return element;
}

// UTF-8 byte length of a single code point's encoding.
export function utf8Len(cp) {
  return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

// Map char-boundary UTF-8 byte offsets to UTF-16 string indices: document
// positions are byte offsets, JS slices by code unit.
export function byteToIndex(text, offsets) {
  const want = new Set(offsets.map(num));
  const map = new Map();
  let byte = 0, idx = 0;
  if (want.has(0)) map.set(0, 0);
  for (const ch of text) {
    byte += utf8Len(ch.codePointAt(0));
    idx += ch.length;
    if (want.has(byte)) map.set(byte, idx);
  }
  return map;
}

export const utf8Bytes = (s) => new TextEncoder().encode(s).length;

function concat(chunks) {
  const out = new Uint8Array(chunks.reduce((n, c) => n + c.length, 0));
  let p = 0;
  for (const c of chunks) { out.set(c, p); p += c.length; }
  return out;
}

const u32 = (n) => {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setUint32(0, n, true);
  return b;
};
const u64 = (n) => {
  const b = new Uint8Array(8);
  new DataView(b.buffer).setBigUint64(0, BigInt(n), true);
  return b;
};
const i64 = (n) => {
  const b = new Uint8Array(8);
  new DataView(b.buffer).setBigInt64(0, BigInt(n), true);
  return b;
};

export function encodeValue(value) {
  const enc = new TextEncoder();
  if (value == null) return new Uint8Array([0]);
  if (typeof value === 'boolean') return new Uint8Array([1, value ? 1 : 0]);
  if (typeof value === 'number' || typeof value === 'bigint') {
    return value < 0
      ? concat([new Uint8Array([2]), i64(value)])
      : concat([new Uint8Array([3]), u64(value)]);
  }
  if (typeof value === 'string') {
    const bytes = enc.encode(value);
    return concat([new Uint8Array([4]), u32(bytes.length), bytes]);
  }
  if (value instanceof Uint8Array) {
    return concat([new Uint8Array([5]), u32(value.length), value]);
  }
  if (Array.isArray(value)) {
    return concat([new Uint8Array([6]), u32(value.length),
                   ...value.map(encodeValue)]);
  }
  if (typeof value === 'object') {
    const entries = Object.entries(value);
    const fields = [];
    for (const [key, fieldValue] of entries) {
      const bytes = enc.encode(key);
      fields.push(u32(bytes.length), bytes, encodeValue(fieldValue));
    }
    return concat([new Uint8Array([7]), u32(entries.length), ...fields]);
  }
  throw new TypeError('unsupported protocol value');
}

export function encodeMessage(kind, payload) {
  return concat([new Uint8Array([4, kind]), encodeValue(payload)]);
}

export function decodeMessage(buffer) {
  const dv = new DataView(buffer);
  if (dv.byteLength > PROTOCOL_LIMITS.messageBytes) {
    throw new Error('protocol message length exceeded');
  }
  if (dv.byteLength < 3 || dv.getUint8(0) !== 4) {
    throw new Error('unsupported protocol frame');
  }
  const kind = dv.getUint8(1);
  const [payload, end] = decodeValue(dv, 2);
  if (end !== dv.byteLength) throw new Error('trailing protocol bytes');
  return { kind, payload };
}

export function browserInboundKind(kind) {
  if (kind === 1) return 'snapshot';
  if (kind === 2) return 'delta';
  if (kind === 6) return 'command-result';
  if (kind === 8) return 'input-result';
  return 'ignore';
}

export function settleCommandResult(queue) {
  if (!Array.isArray(queue) || queue.length === 0) return null;
  return { owner: queue[0], queue: queue.slice(1) };
}

export function encodeCommandRequest(id, baseRevision, payload = null) {
  return encodeMessage(0, { id, base_revision: BigInt(baseRevision), payload });
}

export const encodeUiNodeActivationCommand = (
    nodeId, schemaGeneration, revision) =>
  encodeCommandRequest('ui.activate', revision, {
    generation: BigInt(schemaGeneration),
    node_id: String(nodeId),
  });

export function encodeClientInput({ code = '', control = false, alt = false,
                                    meta = false, shift = false, text = '' }) {
  const stroke = code ? { code, control, alt, meta, shift } : null;
  return encodeMessage(7, { kind: 0n, stroke, committed_text: text });
}

export function encodeViewNavigationInput(observedRevision) {
  return encodeMessage(7, {
    kind: 12n,
    basis_revision: BigInt(observedRevision),
  });
}

export function encodeResolvedSelectionInput(
    observedRevision, activeTab, documentRevision, selections) {
  return encodeMessage(7, {
    kind: 14n,
    basis_revision: BigInt(observedRevision),
    active_tab: BigInt(activeTab),
    document_revision: BigInt(documentRevision),
    selections: selections.map(({ anchor, active }) => ({
      anchor: BigInt(anchor),
      active: BigInt(active),
    })),
  });
}

const MODIFIER_CODES = new Set([
  'AltLeft', 'AltRight', 'ControlLeft', 'ControlRight',
  'MetaLeft', 'MetaRight', 'ShiftLeft', 'ShiftRight',
]);

export class BrowserKeyDispatchTracker {
  constructor() {
    this.observedKeydowns = new Set();
    this.altDown = false;
  }

  keydown(code) {
    if (code === 'AltLeft' || code === 'AltRight') this.altDown = true;
    if (MODIFIER_CODES.has(code)) return false;
    this.observedKeydowns.add(code);
    return true;
  }

  keyup(code, alt) {
    if (code === 'AltLeft' || code === 'AltRight') {
      this.altDown = false;
      return false;
    }
    if (MODIFIER_CODES.has(code)) return false;
    const observed = this.observedKeydowns.delete(code);
    return !observed && alt && this.altDown;
  }

  clear() {
    this.observedKeydowns.clear();
    this.altDown = false;
  }
}

export const PICKER_MODE = Object.freeze({ FILE: 0, COMMAND: 4 });

const encodePointerInput = (kind, fields, button = 0, phase = 0) =>
  encodeMessage(7, {
    kind: BigInt(kind),
    button: BigInt(button),
    phase: BigInt(phase),
    ...fields,
  });

export const encodePickerPointerInput = (activation, candidateId) =>
  encodePointerInput(3, {
    picker_mode: BigInt(activation.mode),
    activation_id: BigInt(activation.id),
    candidate_id: String(candidateId),
  });

export const encodeDocumentPointerInput = (
    position, revision,
    { additive = false, selectWord = false, phase = 0, edge = 0 } = {}) =>
  encodePointerInput(9, {
    basis_revision: BigInt(revision),
    position: position == null ? null : BigInt(position),
    additive,
    select_word: selectWord,
    edge: BigInt(edge),
  }, 0, phase);

export const encodeScrollLinesInput = (target, rows, revision) =>
  encodeMessage(7, {
    kind: 10n,
    basis_revision: BigInt(revision),
    target: BigInt(target),
    rows: BigInt(rows),
  });

export const encodeScrollFractionInput = (
    target, numerator, denominator, revision) =>
  encodeMessage(7, {
    kind: 11n,
    basis_revision: BigInt(revision),
    target: BigInt(target),
    numerator: BigInt(numerator),
    denominator: BigInt(denominator),
  });

export const encodeTabPointerInput = (tabId, revision, button = 0) =>
  encodePointerInput(1, {
    basis_revision: BigInt(revision),
    tab_id: BigInt(tabId),
  }, button);

export function markedTextByteOffset(byteStart, text, utf16Offset) {
  if (!Number.isSafeInteger(byteStart) || byteStart < 0 ||
      !Number.isSafeInteger(utf16Offset) || utf16Offset < 0 ||
      utf16Offset > text.length) {
    throw new TypeError('invalid marked text offset');
  }
  if (utf16Offset > 0 && utf16Offset < text.length) {
    const before = text.charCodeAt(utf16Offset - 1);
    const after = text.charCodeAt(utf16Offset);
    if (before >= 0xd800 && before <= 0xdbff &&
        after >= 0xdc00 && after <= 0xdfff) {
      throw new TypeError('marked text offset splits a surrogate pair');
    }
  }
  return byteStart + utf8Bytes(text.slice(0, utf16Offset));
}

export const encodeTreePointerInput = (nodeId, revision) =>
  encodePointerInput(2, {
    basis_revision: BigInt(revision),
    node_id: String(nodeId),
  });

export const encodePromptControlPointerInput = (controlId, revision) =>
  encodePointerInput(4, {
    basis_revision: BigInt(revision),
    control_id: String(controlId),
  });

export const encodeExternalActionPointerInput = (action, fileId, revision) =>
  encodePointerInput(5, {
    basis_revision: BigInt(revision),
    invocation: { file_id: String(fileId), action: BigInt(action) },
  });

export const encodeStatusActionPointerInput = (
    { statusId, actionId, generation }, revision) =>
  encodePointerInput(6, {
    basis_revision: BigInt(revision),
    invocation: {
      status_id: BigInt(statusId),
      action_id: String(actionId),
      generation: BigInt(generation),
    },
  });

export const encodePublishedUiActionPointerInput = (
    nodeId, schemaGeneration, revision) =>
  encodePointerInput(7, {
    basis_revision: BigInt(revision),
    schema_generation: BigInt(schemaGeneration),
    node_id: String(nodeId),
  });

export const encodeNoticeActionPointerInput = (actionId, revision) =>
  encodePointerInput(8, {
    basis_revision: BigInt(revision),
    action_id: String(actionId),
  });

export function settleInput(inputQueue, pending, result, appliedRevision) {
  if (inputQueue.length === 0) return null;
  const required = result && result.command
    ? BigInt(result.command.revision) : BigInt(appliedRevision);
  if (BigInt(appliedRevision) < required) return null;
  const [input, ...remainingInputs] = inputQueue;
  const remainingPending = input.predictionId == null
    ? pending : pending.filter((item) => item.id !== input.predictionId);
  return { inputQueue: remainingInputs, pending: remainingPending };
}

export function predictPromptValue(value, key, alt) {
  if (key === 'Backspace') {
    if (alt) return null;
    const segments = [...new Intl.Segmenter(
      undefined, { granularity: 'grapheme' }).segment(value)];
    return segments.length === 0
      ? null
      : value.slice(0, segments[segments.length - 1].index);
  }

  return Array.from(key).length === 1 && !alt ? value + key : null;
}

export function settlePromptPrediction(prediction, remainingInputs,
                                       completedInput) {
  if (!prediction || !completedInput) return prediction;
  return remainingInputs.some((input) => input.promptInput)
    ? prediction : null;
}

export function settlePromptPresentation(prediction, remainingInputs,
                                         completedInput, userScrolled) {
  const next = settlePromptPrediction(
    prediction, remainingInputs, completedInput);
  const settled = prediction != null && next == null;
  return {
    prediction: next,
    renderDocument: settled,
    revealDocument: settled && !userScrolled,
  };
}

export function deferPromptDocumentSurface(surfaces, pending) {
  return pending
    ? surfaces.filter((surface) => surface !== SURFACE.DOCUMENT)
    : surfaces;
}

export const isCurrentGeneration = (current, callbackGeneration) =>
  current === callbackGeneration;

export const replayAttachFrame = (hasState, revision) =>
  'SSG1 ATTACH ' + (hasState ? BigInt(revision) : '-');

export const deltaIsContiguous = (revision, delta) =>
  !!delta && BigInt(delta.base_revision) === BigInt(revision);

export const clearUncertainInputs = () => ({ pending: [], inputQueue: [] });

export const reconnectDelay = (attempt) =>
  Math.min(250 * (2 ** attempt), 5000);

// --- M3 local-echo reconciliation (pure; the browser and the node test share
// this exact code so the client behavior is what the test pins) ---

// Apply one authoritative DocumentDelta splice to the current text. The delta's
// start and erased_bytes are UTF-8 byte offsets; inserted_text is the new text.
export function applyDocumentDelta(text, delta) {
  const start = num(delta.start);
  const erased = num(delta.erased_bytes);
  const map = byteToIndex(text, [start, start + erased]);
  const si = map.get(start);
  const ei = map.get(start + erased);
  if (si === undefined || ei === undefined) return text;  // out-of-range: ignore
  return text.slice(0, si) + (delta.inserted_text || '') + text.slice(ei);
}

// The still-unacknowledged predicted text, in type order.
export const predictedText = (pending) => pending.map((p) => p.text).join('');

// Project the authoritative document plus caret-anchored predictions into what
// to show: the predicted text spliced in at the caret, the displayed caret moved
// past it, and the byte range the prediction occupies (rendered unstyled).
export function project(authText, authCaret, pending) {
  const pred = predictedText(pending);
  const ci = byteToIndex(authText, [authCaret]).get(authCaret);
  const at = ci === undefined ? authText.length : ci;
  const predBytes = utf8Bytes(pred);
  return {
    text: authText.slice(0, at) + pred + authText.slice(at),
    caret: authCaret + predBytes,
    predStart: authCaret,
    predEnd: authCaret + predBytes,
  };
}

// Shift an authoritative byte offset into projected coordinates: content at or
// after the caret moves right by the predicted byte length.
export const shiftOffset = (offset, predStart, predBytes) =>
  offset >= predStart ? offset + predBytes : offset;


function wireNumber(value, name) {
  const n = num(value);
  if (!Number.isInteger(n)) throw new TypeError('malformed palette matcher parameter ' + name);
  return n;
}

export function matcherParametersFromWire(params) {
  if (!params || typeof params !== 'object') {
    throw new TypeError('missing palette matcher parameters');
  }
  return {
    baseScore: wireNumber(params.base_score, 'base_score'),
    wordBoundaryBonus: wireNumber(params.word_boundary_bonus, 'word_boundary_bonus'),
    contiguityBonus: wireNumber(params.contiguity_bonus, 'contiguity_bonus'),
    exactCaseBonus: wireNumber(params.exact_case_bonus, 'exact_case_bonus'),
    lengthCap: wireNumber(params.length_cap, 'length_cap'),
  };
}

export function matcherBoundsFromPalette(palette) {
  if (!palette || typeof palette !== 'object') throw new TypeError('missing palette section');
  const maxMagnitude = wireNumber(palette.max_parameter_magnitude, 'max_parameter_magnitude');
  const maxCandidateBytes = wireNumber(palette.max_candidate_bytes, 'max_candidate_bytes');
  return { params: matcherParametersFromWire(palette.parameters), maxMagnitude, maxCandidateBytes };
}

export function pickerCandidatesFromPalette(palette, mode) {
  if (!palette || typeof palette !== 'object') {
    throw new TypeError('missing palette section');
  }
  if (mode === PICKER_MODE.COMMAND) {
    if (!Array.isArray(palette.command_candidates)) {
      throw new TypeError('missing command picker inventory');
    }
    return palette.command_candidates;
  }
  if (mode === PICKER_MODE.FILE) {
    if (!Array.isArray(palette.file_candidates)) {
      throw new TypeError('missing file picker inventory');
    }
    return palette.file_candidates;
  }
  throw new TypeError('unsupported picker mode');
}

export function effectivePickerMode(palette, localMode) {
  return palette && palette.active_mode != null
    ? num(palette.active_mode)
    : localMode;
}

export function queuePickerSubmit(
    intent, authoritativeActivation, openingPending, queued) {
  if (queued) return { send: null, queued };
  if (openingPending) return { send: null, queued: intent };
  const submit = authoritativeActivation &&
      authoritativeActivation.mode === intent.mode
    ? {
        activation: authoritativeActivation,
        candidateId: intent.candidateId,
      }
    : null;
  if (submit) return { send: submit, queued: null };
  return { send: null, queued: null };
}

export function settlePickerLifecycle(
    completedInput, authoritativeActivation, completedActivation, queuedSubmit) {
  if (!completedInput || completedInput.pickerOpenMode == null) {
    return {
      activation: authoritativeActivation, submit: null,
      preservePresentation: false,
    };
  }

  const expected = completedInput.pickerOpenMode;
  const matchingOpening = authoritativeActivation && completedActivation &&
      authoritativeActivation.mode === expected &&
      completedActivation.mode === expected &&
      authoritativeActivation.id === completedActivation.id;
  const submit = matchingOpening && queuedSubmit &&
      queuedSubmit.mode === expected
    ? {
        activation: completedActivation,
        candidateId: queuedSubmit.candidateId,
      }
    : null;
  return {
    activation: authoritativeActivation, submit,
    preservePresentation: Boolean(matchingOpening),
  };
}

export function pickerPresentationFromSubmit(
    error, authoritativeActivation, previous) {
  const sameActivation = authoritativeActivation &&
    previous &&
    previous.mode === authoritativeActivation.mode &&
    previous.activationId === authoritativeActivation.id;
  if (sameActivation) {
    return {
      mode: previous.mode,
      activationId: previous.activationId,
      query: previous.query,
      selected: previous.selected,
      pendingSubmit: null,
    };
  }
  return {
    mode: authoritativeActivation == null
      ? null : authoritativeActivation.mode,
    activationId: authoritativeActivation == null
      ? null : authoritativeActivation.id,
    query: '',
    selected: 0,
    pendingSubmit: null,
  };
}

function sameStroke(bindingStroke, inputStroke) {
  return bindingStroke && inputStroke &&
    bindingStroke.code === inputStroke.code &&
    !!bindingStroke.control === !!inputStroke.control &&
    !!bindingStroke.alt === !!inputStroke.alt &&
    !!bindingStroke.meta === !!inputStroke.meta &&
    !!bindingStroke.shift === !!inputStroke.shift;
}

export function resolveKeyCommand(keymap, inputStroke, context) {
  if (!keymap || !Array.isArray(keymap.bindings)) return null;
  let resolved = null;
  for (const binding of keymap.bindings) {
    if (!Array.isArray(binding.sequence) || binding.sequence.length !== 1 ||
        !sameStroke(binding.sequence[0], inputStroke) ||
        (binding.context !== '*' && binding.context !== context)) {
      continue;
    }
    if (resolved == null ||
        (binding.context === '*' && resolved.context !== '*')) {
      resolved = binding;
    }
  }
  return resolved == null ? null : resolved.command_id;
}

export function resolvePickerLifecycle(keymap, palette, inputStroke, context) {
  if (!palette) return null;
  const command = resolveKeyCommand(keymap, inputStroke, context);
  if (command === palette.command_open_command_id) {
    return PICKER_MODE.COMMAND;
  }
  if (command === palette.file_open_command_id) {
    return PICKER_MODE.FILE;
  }
  return null;
}

export const CLIENT_OWNED_INPUT = Object.freeze({
  APPEND_TEXT: 0,
  DELETE_GRAPHEME_BACKWARD: 1,
  DELETE_WORD_BACKWARD: 2,
  SELECT_NEXT: 3,
  SELECT_PREVIOUS: 4,
  SUBMIT: 5,
});

export function predictPickerInput(keymap, inputStroke, text) {
  const command = resolveKeyCommand(keymap, inputStroke, 'prompt');
  if (command === 'prompt.submit') {
    return { kind: CLIENT_OWNED_INPUT.SUBMIT, text: '' };
  }
  if (command === 'prompt.next' || command === 'palette.next') {
    return { kind: CLIENT_OWNED_INPUT.SELECT_NEXT, text: '' };
  }
  if (command === 'prompt.previous' || command === 'palette.previous') {
    return { kind: CLIENT_OWNED_INPUT.SELECT_PREVIOUS, text: '' };
  }
  if (command === 'prompt.cancel') return { close: true };
  if (inputStroke.code === 'Backspace') {
    return {
      kind: inputStroke.alt
        ? CLIENT_OWNED_INPUT.DELETE_WORD_BACKWARD
        : CLIENT_OWNED_INPUT.DELETE_GRAPHEME_BACKWARD,
      text: '',
    };
  }
  return text
    ? { kind: CLIENT_OWNED_INPUT.APPEND_TEXT, text }
    : null;
}

export function deleteLastGrapheme(text) {
  if (!text) return text;
  const segments = [...new Intl.Segmenter(
    undefined, { granularity: 'grapheme' }).segment(text)];
  return segments.length < 2 ? '' : text.slice(0, segments.at(-1).index);
}

export function deleteLastWord(text) {
  let end = text.length;
  const isWord = (character) => /[A-Za-z0-9_]/.test(character);
  while (end > 0 && !isWord(text[end - 1])) --end;
  while (end > 0 && isWord(text[end - 1])) --end;
  return text.slice(0, end);
}

export function applyPickerInputPrediction(picker, prediction, rowCount) {
  const next = { ...picker };
  if (prediction == null || prediction.close ||
      prediction.kind === CLIENT_OWNED_INPUT.SUBMIT) return next;
  switch (prediction.kind) {
    case CLIENT_OWNED_INPUT.APPEND_TEXT:
      next.query += prediction.text;
      next.selected = 0;
      break;
    case CLIENT_OWNED_INPUT.DELETE_GRAPHEME_BACKWARD:
      next.query = deleteLastGrapheme(next.query);
      next.selected = 0;
      break;
    case CLIENT_OWNED_INPUT.DELETE_WORD_BACKWARD:
      next.query = deleteLastWord(next.query);
      next.selected = 0;
      break;
    case CLIENT_OWNED_INPUT.SELECT_NEXT:
      next.selected = clampPaletteSelection(next.selected + 1, rowCount);
      break;
    case CLIENT_OWNED_INPUT.SELECT_PREVIOUS:
      next.selected = clampPaletteSelection(next.selected - 1, rowCount);
      break;
  }
  next.error = '';
  return next;
}

// Apply a tree section delta to the retained tree, mirroring the C++
// TreeDeltaCodec::replay (src/TreeModel.cpp): a per-provider splice (erase
// erase_count nodes at start, insert the new views) plus provider add/remove and
// the selected id. Returns true when the tree is now current (applied, or a no-op
// delta), and false ONLY when the client cannot express the transition and must
// re-sync from a full snapshot: an explicit snapshot_required, a base revision that
// does not match the retained tree (a missed delta), a duplicate or malformed
// provider change. Replay is transactional -- it validates against a working copy
// and commits to `tree` only on full success, so a rejected delta never leaves the
// panel half-applied. Keeping the panel retained here is what stops every
// expand/open/select from forcing a resync.
export function normalizeTreeActiveBinding(tree) {
  if (!tree) return false;
  const providers = Array.isArray(tree.providers) ? tree.providers : [];
  const key = (v) => (typeof v === 'bigint' ? v.toString() : String(v));
  if (new Set(providers.map((provider) => key(provider.provider_id))).size !==
      providers.length) return false;
  if (!Object.prototype.hasOwnProperty.call(tree, 'active_binding')) {
    tree.active_binding = providers.length === 0 ? null : {
      provider_id: providers[0].provider_id, kind: providers[0].kind,
    };
  }
  const active = tree.active_binding;
  if ((providers.length === 0) !== (active == null)) return false;
  if (active == null) return true;
  return providers.filter((provider) =>
    key(provider.provider_id) === key(active.provider_id) &&
    num(provider.kind) === num(active.kind)).length === 1;
}

export function applyTreeDelta(tree, treeDelta) {
  if (!treeDelta) return true;
  if (treeDelta.snapshot_required) return false;
  if (!tree) return false;
  const big = (v) => (typeof v === 'bigint' ? v : BigInt(v == null ? 0 : v));
  if (big(treeDelta.base_revision) !== big(tree.revision)) return false;
  const key = (v) => (typeof v === 'bigint' ? v.toString() : String(v));
  // Work on a copy (providers cloned, each provider's nodes copied) so a later
  // malformed change cannot leave the retained tree partially mutated.
  const base = Array.isArray(tree.providers) ? tree.providers : [];
  const providers = base.map((p) => ({ ...p, nodes: Array.isArray(p.nodes) ? p.nodes.slice() : [] }));
  const seen = new Set();
  for (const change of (treeDelta.providers || [])) {
    const pid = key(change.provider_id);
    if (seen.has(pid)) return false;  // C++ rejects two changes for one provider
    seen.add(pid);
    const idx = providers.findIndex((p) => key(p.provider_id) === pid);
    const start = Number(big(change.start));
    const erase = Number(big(change.erase_count));
    const insert = Array.isArray(change.insert) ? change.insert : [];
    if (change.remove_provider) {
      // A removal carries no splice payload; a nonzero one is malformed.
      if (idx < 0 || start !== 0 || erase !== 0 || insert.length !== 0) return false;
      providers.splice(idx, 1);
      continue;
    }
    if (idx < 0) {
      // A new provider inserts at its sorted position (the C++ keeps providers
      // ordered by id) and carries only inserts, never an erase.
      if (start !== 0 || erase !== 0) return false;
      let at = providers.findIndex((p) => key(p.provider_id) > pid);
      if (at < 0) at = providers.length;
      providers.splice(at, 0, { provider_id: change.provider_id, kind: change.kind,
        nodes: insert.slice(), selected: change.selected == null ? null : change.selected });
      continue;
    }
    const prov = providers[idx];
    if (start > prov.nodes.length || erase > prov.nodes.length - start) return false;
    prov.nodes.splice(start, erase, ...insert);
    prov.kind = change.kind;
    prov.selected = change.selected == null ? null : change.selected;
  }
  // Commit.
  const order = Array.isArray(treeDelta.provider_order)
    ? treeDelta.provider_order.map(key) : null;
  if (!order || order.length !== providers.length ||
      new Set(order).size !== providers.length) return false;
  const byId = new Map(providers.map((provider) => [key(provider.provider_id), provider]));
  if (order.some((id) => !byId.has(id))) return false;
  providers.splice(0, providers.length, ...order.map((id) => byId.get(id)));
  if (byId.size !== providers.length) return false;
  const active = Object.prototype.hasOwnProperty.call(treeDelta, 'active_binding')
    ? treeDelta.active_binding
    : (providers.length === 0 ? null : {
        provider_id: providers[0].provider_id, kind: providers[0].kind,
      });
  if ((providers.length === 0) !== (active == null)) return false;
  if (active != null) {
    const matches = providers.filter((provider) =>
      key(provider.provider_id) === key(active.provider_id) &&
      num(provider.kind) === num(active.kind));
    if (matches.length !== 1) return false;
  }
  tree.providers = providers;
  tree.active_binding = active;
  tree.revision = treeDelta.revision;
  return normalizeTreeActiveBinding(tree);
}

export function clampPaletteSelection(selected, rowCount) {
  const count = Math.max(0, Number(rowCount) || 0);
  if (count === 0) return 0;
  const index = Math.max(0, Number(selected) || 0);
  return Math.min(index, count - 1);
}


export function applySessionDeltaSections(sections, delta) {
  if (!sections || !delta) return sections;
  if (delta.document) {
    sections.document.text = applyDocumentDelta(sections.document.text, delta.document);
    if (delta.document_caret != null) sections.document.caret = num(delta.document_caret);
  } else if (delta.document_caret != null) {
    sections.document.caret = num(delta.document_caret);
  }
  if (delta.selection && delta.selection.replacement != null) sections.selection = delta.selection.replacement;
  if (delta.tabs && delta.tabs.state != null) sections.tabs = delta.tabs.state;
  if (delta.syntax && delta.syntax.spans != null) {
    if (!sections.syntax) sections.syntax = {};
    sections.syntax.spans = delta.syntax.spans;
  }
  if (delta.theme && delta.theme.replacement != null) sections.theme = delta.theme.replacement;
  const replaceWrapped = (name) => { if (delta[name] && delta[name].replacement != null) sections[name] = delta[name].replacement; };
  replaceWrapped('prompt_status');
  replaceWrapped('find_replace');
  // The footer prompt's semantic projection travels as a changed-flagged delta
  // (like a snapshot section replacement, but a null replacement means the prompt
  // CLOSED, which replaceWrapped's non-null guard would wrongly ignore). A frame
  // with changed=false carries no replacement and leaves the prior view intact.
  if (delta.prompt_view && num(delta.prompt_view.changed)) {
    sections.prompt_view = delta.prompt_view.replacement != null ? delta.prompt_view.replacement : null;
  }
  // The draft-conflict notice travels as a changed-flagged delta exactly like the
  // footer prompt: a null replacement means the notice CLEARED, so it must not go
  // through replaceWrapped's non-null guard. changed=false leaves the prior notice.
  if (delta.notice_view && num(delta.notice_view.changed)) {
    sections.notice_view = delta.notice_view.replacement != null ? delta.notice_view.replacement : null;
  }
  // The external-modification section travels as a merge delta (upserted/removed/
  // selected against a base revision), like the diff section it mirrors. Merge it
  // into the retained section so the bar tracks live changes without a full
  // snapshot.
  if (delta.external_modification) {
    sections.external_modification =
      applyExternalModificationDelta(sections.external_modification, delta.external_modification);
  }
  if (delta.ui_frame_delta != null && sections.ui_frame != null) {
    const frame = applyUiFrameDelta(sections.ui_frame, delta.ui_frame_delta);
    if (frame) sections.ui_frame = frame;
  }
  if (delta.palette != null) sections.palette = delta.palette;
  return sections;
}

const sameValue = (left, right) => {
  const encode = (value) => JSON.stringify(
    value, (_, item) => typeof item === 'bigint' ? item.toString() : item);
  return encode(left) === encode(right);
};

const has = (value, key) =>
  value != null && Object.prototype.hasOwnProperty.call(value, key);

function changedReplacement(current, delta, nullable = false) {
  if (!delta) return { accepted: true, value: current };
  const changed = !!num(delta.changed);
  const replacementPresent = has(delta, 'replacement');
  if (!changed) {
    return replacementPresent && delta.replacement != null
      ? { accepted: false, value: current }
      : { accepted: true, value: current };
  }
  if (!replacementPresent || (!nullable && delta.replacement == null)) {
    return { accepted: false, value: current };
  }
  return { accepted: true, value: delta.replacement };
}

function revisionReplacement(current, delta) {
  if (!delta) return { accepted: true, value: current };
  if (!current ||
      BigInt(delta.base_revision) !== BigInt(current.revision) ||
      BigInt(delta.revision) < BigInt(delta.base_revision)) {
    return { accepted: false, value: current };
  }
  if (delta.state == null) {
    return BigInt(delta.revision) === BigInt(delta.base_revision)
      ? { accepted: true, value: current }
      : { accepted: false, value: current };
  }
  return BigInt(delta.state.revision) === BigInt(delta.revision)
    ? { accepted: true, value: delta.state }
    : { accepted: false, value: current };
}

function applySettingsDelta(current, delta) {
  if (!delta) return { accepted: true, value: current };
  const entries = (current?.entries || []).map((entry) => ({ ...entry }));
  for (const change of (delta.changes || [])) {
    const key = num(change.key);
    const entry = entries[key];
    if (!entry || !sameValue(entry.effective, change.before)) {
      return { accepted: false, value: current };
    }
    entry.effective = change.after;
  }
  return { accepted: true, value: { ...current, entries } };
}

function applyIdMerge(current, delta, itemKey, selectionKey = null) {
  if (!delta || !current ||
      BigInt(delta.base_revision) !== BigInt(current.revision) ||
      BigInt(delta.revision) < BigInt(delta.base_revision)) {
    return { accepted: false, value: current };
  }
  const key = (value) => String(value == null ? '' : value);
  const items = Array.isArray(current[itemKey]) ? current[itemKey] : [];
  const byId = new Map(items.map((item) => [key(item.id), item]));
  const changed = new Set();
  for (const id of (delta.removed || [])) {
    const normalized = key(id);
    if (changed.has(normalized) || !byId.delete(normalized)) {
      return { accepted: false, value: current };
    }
    changed.add(normalized);
  }
  for (const item of (delta.upserted || [])) {
    const normalized = key(item.id);
    if (!normalized || changed.has(normalized)) {
      return { accepted: false, value: current };
    }
    changed.add(normalized);
    byId.set(normalized, item);
  }
  const value = { ...current, revision: delta.revision,
                  [itemKey]: [...byId.values()] };
  if (has(delta, 'message')) value.message = delta.message;
  if (selectionKey) {
    value[selectionKey] = delta.selected == null ? null : delta.selected;
    if (value[selectionKey] != null && !byId.has(key(value[selectionKey]))) {
      return { accepted: false, value: current };
    }
  }
  return { accepted: true, value };
}

function uiNodeIds(schema) {
  if (!schema?.root) return null;
  const ids = [];
  const visit = (node) => {
    if (!node || typeof node.id !== 'string') return false;
    ids.push(node.id);
    for (const child of (node.container?.children || [])) {
      if (!visit(child)) return false;
    }

    return true;
  };
  return visit(schema.root) ? ids : null;
}

function uiNodeById(node, id) {
  if (!node || typeof node.id !== 'string') return null;
  if (node.id === id) return node;
  for (const child of (node.container?.children || [])) {
    const found = uiNodeById(child, id);
    if (found) return found;
  }
  return null;
}

function effectiveUiPresence(schema, presence) {
  const direct = new Map((presence?.nodes || []).map((record) =>
    [record.id, !!record.present]));
  const effective = new Map();
  const visit = (node, ancestorsPresent) => {
    if (!node || typeof node.id !== 'string' || !direct.has(node.id)) {
      return false;
    }
    const present = ancestorsPresent && direct.get(node.id) === true;
    effective.set(node.id, present);
    return (node.container?.children || []).every(
      (child) => visit(child, present));
  };
  return visit(schema?.root, true) ? effective : null;
}

function validUiFrame(frame) {
  const schema = frame?.schema;
  const state = frame?.state;
  const presence = frame?.presence;
  const ids = uiNodeIds(schema);
  if (!ids || !state || !presence ||
      !frame.version ||
      num(frame.version.generation) !== num(schema.generation) ||
      num(frame.version.presence_basis) !== num(presence.basis) ||
      num(schema.generation) !== num(state.generation) ||
      num(schema.generation) !== num(presence.generation)) return false;
  const expected = new Set(ids);
  const stateIds = (state.nodes || []).map((node) => node.id);
  const presenceIds = (presence.nodes || []).map((node) => node.id);
  if (!(expected.size === ids.length &&
    stateIds.length === ids.length && presenceIds.length === ids.length &&
    stateIds.every((id) => expected.has(id)) &&
    presenceIds.every((id) => expected.has(id)) &&
    new Set(stateIds).size === stateIds.length &&
    new Set(presenceIds).size === presenceIds.length)) return false;

  const path = state.focus_path;
  if (!Array.isArray(path) || path.length === 0 ||
      path.some((id) => {
        const context = num(uiNodeById(schema.root, id)?.focus_context);
        return !expected.has(id) || !Number.isInteger(context) ||
          context < 0 || context > 3;
      })) return false;
  const baseContext = num(uiNodeById(schema.root, path[0])?.focus_context);
  if (baseContext !== 0 && baseContext !== 1) return false;
  const effective = effectiveUiPresence(schema, presence);
  return effective?.get(path[path.length - 1]) === true;
}

function sameUiFrameVersion(left, right) {
  return !!left && !!right &&
    num(left.generation) === num(right.generation) &&
    num(left.presence_basis) === num(right.presence_basis);
}

export function applyUiFrameDelta(frame, delta) {
  if (!validUiFrame(frame) || !delta ||
      !sameUiFrameVersion(frame.version, delta.base)) return null;
  if (delta.kind === 'replacement') {
    return validUiFrame(delta.frame) &&
      sameUiFrameVersion(delta.frame.version, delta.target) &&
      num(delta.target.generation) !== num(delta.base.generation)
      ? delta.frame : null;
  }
  if (delta.kind !== 'changes' ||
      num(delta.target?.generation) !== num(delta.base.generation) ||
      num(delta.target?.presence_basis) < num(delta.base.presence_basis) ||
      num(delta.state?.generation) !== num(delta.target.generation) ||
      num(delta.presence?.generation) !== num(delta.target.generation) ||
      num(delta.presence?.basis) !== num(delta.target.presence_basis)) return null;

  const stateChanges = delta.state.nodes || [];
  const presenceChanges = delta.presence.nodes || [];
  const known = new Set(uiNodeIds(frame.schema));
  const stateIds = stateChanges.map((record) => record.id);
  const presenceIds = presenceChanges.map((record) => record.id);
  if (new Set(stateIds).size !== stateIds.length ||
      new Set(presenceIds).size !== presenceIds.length ||
      stateIds.some((id) => !known.has(id)) ||
      presenceIds.some((id) => !known.has(id)) ||
      (presenceChanges.length > 0 &&
       num(delta.target.presence_basis) <= num(delta.base.presence_basis))) {
    return null;
  }
  const replace = (records, changes) => {
    const byId = new Map(changes.map((record) => [record.id, record]));
    return records.map((record) => byId.get(record.id) || record);
  };
  const candidate = {
    version: delta.target,
    schema: frame.schema,
    state: {
      ...frame.state,
      nodes: replace(frame.state.nodes, stateChanges),
      focus_path: delta.state.focus_path == null
        ? frame.state.focus_path : delta.state.focus_path,
    },
    presence: {
      ...frame.presence,
      basis: delta.target.presence_basis,
      nodes: replace(frame.presence.nodes, presenceChanges),
    },
  };
  return validUiFrame(candidate) ? candidate : null;
}

function normalizeUiFrameSections(sections) {
  const legacyNames = ['ui', 'ui_state', 'ui_presence'];
  const legacyPresent = legacyNames.filter((name) =>
    Object.prototype.hasOwnProperty.call(sections, name));
  if (sections.ui_frame != null) {
    if (legacyPresent.length !== 0 || !validUiFrame(sections.ui_frame)) {
      return false;
    }
    const pair = uiFrameLegacyFocusPair(sections.ui_frame);
    const external = sections.external_focus_held == null
      ? false : strictBoolean(sections.external_focus_held);
    return pair != null &&
      sections.focus != null && num(sections.focus) === pair.focus &&
      external != null && external === pair.external;
  }
  if (legacyPresent.length === 0) return true;
  if (legacyPresent.length !== legacyNames.length ||
      legacyNames.some((name) => sections[name] == null)) return false;
  const schema = structuredClone(sections.ui);
  const legacyContexts = new Map([
    ['editor', 0], ['panel', 1], ['input_line', 2],
    ['footer.prompt', 2], ['externalmod', 3],
  ]);
  const annotate = (node) => {
    if (!node || typeof node.id !== 'string') return;
    // Preceding schemas had no focus metadata; known host identity supplies it.
    if (legacyContexts.has(node.id)) {
      node.focus_context = legacyContexts.get(node.id);
    }
    for (const child of (node.container?.children || [])) annotate(child);
  };
  annotate(schema.root);
  const state = { ...sections.ui_state };
  const external = sections.external_focus_held == null
    ? false : strictBoolean(sections.external_focus_held);
  if (sections.focus == null || external == null) return false;
  if (state.focus_path == null) {
    const provisional = {
      schema,
      state,
      presence: sections.ui_presence,
    };
    state.focus_path = legacyFocusPath(
      provisional, num(sections.focus),
      external);
    if (!state.focus_path) return false;
  }
  const frame = {
    version: {
      generation: sections.ui.generation,
      presence_basis: sections.ui_presence.basis,
    },
    schema,
    state,
    presence: sections.ui_presence,
  };
  if (!validUiFrame(frame)) return false;
  sections.ui_frame = frame;
  for (const name of legacyNames) delete sections[name];
  return true;
}

function uiFrameLegacyFocusPair(frame) {
  const resolved = resolveUiFocusPath(
    frame?.schema, frame?.state, frame?.presence);
  if (!resolved) return null;
  const contexts = new Map();
  const collect = (node) => {
    if (!node || typeof node.id !== 'string') return;
    contexts.set(node.id, num(node.focus_context));
    for (const child of (node.container?.children || [])) collect(child);
  };
  collect(frame.schema.root);
  let focus = null;
  for (let index = resolved.path.length - 1; index >= 0; --index) {
    const context = contexts.get(resolved.path[index]);
    if (context !== 3) {
      focus = context;
      break;
    }
  }
  return focus == null || focus < 0 || focus > 2 ? null : {
    focus,
    external: resolved.context === 'external',
  };
}

function legacyFocusPath(frame, focus, external) {
  if (!Number.isInteger(focus) || focus < 0 || focus > 2 ||
      (focus === 2 && external)) {
    return null;
  }
  const base = focus === 1 ? 'panel' : 'editor';
  const baseNode = uiNodeById(frame.schema.root, base);
  if (num(baseNode?.focus_context) !== (focus === 1 ? 1 : 0)) return null;
  const effective = effectiveUiPresence(frame.schema, frame.presence);
  if (!effective) return null;
  const path = [base];
  if (focus === 2) {
    const prompts = ['input_line', 'footer.prompt'].filter((id) =>
      num(uiNodeById(frame.schema.root, id)?.focus_context) === 2 &&
      effective.get(id) === true);
    if (prompts.length !== 1) return null;
    path.push(prompts[0]);
  } else if (effective.get(base) !== true) {
    return null;
  }
  if (external) {
    if (num(uiNodeById(frame.schema.root, 'externalmod')?.focus_context) !== 3 ||
        effective.get('externalmod') !== true) {
      return null;
    }
    path.push('externalmod');
  }
  return path;
}

function frameDeltaChangesFocus(delta) {
  if (delta?.ui_frame_delta != null) {
    return delta.ui_frame_delta.kind === 'replacement' ||
      delta.ui_frame_delta.state?.focus_path != null;
  }
  return delta?.ui_state?.focus_path != null;
}

function strictBoolean(value) {
  return typeof value === 'boolean' ? value : null;
}

function reconcileLegacyFocusDelta(baseFrame, candidate, delta) {
  const hasFocus = delta.focus != null;
  const hasExternal = delta.external_focus_held != null;
  if (!hasFocus && !hasExternal) return candidate;
  const external = hasExternal
    ? strictBoolean(delta.external_focus_held) : null;
  if (hasExternal && external == null) return null;
  const actual = uiFrameLegacyFocusPair(candidate);
  if (!actual) return null;
  if (frameDeltaChangesFocus(delta)) {
    if ((hasFocus && actual.focus !== num(delta.focus)) ||
        (hasExternal && actual.external !== external)) {
      return null;
    }
    return candidate;
  }
  const base = uiFrameLegacyFocusPair(baseFrame);
  if (!base) return null;
  const focus = hasFocus ? num(delta.focus) : base.focus;
  const requestedExternal = hasExternal ? external : base.external;
  const path = legacyFocusPath(candidate, focus, requestedExternal);
  if (!path) return null;
  const reconciled = {
    ...candidate,
    state: { ...candidate.state, focus_path: path },
  };
  return validUiFrame(reconciled) ? reconciled : null;
}

function validSyntaxState(state) {
  const textBytes = num(state?.text_bytes);
  if (!state || textBytes < 0 || !Array.isArray(state.spans) ||
      !Array.isArray(state.bracket_pairs) ||
      !Array.isArray(state.unmatched_brackets) ||
      !Array.isArray(state.comment_tokens) ||
      !Array.isArray(state.comment_ranges) ||
      !Array.isArray(state.indentation) || state.indentation.length === 0) {
    return false;
  }
  let cursor = 0;
  for (const span of state.spans) {
    const begin = num(span.begin);
    const end = num(span.end);
    if (begin !== cursor || begin >= end || end > textBytes) return false;
    cursor = end;
  }
  if (cursor !== textBytes) return false;
  let previous = -1;
  for (const pair of state.bracket_pairs) {
    const open = num(pair.open);
    const close = num(pair.close);
    if (open >= close || close >= textBytes || open <= previous) return false;
    previous = open;
  }
  previous = -1;
  for (const bracket of state.unmatched_brackets) {
    const offset = num(bracket.offset);
    if (offset >= textBytes || offset <= previous) return false;
    previous = offset;
  }
  let previousBegin = -1;
  let previousEnd = -1;
  for (const token of state.comment_tokens) {
    const begin = num(token.range?.begin);
    const end = num(token.range?.end);
    if (begin >= end || end > textBytes ||
        (begin < previousBegin ||
         (begin === previousBegin && end <= previousEnd))) return false;
    previousBegin = begin;
    previousEnd = end;
  }
  previous = 0;
  for (const range of state.comment_ranges) {
    const begin = num(range.range?.begin);
    const end = num(range.range?.end);
    if (begin < previous || begin >= end || end > textBytes) return false;
    previous = end;
  }
  previous = -1;
  for (let index = 0; index < state.indentation.length; ++index) {
    const line = state.indentation[index];
    const lineStart = num(line.line_start);
    const contentStart = num(line.content_start);
    if (num(line.line) !== index || lineStart > contentStart ||
        contentStart > textBytes || lineStart <= previous) return false;
    previous = lineStart;
  }
  return true;
}

// Transactionally apply every semantic section. Unchanged sections retain their
// identity; failure returns null and leaves the retained frame untouched.
export function applySessionDeltaCopy(sections, delta) {
  if (!sections || !delta) return null;
  const next = { ...sections };
  if (delta.document) {
    const document = delta.document;
    if (BigInt(document.base_revision) !== BigInt(sections.document.revision) ||
        BigInt(document.revision) === BigInt(document.base_revision) &&
          (num(document.erased_bytes) !== 0 || document.inserted_text !== '')) {
      return null;
    }
    const start = num(document.start);
    const erased = num(document.erased_bytes);
    const total = utf8Bytes(sections.document.text);
    const boundaries = byteToIndex(
      sections.document.text, [start, start + erased]);
    if (start < 0 || erased < 0 || start + erased > total ||
        !boundaries.has(start) || !boundaries.has(start + erased)) return null;
    next.document = {
      ...sections.document,
      text: applyDocumentDelta(sections.document.text, document),
      revision: document.revision,
      diff_file_identity: document.diff_file_identity,
    };
  } else if (delta.document_caret != null) {
    next.document = { ...sections.document };
  }
  if (delta.document_caret != null) {
    const caret = num(delta.document_caret);
    if (caret < 0 || caret > utf8Bytes(next.document.text) ||
        !byteToIndex(next.document.text, [caret]).has(caret)) return null;
    next.document.caret = delta.document_caret;
  }

  const replacements = [
    ['selection', false], ['history', false], ['clipboard', false],
    ['prompt_status', false], ['keymap', false],
    ['prompt_view', true], ['notice_view', true],
  ];
  for (const [name, nullable] of replacements) {
    const replayed = changedReplacement(sections[name], delta[name], nullable);
    if (!replayed.accepted) return null;
    next[name] = replayed.value;
  }
  const find = delta.find_replace;
  if (find) {
    if (num(find.base_generation) !== num(sections.find_replace.generation)) {
      return null;
    }
    const replayed = changedReplacement(sections.find_replace, find);
    if (!replayed.accepted ||
        (replayed.value !== sections.find_replace &&
         num(replayed.value.generation) < num(find.base_generation))) return null;
    next.find_replace = replayed.value;
  }
  for (const name of ['search', 'lsp_sync', 'lsp_features']) {
    const replayed = revisionReplacement(sections[name], delta[name]);
    if (!replayed.accepted) return null;
    next[name] = replayed.value;
  }
  const settings = applySettingsDelta(sections.settings, delta.settings);
  if (!settings.accepted) return null;
  next.settings = settings.value;
  if (delta.text_encoding) {
    if (!sameValue(sections.text_encoding, delta.text_encoding.before)) return null;
    next.text_encoding = delta.text_encoding.after;
  }
  if (delta.tabs?.state != null) next.tabs = delta.tabs.state;

  const diff = applyIdMerge(sections.diff, delta.diff, 'files');
  if (delta.diff && !diff.accepted) return null;
  if (delta.diff) next.diff = diff.value;
  const external = applyIdMerge(
    sections.external_modification, delta.external_modification,
    'files', 'selected');
  if (delta.external_modification && !external.accepted) return null;
  if (delta.external_modification) next.external_modification = external.value;

  if (delta.follow_edits) {
    if (num(delta.follow_edits.base_generation) !==
        num(sections.follow_edits.generation)) return null;
    if (delta.follow_edits.replacement == null) {
      if (num(delta.follow_edits.generation) !==
          num(delta.follow_edits.base_generation)) return null;
    } else {
      if (num(delta.follow_edits.replacement.generation) !==
          num(delta.follow_edits.generation)) return null;
      next.follow_edits = delta.follow_edits.replacement;
    }
  }
  if (delta.tree) {
    next.tree = { ...sections.tree };
    if (!applyTreeDelta(next.tree, delta.tree)) return null;
  }
  if (delta.syntax) {
    const baseRevision = BigInt(delta.syntax.base_revision);
    const revision = BigInt(delta.syntax.revision);
    const members = [
      'language', 'text_bytes', 'spans', 'bracket_pairs',
      'unmatched_brackets', 'comment_tokens', 'comment_ranges', 'indentation',
    ];
    const changed = members.some((member) => delta.syntax[member] != null);
    if (baseRevision !== BigInt(sections.syntax.revision) ||
        revision < baseRevision || (revision === baseRevision && changed)) {
      return null;
    }
    const syntax = { ...sections.syntax, revision: delta.syntax.revision };
    for (const member of members) {
      if (delta.syntax[member] != null) syntax[member] = delta.syntax[member];
    }
    if (!validSyntaxState(syntax)) return null;
    next.syntax = syntax;
  }
  if (delta.theme?.replacement != null) next.theme = delta.theme.replacement;
  if (delta.palette != null) {
    next.palette = delta.palette.replacement ?? delta.palette;
  }
  const legacyUiNames = ['ui', 'ui_state', 'ui_presence'];
  const hasLegacyUi = legacyUiNames.some((name) =>
    Object.prototype.hasOwnProperty.call(delta, name));
  if (delta.ui_frame_delta != null && hasLegacyUi) return null;
  if (delta.ui_frame_delta != null) {
    next.ui_frame = applyUiFrameDelta(sections.ui_frame, delta.ui_frame_delta);
    if (!next.ui_frame) return null;
  } else if (hasLegacyUi) {
    const schema = delta.ui ?? sections.ui_frame?.schema;
    const stateReplacement = delta.ui_state ?? sections.ui_frame?.state;
    const presence = delta.ui_presence ?? sections.ui_frame?.presence;
    if (!schema || !stateReplacement || !presence) return null;
    const state = stateReplacement.focus_path == null
      ? { ...stateReplacement, focus_path: sections.ui_frame.state.focus_path }
      : stateReplacement;
    const frame = {
      version: {
        generation: schema.generation,
        presence_basis: presence.basis,
      },
      schema, state, presence,
    };
    if (!validUiFrame(frame)) return null;
    next.ui_frame = frame;
  }
  if (sections.ui_frame != null || next.ui_frame != null) {
    next.ui_frame = reconcileLegacyFocusDelta(
      sections.ui_frame, next.ui_frame, delta);
    if (!next.ui_frame) return null;
  }
  if (delta.watcher_available != null) {
    next.watcher_available = !!num(delta.watcher_available);
  }
  delete next.focus;
  delete next.external_focus_held;
  return next;
}

export function applySessionDelta(sections, revision, delta) {
  if (!sections) return { kind: 'missing-state' };
  if (!deltaIsContiguous(revision, delta)) return { kind: 'revision-gap' };
  const next = applySessionDeltaCopy(sections, delta);
  if (!next) return { kind: 'semantic-rejection' };
  return {
    kind: 'accepted',
    sections: next,
    revision: BigInt(delta.revision),
  };
}

// The draft-conflict notice's geometry-free semantic projection normalized for the
// renderer, or null when the active document raises no notice. Owns the snake_case
// wire coupling (command) so the client draws the notice bar and its clickable
// action labels without re-deriving field names.
export function noticeViewFromSections(sections) {
  if (!sections) return null;
  const nv = sections.notice_view;
  if (nv == null) return null;
  const actions = (nv.actions || []).map((a) => ({
    id: String(a.id == null ? '' : a.id),
    label: String(a.label == null ? '' : a.label),
    command: String(a.command == null ? '' : a.command),
  }));
  return { text: String(nv.text == null ? '' : nv.text), actions };
}

// Merge an external-modification delta (base_revision/revision/upserted/removed/
// selected/message) into the retained section, mirroring the
// C++ ExternalModificationDeltaCodec.replay: removed ids drop, upserted files
// replace-or-add by id, and the selection re-homes to the delta's value. Pure.
export function applyExternalModificationDelta(section, delta) {
  if (!delta) return section;
  const base = section && Array.isArray(section.files)
    ? section : { revision: 0, message: '', files: [], selected: null };
  const byId = new Map(base.files.map((f) => [String(f.id), f]));
  for (const id of (delta.removed || [])) byId.delete(String(id));
  for (const f of (delta.upserted || [])) byId.set(String(f.id), f);
  return {
    revision: num(delta.revision),
    message: typeof delta.message === 'string' ? delta.message : base.message,
    files: [...byId.values()],
    selected: delta.selected != null ? String(delta.selected) : null,
  };
}

// The external-modification bar's geometry-free projection for the renderer, or
// null when no file is externally changed (the bar is absent). Owns the wire
// coupling while preserving the library-published labels and command affordances.
export function externalModificationFromSections(sections) {
  if (!sections) return null;
  const section = sections.external_modification;
  if (!section || !Array.isArray(section.files) || section.files.length === 0) return null;
  const selectedId = section.selected != null ? String(section.selected) : null;
  const files = section.files.map((f) => {
    const id = String(f.id == null ? '' : f.id);
    const actions = (f.actions || []).map((a) => ({
      action: num(a.action),
      label: String(a.label == null ? '' : a.label),
      command: String(a.command == null ? '' : a.command),
    }));
    return {
      id,
      path: String(f.path == null ? '' : f.path),
      statusLabel: String(f.status_label == null ? '' : f.status_label),
      accessibleStatus: String(f.accessible_status == null ? '' : f.accessible_status),
      selected: id === selectedId,
      actions,
    };
  });
  return { message: String(section.message == null ? '' : section.message), files };
}

// Whether the external-modification bar holds the effective keyboard focus. The
// wire `focus` field is NEVER ExternalModification (it is legacy-projected to
// Editor/Panel/Prompt for older clients); this additive bool is the ONLY signal,
// so the client MUST read it rather than compare the focus ordinal to a value
// that never appears on the wire.
// --- UI-VM: the web interpreter over the published schema + dynamic node state ---
//
// Wire ordinals, pinned by the C++ enums (WidgetKind, RegionRole, Axis, SizeKind,
// SemanticRole). The schema's leaves carry `kind`; regions carry `role`; nodes carry
// `size`; containers carry `axis`.
export const WIDGET = { CONTAINER: 0, LABEL: 1, FIELD: 2, CHECKBOX: 3, TEXT_INPUT: 4, SPACER: 5, VIEW: 6, STATUS_ACTIONS: 7 };
export const AXIS = { ROW: 0, COLUMN: 1 };
export const SIZE = { EXACT: 0, FLEX: 1, AUTO: 2, RESPONSIVE: 3 };
// Whether a node is an independent scroll viewport, pinned to the C++ ScrollAxis
// enum. An unrecognized value is treated as NONE (a future axis degrades to "not
// a viewport"), matching the wire decoder's forward-compat rule.
export const SCROLL = { NONE: 0, VERTICAL: 1 };
// Opaque client-rendered surfaces a View leaf may name, pinned to the C++ ViewSurface enum.
export const SURFACE = { TABBAR: 0, FILETREE: 1, GITSTATUS: 2, FINDRESULTS: 3, SYMBOLS: 4, FOOTER_PROMPT: 5, NOTICE: 6, EXTERNAL_MODIFICATION: 7, DOCUMENT: 8, TREE: 9 };
export const PALETTE_PRESENCE_OP = { SHOW: 0, HIDE: 1 };
const ALL_SURFACES = Object.freeze([
  SURFACE.TABBAR, SURFACE.TREE, SURFACE.FINDRESULTS, SURFACE.NOTICE,
  SURFACE.EXTERNAL_MODIFICATION, SURFACE.DOCUMENT,
]);
const TREE_SURFACES = Object.freeze([
  SURFACE.TREE,
]);

export function browserRenderPlan(delta) {
  const surfaces = new Set();
  const add = (...values) => values.forEach((value) => surfaces.add(value));
  const replacementChanged = (value) =>
    !!value && (value.replacement != null || !!num(value.changed));
  const revisionChanged = (value) =>
    !!value && BigInt(value.base_revision == null ? 0 : value.base_revision) !==
      BigInt(value.revision == null ? 0 : value.revision);
  if (delta.document || delta.document_caret != null ||
      replacementChanged(delta.selection) ||
      (delta.syntax && delta.syntax.spans != null)) {
    add(SURFACE.DOCUMENT);
  }
  if (delta.tabs && delta.tabs.state != null) add(SURFACE.TABBAR);
  if (revisionChanged(delta.tree)) add(...TREE_SURFACES);
  if (delta.palette) add(SURFACE.FINDRESULTS);
  if (delta.notice_view && !!num(delta.notice_view.changed))
    add(SURFACE.NOTICE);
  if (revisionChanged(delta.external_modification))
    add(SURFACE.EXTERNAL_MODIFICATION);
  const repaintTheme =
    !!(delta.theme && delta.theme.replacement != null);
  if (repaintTheme) add(...ALL_SURFACES);
  return {
    rebuild: delta.ui_frame_delta?.kind === 'replacement',
    reconcile: !!(delta.ui_frame_delta ||
                   replacementChanged(delta.prompt_status) || repaintTheme),
    responsive: delta.ui_frame_delta?.kind === 'replacement' ||
      (delta.ui_frame_delta?.presence?.nodes || []).length > 0,
    repaintTheme,
    surfaces: [...surfaces].sort((a, b) => a - b),
  };
}

export function mergeBrowserRenderPlans(left, right) {
  return {
    rebuild: !!left.rebuild || !!right.rebuild,
    reconcile: !!left.reconcile || !!right.reconcile,
    responsive: !!left.responsive || !!right.responsive,
    repaintTheme: !!left.repaintTheme || !!right.repaintTheme,
    surfaces: [...new Set([...(left.surfaces || []), ...(right.surfaces || [])])],
  };
}

export function applyPalettePresenceOverlay(schema, presence, overlay) {
  if (!schema || !schema.root || !presence || !overlay) {
    return { presence, error: 'missing picker presence overlay', stale: false };
  }
  if (num(schema.generation) !== num(overlay.generation)) {
    return { presence, error: null, stale: true };
  }

  const parentById = new Map();
  const schemaIds = new Set();
  const collect = (node, parent = null) => {
    if (!node || schemaIds.has(node.id)) return false;
    schemaIds.add(node.id);
    parentById.set(node.id, parent);
    for (const child of (node.container && node.container.children) || []) {
      if (!collect(child, node.id)) return false;
    }
    return true;
  };
  if (!collect(schema.root)) {
    return { presence, error: 'invalid picker schema', stale: false };
  }

  const records = new Map(
    (presence.nodes || []).map((record) => [record.id, record]));
  const overrides = new Map();
  for (const op of (overlay.ops || [])) {
    const kind = num(op.kind);
    if (!schemaIds.has(op.target) || !records.has(op.target) ||
        overrides.has(op.target) ||
        (kind !== PALETTE_PRESENCE_OP.SHOW &&
         kind !== PALETTE_PRESENCE_OP.HIDE)) {
      return { presence, error: 'invalid picker presence overlay', stale: false };
    }
    overrides.set(op.target, kind === PALETTE_PRESENCE_OP.SHOW);
  }
  for (const [target, shown] of overrides) {
    if (!shown) continue;
    for (let parent = parentById.get(target); parent != null;
         parent = parentById.get(parent)) {
      if (overrides.get(parent) === false) {
        return {
          presence, error: 'contradictory picker presence overlay',
          stale: false,
        };
      }
    }
  }

  return {
    presence: {
      ...presence,
      nodes: (presence.nodes || []).map((record) =>
        overrides.has(record.id)
          ? { ...record, present: overrides.get(record.id) }
          : record),
    },
    error: null,
    stale: false,
  };
}

const samePointerBasis = (left, right) =>
  !!left && !!right && left.text === right.text && left.tab === right.tab;

export function settlePointerSelection(inFlight, queued, error, currentBasis) {
  if (queued && samePointerBasis(queued.basis, currentBasis)) {
    return { dispatch: queued, preview: queued };
  }
  if (error === 3 && inFlight && inFlight.retries === 0 &&
      samePointerBasis(inFlight.basis, currentBasis)) {
    const retry = { ...inFlight, retries: 1 };
    return { dispatch: retry, preview: retry };
  }
  return { dispatch: null, preview: null };
}

export function webExtentCss(value, axis) {
  return axis === AXIS.COLUMN
    ? `calc(${value} * var(--ssg-row))`
    : `${value}ch`;
}

export function responsiveFlexCss(size, axis) {
  const minimum = size.minimum || 0;
  const preferred = size.extent || minimum;
  const shrink = size.optional && preferred > 0
    ? (preferred - minimum) / preferred : 0;
  return {
    flex: `${size.growth || 0} ${shrink} ${webExtentCss(preferred, axis)}`,
    minimumProperty: axis === AXIS.COLUMN ? 'minHeight' : 'minWidth',
    minimumValue: webExtentCss(minimum, axis),
  };
}

export function responsiveSurvivors(children, availableExtent, gap) {
  const responsive = children.some((child) =>
    child.size && child.size.kind === SIZE.RESPONSIVE);
  if (!responsive) return new Set(children.map((child) => child.id));
  if (!Number.isSafeInteger(availableExtent) || availableExtent < 0 ||
      !Number.isSafeInteger(gap) || gap < 0 ||
      children.some((child) => child.size && child.size.kind === SIZE.AUTO)) {
    return null;
  }
  const active = children.map(() => true);
  const floor = (child) => {
    const size = child.size || {};
    if (size.kind === SIZE.EXACT) return size.extent || 0;
    if (size.kind === SIZE.FLEX) return 0;
    return size.minimum || 0;
  };
  const required = () => {
    let count = 0;
    let total = 0;
    for (let index = 0; index < children.length; index++) {
      if (!active[index]) continue;
      count++;
      total += floor(children[index]);
    }
    return total + Math.max(count - 1, 0) * gap;
  };
  while (required() > availableExtent) {
    let drop = -1;
    for (let index = children.length - 1; index >= 0; index--) {
      const size = children[index].size || {};
      if (active[index] && size.kind === SIZE.RESPONSIVE && size.optional) {
        drop = index;
        break;
      }
    }
    if (drop < 0) return null;
    active[drop] = false;
  }
  return new Set(children.filter((_, index) => active[index])
                         .map((child) => child.id));
}

export class GenerationRetainedCache {
  constructor() {
    this.generation = null;
    this.entries = new Map();
  }
  begin(generation) {
    if (this.generation === generation) return false;
    this.generation = generation;
    this.entries.clear();
    return true;
  }
  getOrCreate(id, create) {
    if (!this.entries.has(id)) this.entries.set(id, create());
    return this.entries.get(id);
  }
  get(id) { return this.entries.get(id); }
  values() { return this.entries.values(); }
}

export function gitAffordanceFromNode(node) {
  const value = node && node.git_status;
  if (!value) return null;
  return {
    shortLabel: String(value.short_label == null ? '' : value.short_label),
    role: num(value.role),
  };
}

export function preferredKeyboardSurface(surfaces) {
  const present = new Set(surfaces || []);
  if (present.has(SURFACE.FINDRESULTS)) return SURFACE.FINDRESULTS;
  if (present.has(SURFACE.DOCUMENT)) return SURFACE.DOCUMENT;
  return null;
}

export function predictedFocusCapture({
  localMode,
  authoritativeActivation,
}) {
  return localMode != null && authoritativeActivation == null
    ? 'input_line' : null;
}

export function resolveUiFocusPath(schema, state, presence, predictedNode = null) {
  if (!schema || !state || !presence ||
      num(schema.generation) !== num(state.generation) ||
      num(schema.generation) !== num(presence.generation)) {
    return null;
  }
  if (state.focus_path == null) return undefined;
  if (!Array.isArray(state.focus_path) || state.focus_path.length === 0) {
    return null;
  }
  const schemaNodes = new Map();
  const collect = (node) => {
    if (!node || typeof node.id !== 'string') return;
    schemaNodes.set(node.id, node);
    for (const child of (node.container && node.container.children) || []) {
      collect(child);
    }
  };
  collect(schema.root);
  const present = effectiveUiPresence(schema, presence);
  if (!present) return null;
  const validContext = (node) => {
    const context = num(node?.focus_context);
    return Number.isInteger(context) && context >= 0 && context <= 3;
  };
  if (state.focus_path.some((id) =>
        typeof id !== 'string' || !validContext(schemaNodes.get(id)))) {
    return null;
  }
  if (state.focus_path[0] !== 'editor' &&
      state.focus_path[0] !== 'panel') {
    return null;
  }
  if (predictedNode != null &&
      (!validContext(schemaNodes.get(predictedNode)) ||
       !present.get(predictedNode))) {
    return null;
  }
  const effectivePath = [...state.focus_path];
  if (predictedNode != null &&
      effectivePath[effectivePath.length - 1] !== predictedNode) {
    effectivePath.push(predictedNode);
  }
  const effective = effectivePath[effectivePath.length - 1];
  if (!present.get(effective)) return null;
  return {
    path: effectivePath,
    effective,
    context: ['editor', 'panel', 'prompt', 'external'][
      num(schemaNodes.get(effective).focus_context)],
    captured: effectivePath.length > 1,
  };
}

export function focusUiNode(nodeId, findNode) {
  if (nodeId == null) return false;
  const node = findNode(nodeId);
  if (!node) return false;
  node.tabIndex = -1;
  node.focus({ preventScroll: true });
  return true;
}

const STRUCTURAL_ROLE = { prompt: 16 };
const structuralRole = (name) => Object.prototype.hasOwnProperty.call(STRUCTURAL_ROLE, name)
  ? STRUCTURAL_ROLE[name] : null;

// The primitives THIS web build's interpreter can draw. Header prompt TextInput
// state is browser-local; footer prompt TextInputs resolve through UiState.
// Placement is tree structure + well-known node ids, so there is no region-role set.
export const WEB_UI_PROFILE = {
  widgets: new Set([WIDGET.CONTAINER, WIDGET.LABEL, WIDGET.FIELD, WIDGET.CHECKBOX, WIDGET.TEXT_INPUT, WIDGET.SPACER, WIDGET.VIEW, WIDGET.STATUS_ACTIONS]),
  surfaces: new Set([SURFACE.TABBAR, SURFACE.TREE, SURFACE.FINDRESULTS, SURFACE.NOTICE, SURFACE.EXTERNAL_MODIFICATION, SURFACE.DOCUMENT]),
  sizes: new Set(Object.values(SIZE)),
};

// The first schema primitive `profile` does not support, as
// { kind: 'widget'|'surface'|'size', ordinal }, or null when every leaf widget,
// view surface, and size kind is supported.
export function firstUnsupportedPrimitive(schema, profile = WEB_UI_PROFILE) {
  if (!schema || !schema.root) return null;
  const surfaces = profile.surfaces || new Set();
  const sizes = profile.sizes || new Set();
  const walk = (node) => {
    if (!node) return null;
    const size = node.size && node.size.kind != null
      ? num(node.size.kind) : SIZE.EXACT;
    if (!sizes.has(size)) return { kind: 'size', ordinal: size };
    if (node.leaf && typeof node.leaf === 'object') {
      const kind = num(node.leaf.kind);
      if (!profile.widgets.has(kind)) return { kind: 'widget', ordinal: kind };
      if (kind === WIDGET.VIEW) {
        const surface = num(node.leaf.surface);
        if (!surfaces.has(surface)) return { kind: 'surface', ordinal: surface };
      }
    }
    if (node.container && Array.isArray(node.container.children)) {
      for (const child of node.container.children) {
        const bad = walk(child);
        if (bad) return bad;
      }
    }
    return null;
  };
  return walk(schema.root);
}

const semanticRoleCount = 28;
const validSemanticRole = (value) =>
  (typeof value === 'number' || typeof value === 'bigint') &&
  Number.isInteger(num(value)) && num(value) >= 0 &&
  num(value) < semanticRoleCount;

export function firstMalformedNodeStyle(schema) {
  if (!schema || !schema.root) return null;
  const walk = (node) => {
    if (!node) return null;
    if (node.style !== undefined) {
      if (!node.style || typeof node.style !== 'object' ||
          Array.isArray(node.style)) return { kind: 'style', id: node.id };
      for (const channel of ['foreground', 'background']) {
        if (node.style[channel] !== undefined &&
            !validSemanticRole(node.style[channel])) {
          return { kind: 'style', id: node.id };
        }
      }
    }
    if (node.container && Array.isArray(node.container.children)) {
      for (const child of node.container.children) {
        const bad = walk(child);
        if (bad) return bad;
      }
    }
    return null;
  };
  return walk(schema.root);
}

// Interpret the schema (a single root node) + dynamic state (resolved values/presence)
// into a RENDER TREE (rooted at `root`) the DOM builder mirrors 1:1 -- the generic
// container tree is preserved (axis, gap, the left/middle/right grouping, and the FULL
// published sizing: each node carries its Size {kind, extent} and each container its
// Inset), so packing follows the published tree rather than a flattened, flex-only item
// list. Each node:
//   container: { id, kind:'container', axis, gap, size, inset, children:[...] }
//   leaf:      { id, kind:'leaf', widget, size, text, checked?, command?, role,
//                spacer?, surface?, actions? }
// A non-present node (and its subtree) is omitted; a Label/Field with no resolved leaf
// state is the resolved drop and is omitted; a Checkbox always renders; a Spacer renders
// a gap. `role` is the effective SemanticRole ordinal the SERVER resolved (the widget's
// own role or the area default) and published in the dynamic state, so the client
// colors from theme.role_colors by ordinal and never re-derives a role name.
//
// Returns null (do not interpret; wait for a consistent frame) when the schema and
// state are from different frames (different generation, or a node-id set that does not
// correspond one-to-one), when a primitive is unsupported, or when a state record's
// SHAPE disagrees with its schema node (a container or spacer carrying leaf state, a
// checkbox missing leaf state or missing its `checked`, or a Label/Field leaf state that
// carries `checked`) -- a malformed frame is never partially drawn. `root` is the
// interpreted render root; its children are the well-known areas (id "header"/"footer").
export function interpretChrome(schema, state, presence, profile = WEB_UI_PROFILE) {
  if (!schema || !schema.root || !state || !presence) return null;
  if (firstUnsupportedPrimitive(schema, profile)) return null;
  if (num(schema.generation) !== num(state.generation)) return null;
  // Presence is a separate basis-stamped section; it must name the same generation.
  if (num(schema.generation) !== num(presence.generation)) return null;

  const stateById = new Map();
  for (const n of (state.nodes || [])) stateById.set(n.id, n);
  const presentById = new Map();
  for (const p of (presence.nodes || [])) presentById.set(p.id, !!num(p.present));

  // Node-id correspondence: exactly the schema's ids, one-to-one with BOTH the
  // state and the presence section.
  const schemaIds = [];
  const collect = (node) => {
    if (!node) return;
    schemaIds.push(node.id);
    if (node.container && Array.isArray(node.container.children))
      for (const c of node.container.children) collect(c);
  };
  collect(schema.root);
  if ((state.nodes || []).length !== schemaIds.length) return null;
  if ((presence.nodes || []).length !== schemaIds.length) return null;
  for (const id of schemaIds) { if (!stateById.has(id) || !presentById.has(id)) return null; }

  let shapeOk = true;
  // The published sizing, validated before the DOM builder consumes it.
  const sizeOf = (node) => {
    const s = node.size && typeof node.size === 'object' ? node.size : {};
    const uint = (value, fallback = null) => {
      if (value === undefined) return fallback;
      const converted = num(value);
      return Number.isSafeInteger(converted) && converted >= 0
        ? converted : null;
    };
    const kind = uint(s.kind, SIZE.EXACT);
    const extent = uint(s.extent, 0);
    if (kind === null || extent === null) return null;
    if (kind === SIZE.EXACT || kind === SIZE.FLEX || kind === SIZE.AUTO) {
      return { kind, extent };
    }
    if (kind !== SIZE.RESPONSIVE ||
        s.extent === undefined ||
        typeof s.optional !== 'boolean') return null;
    const minimum = uint(s.minimum);
    const growth = uint(s.growth);
    if (minimum === null || growth === null) return null;
    const validOptional =
      s.optional && growth === 0 && extent > 0 && minimum <= extent;
    const validFlex =
      !s.optional && growth === 1 && extent === minimum;
    if (!validOptional && !validFlex) return null;
    // Responsive reuses the legacy extent slot as its preferred size.
    return { kind, extent, minimum, growth, optional: s.optional };
  };
  const insetOf = (container) => {
    const i = container.inset && typeof container.inset === 'object' ? container.inset : {};
    return { left: num(i.left) || 0, right: num(i.right) || 0,
             top: num(i.top) || 0, bottom: num(i.bottom) || 0 };
  };
  // A scroll axis the client does not recognize degrades to NONE, matching the
  // The scroll axis a node's container declares. ABSENCE (undefined) is None and
  // an unrecognized numeric ordinal degrades to None (a future axis renders as
  // "not a viewport"), matching the C++ wire decoder. A PRESENT field that is null
  // or the wrong TYPE is malformed and returns null so the caller rejects the
  // frame, exactly as the C++ decoder fails a present non-uint scroll.
  const scrollOf = (container) => {
    const raw = container.scroll;
    if (raw === undefined) return SCROLL.NONE;  // absent only
    if (typeof raw !== 'number' && typeof raw !== 'bigint') return null;  // null/wrong type -> malformed
    return num(raw) === SCROLL.VERTICAL ? SCROLL.VERTICAL : SCROLL.NONE;
  };
  const styleOf = (node, inherited) => {
    if (node.style === undefined) return { ...inherited };
    if (!node.style || typeof node.style !== 'object' ||
        Array.isArray(node.style)) return null;
    const resolved = { ...inherited };
    for (const channel of ['foreground', 'background']) {
      if (node.style[channel] === undefined) continue;
      const value = node.style[channel];
      if (!validSemanticRole(value)) return null;
      resolved[channel] = num(value);
    }
    return resolved;
  };
  const build = (node, inheritedStyle = {}) => {
    const style = styleOf(node, inheritedStyle);
    if (style === null) { shapeOk = false; return null; }
    const size = sizeOf(node);
    if (size === null) { shapeOk = false; return null; }
    const st = stateById.get(node.id);
    const hasLeafState = st.leaf != null && typeof st.leaf === 'object';
    const isContainer = node.container != null && typeof node.container === 'object';
    // Shape validation runs regardless of presence, so a malformed frame is caught.
    if (isContainer) {
      if (hasLeafState) { shapeOk = false; return null; }
      const directSizes = (node.container.children || []).map(sizeOf);
      if (directSizes.some((childSize) => childSize === null) ||
          (directSizes.some((childSize) => childSize.kind === SIZE.RESPONSIVE) &&
           directSizes.some((childSize) => childSize.kind === SIZE.AUTO))) {
        shapeOk = false; return null;
      }
      const scroll = scrollOf(node.container);
      if (scroll === null) { shapeOk = false; return null; }  // malformed scroll type
      const children = [];
      for (const c of (node.container.children || [])) {
        const built = build(c, style);
        if (built) children.push(built);
      }
      if (!presentById.get(node.id)) return null;  // hidden subtree not drawn
      return { id: node.id, kind: 'container', axis: num(node.container.axis),
               gap: num(node.container.gap) || 0, size,
               scroll, inset: insetOf(node.container), style, children };
    }
    if (!node.leaf || typeof node.leaf !== 'object') { shapeOk = false; return null; }
    const wk = num(node.leaf.kind);
    if ((wk === WIDGET.SPACER || wk === WIDGET.VIEW ||
         wk === WIDGET.STATUS_ACTIONS) && hasLeafState) {
      shapeOk = false; return null;
    }
    if (wk === WIDGET.CHECKBOX) {
      // A checkbox must carry leaf state AND a resolved `checked`.
      if (!hasLeafState || st.leaf.checked == null ||
          st.leaf.active != null) { shapeOk = false; return null; }
    } else if (wk === WIDGET.LABEL || wk === WIDGET.FIELD) {
      // A Label/Field must NOT carry `checked` -- that field is a checkbox's alone.
      if (hasLeafState &&
          (st.leaf.checked != null || st.leaf.active != null)) {
        shapeOk = false; return null;
      }
    } else if (wk === WIDGET.TEXT_INPUT) {
      // Header picker input is state-free and browser-local. A footer input is
      // stateful and explicitly names whether it owns prompt keyboard input.
      if (hasLeafState &&
          (st.leaf.checked != null || st.leaf.active == null)) {
        shapeOk = false; return null;
      }
    }
    if (!presentById.get(node.id)) return null;  // hidden leaf not drawn
    if (wk === WIDGET.SPACER) {
      const w = node.leaf.width != null ? num(node.leaf.width) : null;
      return { id: node.id, kind: 'leaf', widget: wk, spacer: true,
               width: w, size, style };
    }
    if (wk === WIDGET.VIEW) {
      return { id: node.id, kind: 'leaf', widget: wk, size,
               surface: num(node.leaf.surface), style };
    }
    if (wk === WIDGET.STATUS_ACTIONS) {
      return { id: node.id, kind: 'leaf', widget: wk, size,
               actions: [], style };
    }
    if (wk === WIDGET.TEXT_INPUT) {
      if (hasLeafState) {
        return { id: node.id, kind: 'leaf', widget: wk, size,
                 controlId: node.leaf.id, text: st.leaf.value || '',
                 label: st.leaf.label || '', active: !!num(st.leaf.active),
                 command: st.leaf.command != null ? st.leaf.command : null,
                 role: node.leaf.role != null || style.foreground == null
                   ? num(st.leaf.role) : null,
                 style };
      }
      // The header query anchor remains state-free: browser-owned local text
      // produces no per-keystroke tree delta.
      return { id: node.id, kind: 'leaf', widget: wk, size,
               role: structuralRole(node.leaf.role),
               sigil: node.leaf.sigil || '', style };
    }
    if (wk === WIDGET.CHECKBOX) {
      return { id: node.id, kind: 'leaf', widget: wk, size,
               role: node.leaf.role != null || style.foreground == null
                 ? num(st.leaf.role) : null,
               text: st.leaf.value || '', label: st.leaf.label || '',
               checked: !!num(st.leaf.checked),
               command: st.leaf.command != null ? st.leaf.command : null,
               style };
    }
    // Label/Field: no leaf state is the resolved drop (not drawn, not an error).
    if (!hasLeafState) return null;
    return { id: node.id, kind: 'leaf', widget: wk, size,
             role: node.leaf.role != null || style.foreground == null
               ? num(st.leaf.role) : null,
             text: st.leaf.value || '', label: st.leaf.label || '',
             command: st.leaf.command != null ? st.leaf.command : null,
             style };
  };

  const rootNode = build(schema.root);
  if (!shapeOk) return null;
  return { root: rootNode };
}
