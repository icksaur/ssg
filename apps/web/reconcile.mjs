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
        typeof node.document.text === 'string') return node;
    for (const k of Object.keys(node)) { const f = findSections(node[k]); if (f) return f; }
  }
  return null;
}

export const num = (v) => typeof v === 'bigint' ? Number(v) : v;
export const hex2 = (n) => (n & 255).toString(16).padStart(2, '0');
export const cssColor = (c) => c ? ('#' + hex2(num(c.red)) + hex2(num(c.green)) + hex2(num(c.blue))) : '';

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

export function encodeValue(value) {
  const enc = new TextEncoder();
  if (value == null) return new Uint8Array([0]);
  if (typeof value === 'boolean') return new Uint8Array([1, value ? 1 : 0]);
  if (typeof value === 'number' || typeof value === 'bigint') {
    return concat([new Uint8Array([3]), u64(value)]);
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
  return concat([new Uint8Array([2, kind]), encodeValue(payload)]);
}

export function decodeMessage(buffer) {
  const dv = new DataView(buffer);
  if (dv.byteLength > PROTOCOL_LIMITS.messageBytes) {
    throw new Error('protocol message length exceeded');
  }
  if (dv.byteLength < 3 || dv.getUint8(0) !== 2) {
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

export function encodeClientInput({ code = '', control = false, alt = false,
                                    meta = false, shift = false, text = '' }) {
  const stroke = code ? { code, control, alt, meta, shift } : null;
  return encodeMessage(7, { stroke, committed_text: text });
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

export function encodeStatusActionInvocation({ statusId, actionId, generation }) {
  return encodeMessage(5, {
    status_id: BigInt(statusId), action_id: actionId,
    generation: BigInt(generation),
  });
}

export const PICKER_MODE = Object.freeze({ FILE: 0, COMMAND: 4 });

export const encodePickerSubmit = (mode, candidateId, revision) =>
  encodeCommandRequest('picker.submit', revision,
                       { mode: BigInt(mode), candidate_id: String(candidateId) });

export const encodeSelectionByteRange = (anchor, active, revision) =>
  encodeCommandRequest('select.set_byte_range', revision, {
    anchor_byte_offset: BigInt(anchor),
    active_byte_offset: BigInt(active),
  });

export const encodeTabAction = (commandId, tabId, revision) => {
  if (commandId !== 'tab.activate' && commandId !== 'tab.close') {
    throw new TypeError('unsupported tab action');
  }
  return encodeCommandRequest(commandId, revision, BigInt(tabId));
};

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

export const encodeTreeActivation = (nodeId, revision) =>
  encodeCommandRequest('tree.activate_node', revision,
                       { node_id: String(nodeId) });

export const encodePromptFocus = (index, revision) =>
  encodeCommandRequest('prompt.focus_control', revision,
                       { index: BigInt(index) });

export const encodeExternalAction = (action, fileId, revision) =>
  encodeCommandRequest('external.invoke_action', revision,
                       { file_id: String(fileId), action: BigInt(action) });

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

function sameStroke(bindingStroke, inputStroke) {
  return bindingStroke && inputStroke &&
    bindingStroke.code === inputStroke.code &&
    !!bindingStroke.control === !!inputStroke.control &&
    !!bindingStroke.alt === !!inputStroke.alt &&
    !!bindingStroke.meta === !!inputStroke.meta &&
    !!bindingStroke.shift === !!inputStroke.shift;
}

export function resolvePickerLifecycle(keymap, palette, inputStroke, context) {
  if (!keymap || !Array.isArray(keymap.bindings) || !palette) return null;
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
  if (resolved == null) return null;
  if (resolved.command_id === palette.command_open_command_id) {
    return PICKER_MODE.COMMAND;
  }
  if (resolved.command_id === palette.file_open_command_id) {
    return PICKER_MODE.FILE;
  }
  return null;
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
  tree.providers = providers;
  tree.revision = treeDelta.revision;
  return true;
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
  if (delta.focus != null) sections.focus = num(delta.focus);
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
  // snapshot. `external_focus_held` is an additive top-level bool, present only
  // when it flips; absent leaves the prior value.
  if (delta.external_modification) {
    sections.external_modification =
      applyExternalModificationDelta(sections.external_modification, delta.external_modification);
  }
  if (delta.external_focus_held != null) {
    sections.external_focus_held = !!num(delta.external_focus_held);
  }
  const replaceDirect = (name) => { if (delta[name] != null) sections[name] = delta[name]; };
  replaceDirect('ui');
  replaceDirect('ui_state');
  replaceDirect('ui_presence');
  replaceDirect('palette');
  return sections;
}

// PromptControlKind ordinals (C++ PromptControlKind).
export const PROMPT_CONTROL = { INPUT: 0, TOGGLE: 1, COUNT: 2 };

// Decide how the persistent footer-prompt container moves keyboard focus as the
// published PromptView changes, without a DOM so the rule is testable: focus the
// container ONLY on open and on active-input change (never per message, so an
// unrelated frame cannot steal deliberate focus and a re-render preserves focus +
// aria-activedescendant), and restore the captured focus on close. `prev` is
// { open, activeInput }; `pv` is the promptViewFromSections result (null when
// closed). Returns { open, activeInput, focusContainer, restoreFocus,
// activeDescendant }.
export function promptFocusPlan(prev, pv) {
  const was = prev || { open: false, activeInput: -1 };
  if (!pv) {
    return { open: false, activeInput: -1, focusContainer: false,
             restoreFocus: !!was.open, activeDescendant: null };
  }
  const focusContainer = !was.open || was.activeInput !== pv.activeInput;
  return { open: true, activeInput: pv.activeInput, focusContainer,
           restoreFocus: false, activeDescendant: pv.activeInput };
}


// The active footer prompt's geometry-free semantic projection normalized for the
// renderer, or null when no footer-region prompt is open. Owns the snake_case wire
// coupling (accessible_label, active_input) so the client draws controls without
// re-deriving field names. `activeInput` indexes the INPUT controls only.
export function promptViewFromSections(sections) {
  if (!sections) return null;
  const pv = sections.prompt_view;
  if (pv == null) return null;
  const controls = (pv.controls || []).map((c) => ({
    kind: num(c.kind),
    id: String(c.id == null ? '' : c.id),
    label: String(c.accessible_label == null ? '' : c.accessible_label),
    value: String(c.value == null ? '' : c.value),
    checked: !!c.checked,
    command: String(c.command == null ? '' : c.command),
  }));
  return {
    kind: num(pv.kind),
    label: String(pv.accessible_label == null ? '' : pv.accessible_label),
    controls,
    activeInput: num(pv.active_input),
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
export function externalFocusHeld(sections) {
  return !!(sections && num(sections.external_focus_held));
}

// --- UI-VM: the web interpreter over the published schema + dynamic node state ---
//
// Wire ordinals, pinned by the C++ enums (WidgetKind, RegionRole, Axis, SizeKind,
// SemanticRole). The schema's leaves carry `kind`; regions carry `role`; nodes carry
// `size`; containers carry `axis`.
export const WIDGET = { CONTAINER: 0, LABEL: 1, FIELD: 2, CHECKBOX: 3, TEXT_INPUT: 4, SPACER: 5, VIEW: 6, STATUS_ACTIONS: 7 };
export const AXIS = { ROW: 0, COLUMN: 1 };
export const SIZE = { EXACT: 0, FLEX: 1, AUTO: 2 };
// Whether a node is an independent scroll viewport, pinned to the C++ ScrollAxis
// enum. An unrecognized value is treated as NONE (a future axis degrades to "not
// a viewport"), matching the wire decoder's forward-compat rule.
export const SCROLL = { NONE: 0, VERTICAL: 1 };
// Opaque client-rendered surfaces a View leaf may name, pinned to the C++ ViewSurface enum.
export const SURFACE = { TABBAR: 0, FILETREE: 1, GITSTATUS: 2, FINDRESULTS: 3, SYMBOLS: 4, FOOTER_PROMPT: 5, NOTICE: 6, EXTERNAL_MODIFICATION: 7, DOCUMENT: 8 };
export const PALETTE_PRESENCE_OP = { SHOW: 0, HIDE: 1 };
const ALL_SURFACES = Object.freeze(Object.values(SURFACE));
const TREE_SURFACES = Object.freeze([
  SURFACE.FILETREE, SURFACE.GITSTATUS, SURFACE.SYMBOLS,
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
  if (delta.prompt_view && !!num(delta.prompt_view.changed))
    add(SURFACE.FOOTER_PROMPT);
  if (delta.notice_view && !!num(delta.notice_view.changed))
    add(SURFACE.NOTICE);
  if (revisionChanged(delta.external_modification))
    add(SURFACE.EXTERNAL_MODIFICATION);
  const repaintTheme =
    !!(delta.theme && delta.theme.replacement != null);
  if (repaintTheme) add(...ALL_SURFACES);
  return {
    rebuild: !!delta.ui,
    reconcile: !!(delta.ui || delta.ui_state || delta.ui_presence ||
                   replacementChanged(delta.prompt_status) || repaintTheme),
    repaintTheme,
    surfaces: [...surfaces].sort((a, b) => a - b),
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
const STRUCTURAL_ROLE = { prompt: 16 };
const structuralRole = (name) => Object.prototype.hasOwnProperty.call(STRUCTURAL_ROLE, name)
  ? STRUCTURAL_ROLE[name] : null;

// The primitives THIS web build's interpreter can draw. The built-in prompt
// TextInput (the header query anchor) carries no server leaf state -- the browser
// owns the query text locally -- so it renders as a bare anchor node. Placement is
// tree structure + well-known node ids, so there is no region-role set.
export const WEB_UI_PROFILE = {
  widgets: new Set([WIDGET.CONTAINER, WIDGET.LABEL, WIDGET.FIELD, WIDGET.CHECKBOX, WIDGET.TEXT_INPUT, WIDGET.SPACER, WIDGET.VIEW, WIDGET.STATUS_ACTIONS]),
  surfaces: new Set([SURFACE.TABBAR, SURFACE.FILETREE, SURFACE.GITSTATUS, SURFACE.FINDRESULTS, SURFACE.SYMBOLS, SURFACE.FOOTER_PROMPT, SURFACE.NOTICE, SURFACE.EXTERNAL_MODIFICATION, SURFACE.DOCUMENT]),
};

// The first schema primitive `profile` does not support, as
// { kind: 'widget'|'surface', ordinal }, or null when every leaf widget kind and
// view surface is supported. Placement is a property of tree structure + well-known
// node ids, so there is no region-role check. The interpreter runs only when this
// returns null.
export function firstUnsupportedPrimitive(schema, profile = WEB_UI_PROFILE) {
  if (!schema || !schema.root) return null;
  const surfaces = profile.surfaces || new Set();
  const walk = (node) => {
    if (!node) return null;
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
  // The published sizing, carried verbatim so the DOM builder honors every constraint:
  // { kind, extent } (Exact => a fixed extent, Flex => a grow weight, Auto => content).
  const sizeOf = (node) => {
    const s = node.size && typeof node.size === 'object' ? node.size : {};
    return { kind: num(s.kind), extent: s.extent != null ? num(s.extent) : 0 };
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
  const build = (node) => {
    const st = stateById.get(node.id);
    const hasLeafState = st.leaf != null && typeof st.leaf === 'object';
    const isContainer = node.container != null && typeof node.container === 'object';
    // Shape validation runs regardless of presence, so a malformed frame is caught.
    if (isContainer) {
      if (hasLeafState) { shapeOk = false; return null; }
      const scroll = scrollOf(node.container);
      if (scroll === null) { shapeOk = false; return null; }  // malformed scroll type
      const children = [];
      for (const c of (node.container.children || [])) {
        const built = build(c);
        if (built) children.push(built);
      }
      if (!presentById.get(node.id)) return null;  // hidden subtree not drawn
      return { id: node.id, kind: 'container', axis: num(node.container.axis),
               gap: num(node.container.gap) || 0, size: sizeOf(node),
               scroll, inset: insetOf(node.container), children };
    }
    if (!node.leaf || typeof node.leaf !== 'object') { shapeOk = false; return null; }
    const wk = num(node.leaf.kind);
    if ((wk === WIDGET.SPACER || wk === WIDGET.VIEW || wk === WIDGET.STATUS_ACTIONS ||
         wk === WIDGET.TEXT_INPUT) && hasLeafState) {
      shapeOk = false; return null;
    }
    if (wk === WIDGET.CHECKBOX) {
      // A checkbox must carry leaf state AND a resolved `checked`.
      if (!hasLeafState || st.leaf.checked == null) { shapeOk = false; return null; }
    } else if (wk === WIDGET.LABEL || wk === WIDGET.FIELD) {
      // A Label/Field must NOT carry `checked` -- that field is a checkbox's alone.
      if (hasLeafState && st.leaf.checked != null) { shapeOk = false; return null; }
    }
    if (!presentById.get(node.id)) return null;  // hidden leaf not drawn
    if (wk === WIDGET.SPACER) {
      const w = node.leaf.width != null ? num(node.leaf.width) : null;
      return { id: node.id, kind: 'leaf', widget: wk, spacer: true, width: w, size: sizeOf(node) };
    }
    if (wk === WIDGET.VIEW) {
      return { id: node.id, kind: 'leaf', widget: wk, size: sizeOf(node),
               surface: num(node.leaf.surface) };
    }
    if (wk === WIDGET.STATUS_ACTIONS) {
      return { id: node.id, kind: 'leaf', widget: wk, size: sizeOf(node), actions: [] };
    }
    if (wk === WIDGET.TEXT_INPUT) {
      // The prompt query anchor: no server text (the browser owns the query
      // locally), so no per-keystroke tree delta is ever produced.
      return { id: node.id, kind: 'leaf', widget: wk, size: sizeOf(node),
               role: structuralRole(node.leaf.role), sigil: node.leaf.sigil || '' };
    }
    if (wk === WIDGET.CHECKBOX) {
      return { id: node.id, kind: 'leaf', widget: wk, size: sizeOf(node), role: num(st.leaf.role),
               text: st.leaf.value || '', checked: !!num(st.leaf.checked),
               command: st.leaf.command != null ? st.leaf.command : null };
    }
    // Label/Field: no leaf state is the resolved drop (not drawn, not an error).
    if (!hasLeafState) return null;
    return { id: node.id, kind: 'leaf', widget: wk, size: sizeOf(node), role: num(st.leaf.role),
             text: st.leaf.value || '',
             command: st.leaf.command != null ? st.leaf.command : null };
  };

  const rootNode = build(schema.root);
  if (!shapeOk) return null;
  return { root: rootNode };
}
