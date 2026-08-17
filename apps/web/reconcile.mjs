// Pure client logic with no DOM dependency, so a node test can exercise the wire
// decode, the UTF-8 <-> UTF-16 offset mapping, and (from M3) the local-echo
// reconciliation exactly as the browser runs them. Nothing here touches
// document/window; client.mjs owns all rendering and I/O.

// Decode one tagged ProtocolValue from a DataView at {p}. Returns [value, next].
// Mirrors protocol/schema/README.md's tag encoding.
export function decodeValue(dv, p) {
  const tag = dv.getUint8(p); p += 1;
  switch (tag) {
    case 0: return [null, p];
    case 1: return [dv.getUint8(p) !== 0, p + 1];
    case 2: { const v = dv.getBigInt64(p, true); return [v, p + 8]; }
    case 3: { const v = dv.getBigUint64(p, true); return [v, p + 8]; }
    case 4: case 5: {
      const n = dv.getUint32(p, true); p += 4;
      const bytes = new Uint8Array(dv.buffer, dv.byteOffset + p, n); p += n;
      if (tag === 4) return [new TextDecoder().decode(bytes), p];
      return [bytes.slice(), p];
    }
    case 6: {
      const n = dv.getUint32(p, true); p += 4; const arr = [];
      for (let i = 0; i < n; i++) { const [v, np] = decodeValue(dv, p); arr.push(v); p = np; }
      return [arr, p];
    }
    case 7: {
      const n = dv.getUint32(p, true); p += 4; const obj = {};
      for (let i = 0; i < n; i++) {
        const kl = dv.getUint32(p, true); p += 4;
        const key = new TextDecoder().decode(new Uint8Array(dv.buffer, dv.byteOffset + p, kl)); p += kl;
        const [v, np] = decodeValue(dv, p); obj[key] = v; p = np;
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

export function encodeStatusActionInvocation({ statusId, actionId, generation }) {
  const enc = new TextEncoder();
  const chunks = [new Uint8Array([1, 5])];
  const u32 = (n) => { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, n, true); return b; };
  const u64 = (n) => { const b = new Uint8Array(8); new DataView(b.buffer).setBigUint64(0, BigInt(n), true); return b; };
  const text = (s) => { const b = enc.encode(s); return [new Uint8Array([4]), u32(b.length), b]; };
  const uint = (n) => [new Uint8Array([3]), u64(n)];
  const field = (k, parts) => { const kb = enc.encode(k); chunks.push(u32(kb.length), kb, ...parts); };
  chunks.push(new Uint8Array([7]), u32(3));
  field('status_id', uint(statusId));
  field('action_id', text(actionId));
  field('generation', uint(generation));
  const out = new Uint8Array(chunks.reduce((n, c) => n + c.length, 0));
  let p = 0;
  for (const c of chunks) { out.set(c, p); p += c.length; }
  return out;
}

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

// Drop every prediction the host has settled (applied or rejected): both leave
// the authoritative document as truth, so re-basing keeps only ids beyond it.
export const dropSettled = (pending, settledId) =>
  pending.filter((p) => num(p.id) > num(settledId));

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

// Parse the host envelope: [u64 LE settledClientEditId][u8 sectionCount], then
// sectionCount sections each [u8 tag][u32 LE length][bytes]. Returns the settled
// id and each section as a tag plus a DataView over its bytes, so the caller
// decodes tag 0 (library body) and tag 1 (palette report JSON) each its own way.
export function parseEnvelope(buffer) {
  const dv = new DataView(buffer);
  const settledId = dv.getBigUint64(0, true);
  let p = 8;
  const count = dv.getUint8(p); p += 1;
  const sections = [];
  for (let i = 0; i < count; i++) {
    const tag = dv.getUint8(p); p += 1;
    const len = dv.getUint32(p, true); p += 4;
    sections.push({ tag, dv: new DataView(buffer, p, len), length: len });
    p += len;
  }
  return { settledId, sections };
}


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

export function encodePaletteSubmit(candidateId) {
  if (candidateId == null || candidateId === '') return null;
  return 'PSUB:' + String(candidateId);
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
  const replaceDirect = (name) => { if (delta[name] != null) sections[name] = delta[name]; };
  replaceDirect('ui');
  replaceDirect('ui_state');
  replaceDirect('ui_presence');
  replaceDirect('palette');
  return sections;
}

// FocusTarget::Prompt and PromptKind::Palette ordinals, and the wire field names
// the decoded snapshot uses. The sections object is the decoded ProtocolValue
// tree, so its keys are the encoder's snake_case names (prompt_status,
// active_kind) -- NOT camelCase. This one function owns that coupling so a
// mis-spelling cannot silently hide the palette overlay again.
export const FOCUS_PROMPT = 2;
export const PROMPT_PALETTE = 5;

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

// The host ingress that focuses a clicked footer-prompt input by its input index,
// dispatched to prompt.focus_control server-side. One place owns the wire string.
export function promptFocusControlMessage(index) {
  return 'PFOC:' + index;
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
export function isPalettePromptOpen(sections) {
  if (!sections) return false;
  if (num(sections.focus) !== FOCUS_PROMPT) return false;
  const ps = sections.prompt_status;
  return !!ps && ps.active_kind != null && num(ps.active_kind) === PROMPT_PALETTE;
}

// The browser owns the palette query locally; it must clear that local text on a
// FRESH open. A fresh open is either a closed->open transition OR a reopen at the
// same open state signalled by a changed picker epoch (the server bumps the epoch
// each time the picker is opened). Keeping this pure and separate makes the
// reset-on-reopen rule testable without the DOM or a live socket.
export function shouldResetLocalQuery(nowOpen, wasOpen, epoch, lastEpoch) {
  if (!nowOpen) return false;
  if (!wasOpen) return true;
  return pickerEpochValue(epoch) !== pickerEpochValue(lastEpoch);
}

function pickerEpochValue(raw) {
  if (typeof raw === 'bigint' && raw >= 0n) return raw;
  if (typeof raw === 'number' && Number.isSafeInteger(raw) && raw >= 0) return BigInt(raw);
  throw new TypeError('malformed palette picker_epoch');
}

export function pickerEpochFromPalette(palette) {
  if (!palette || !Object.prototype.hasOwnProperty.call(palette, 'picker_epoch') ||
      palette.picker_epoch == null) {
    return 0n;
  }
  return pickerEpochValue(palette.picker_epoch);
}

// --- UI-VM: the web interpreter over the published schema + dynamic node state ---
//
// Wire ordinals, pinned by the C++ enums (WidgetKind, RegionRole, Axis, SizeKind,
// SemanticRole). The schema's leaves carry `kind`; regions carry `role`; nodes carry
// `size`; containers carry `axis`.
export const WIDGET = { CONTAINER: 0, LABEL: 1, FIELD: 2, CHECKBOX: 3, TEXT_INPUT: 4, SPACER: 5, VIEW: 6, STATUS_ACTIONS: 7 };
export const AXIS = { ROW: 0, COLUMN: 1 };
export const SIZE = { EXACT: 0, FLEX: 1, AUTO: 2 };
// Opaque client-rendered surfaces a View leaf may name, pinned to the C++ ViewSurface enum.
export const SURFACE = { TABVIEW: 0, FILETREE: 1, GITSTATUS: 2, FINDRESULTS: 3, SYMBOLS: 4, FOOTER_PROMPT: 5 };
const STRUCTURAL_ROLE = { prompt: 16 };
const structuralRole = (name) => Object.prototype.hasOwnProperty.call(STRUCTURAL_ROLE, name)
  ? STRUCTURAL_ROLE[name] : null;

// The primitives THIS web build's interpreter can draw. The built-in prompt
// TextInput (the header query anchor) carries no server leaf state -- the browser
// owns the query text locally -- so it renders as a bare anchor node. Placement is
// tree structure + well-known node ids, so there is no region-role set.
export const WEB_UI_PROFILE = {
  widgets: new Set([WIDGET.CONTAINER, WIDGET.LABEL, WIDGET.FIELD, WIDGET.CHECKBOX, WIDGET.TEXT_INPUT, WIDGET.SPACER, WIDGET.VIEW, WIDGET.STATUS_ACTIONS]),
  surfaces: new Set([SURFACE.TABVIEW, SURFACE.FILETREE, SURFACE.GITSTATUS, SURFACE.FINDRESULTS, SURFACE.SYMBOLS, SURFACE.FOOTER_PROMPT]),
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
  const build = (node) => {
    const st = stateById.get(node.id);
    const hasLeafState = st.leaf != null && typeof st.leaf === 'object';
    const isContainer = node.container != null && typeof node.container === 'object';
    // Shape validation runs regardless of presence, so a malformed frame is caught.
    if (isContainer) {
      if (hasLeafState) { shapeOk = false; return null; }
      const children = [];
      for (const c of (node.container.children || [])) {
        const built = build(c);
        if (built) children.push(built);
      }
      if (!presentById.get(node.id)) return null;  // hidden subtree not drawn
      return { id: node.id, kind: 'container', axis: num(node.container.axis),
               gap: num(node.container.gap) || 0, size: sizeOf(node),
               inset: insetOf(node.container), children };
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
