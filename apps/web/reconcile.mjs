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

// A palette report is applied only when it is not stale -- its requestId is at
// least the latest request the client has sent -- so an out-of-order or slow
// response can never overwrite newer query/selection state.
export const paletteReportIsFresh = (report, latestRequestId) =>
  report.requestId >= latestRequestId;

// Which window row (index into report.rows) is the selected one, or -1 when the
// selection is outside the window. report.selected is an ABSOLUTE index into the
// full ranked order; report.rows is the visible slice starting at firstVisible.
export function paletteSelectedWindowRow(report) {
  if (!report || report.selected == null || report.selected < 0) return -1;
  const row = report.selected - (report.firstVisible || 0);
  const rows = report.rows ? report.rows.length : 0;
  return row >= 0 && row < rows ? row : -1;
}

// FocusTarget::Prompt and PromptKind::Palette ordinals, and the wire field names
// the decoded snapshot uses. The sections object is the decoded ProtocolValue
// tree, so its keys are the encoder's snake_case names (prompt_status,
// active_kind) -- NOT camelCase. This one function owns that coupling so a
// mis-spelling cannot silently hide the palette overlay again.
export const FOCUS_PROMPT = 2;
export const PROMPT_PALETTE = 5;
export function isPalettePromptOpen(sections) {
  if (!sections) return false;
  if (num(sections.focus) !== FOCUS_PROMPT) return false;
  const ps = sections.prompt_status;
  return !!ps && ps.active_kind != null && num(ps.active_kind) === PROMPT_PALETTE;
}

// --- UI-VM: the web interpreter over the published schema + dynamic node state ---
//
// Wire ordinals, pinned by the C++ enums (WidgetKind, RegionRole, Axis, SizeKind,
// SemanticRole). The schema's leaves carry `kind`; regions carry `role`; nodes carry
// `size`; containers carry `axis`.
export const WIDGET = { CONTAINER: 0, LABEL: 1, FIELD: 2, CHECKBOX: 3, TEXT_INPUT: 4, SPACER: 5 };
export const REGION = { TOP: 0, BOTTOM: 1, LEADING: 2, TRAILING: 3, OVERLAY: 4 };
export const AXIS = { ROW: 0, COLUMN: 1 };
export const SIZE = { EXACT: 0, FLEX: 1, AUTO: 2 };
// SemanticRole name -> ordinal for the roles chrome widgets use, matched to the C++
// SemanticRole enum. A widget's own role overrides the region default (Header for a
// Top region, Footer for a Bottom region); the color is looked up in theme.role_colors
// by ordinal, so a client never invents a color.
export const ROLE_ORDINAL = {
  text: 0, header: 10, footer: 11, status_info: 12, status_warning: 13,
};
const REGION_DEFAULT_ROLE = { [REGION.TOP]: ROLE_ORDINAL.header, [REGION.BOTTOM]: ROLE_ORDINAL.footer };

// The primitives THIS web build's interpreter can draw: header/footer chrome, so
// Container/Label/Field/Checkbox/Spacer leaves in the Top/Bottom regions. TextInput
// and the side/overlay regions are not implemented, so a schema using one is a loud,
// tested rejection -- never a silently dropped element.
export const WEB_UI_PROFILE = {
  widgets: new Set([WIDGET.CONTAINER, WIDGET.LABEL, WIDGET.FIELD, WIDGET.CHECKBOX, WIDGET.SPACER]),
  regions: new Set([REGION.TOP, REGION.BOTTOM]),
};

// The first schema primitive `profile` does not support, as
// { kind: 'widget'|'region', ordinal }, or null when every region role and leaf
// widget kind is supported. The interpreter runs only when this returns null.
export function firstUnsupportedPrimitive(schema, profile = WEB_UI_PROFILE) {
  if (!schema || !Array.isArray(schema.regions)) return null;
  const walk = (node) => {
    if (!node) return null;
    if (node.leaf && typeof node.leaf === 'object') {
      const kind = num(node.leaf.kind);
      if (!profile.widgets.has(kind)) return { kind: 'widget', ordinal: kind };
    }
    if (node.container && Array.isArray(node.container.children)) {
      for (const child of node.container.children) {
        const bad = walk(child);
        if (bad) return bad;
      }
    }
    return null;
  };
  for (const region of schema.regions) {
    const role = num(region.role);
    if (!profile.regions.has(role)) return { kind: 'region', ordinal: role };
    const bad = walk(region.root);
    if (bad) return bad;
  }
  return null;
}

// Interpret the schema (structure) + dynamic state (resolved values/presence) into a
// per-region RENDER TREE the DOM builder mirrors 1:1 -- the generic container tree is
// preserved (axis, flex sizing, gap, and the left/middle/right grouping), so packing
// follows the published tree rather than a flattened item list. Each node:
//   container: { id, kind:'container', axis, gap, flex, children:[...] }
//   leaf:      { id, kind:'leaf', widget, flex, text, checked?, command?, role, spacer? }
// A non-present node (and its subtree) is omitted; a Label/Field with no resolved leaf
// state is the resolved drop and is omitted; a Checkbox always renders; a Spacer renders
// a gap. `role` is the effective SemanticRole ordinal (the widget's own role or the
// region default) the DOM builder colors from the theme.
//
// Returns null (do not interpret; wait for a consistent frame) when the schema and
// state are from different frames (different generation, or a node-id set that does not
// correspond one-to-one), when a primitive is unsupported, or when a state record's
// SHAPE disagrees with its schema node (a container or spacer carrying leaf state, or a
// checkbox missing it) -- a malformed frame is never partially drawn.
export function interpretChrome(schema, state, profile = WEB_UI_PROFILE) {
  if (!schema || !Array.isArray(schema.regions) || !state) return null;
  if (firstUnsupportedPrimitive(schema, profile)) return null;
  if (num(schema.generation) !== num(state.generation)) return null;

  const stateById = new Map();
  for (const n of (state.nodes || [])) stateById.set(n.id, n);

  // Node-id correspondence: exactly the schema's ids, one-to-one with the state.
  const schemaIds = [];
  const collect = (node) => {
    if (!node) return;
    schemaIds.push(node.id);
    if (node.container && Array.isArray(node.container.children))
      for (const c of node.container.children) collect(c);
  };
  for (const region of schema.regions) collect(region.root);
  if ((state.nodes || []).length !== schemaIds.length) return null;
  for (const id of schemaIds) if (!stateById.has(id)) return null;

  let shapeOk = true;
  const isFlex = (node) => !!(node.size && num(node.size.kind) === SIZE.FLEX);
  const build = (node, defaultRole) => {
    const st = stateById.get(node.id);
    const hasLeafState = st.leaf != null && typeof st.leaf === 'object';
    const isContainer = node.container != null && typeof node.container === 'object';
    // Shape validation runs regardless of presence, so a malformed frame is caught.
    if (isContainer) {
      if (hasLeafState) { shapeOk = false; return null; }
      const children = [];
      for (const c of (node.container.children || [])) {
        const built = build(c, defaultRole);
        if (built) children.push(built);
      }
      if (!num(st.present)) return null;  // hidden subtree not drawn
      return { id: node.id, kind: 'container', axis: num(node.container.axis),
               gap: num(node.container.gap) || 0, flex: isFlex(node), children };
    }
    if (!node.leaf || typeof node.leaf !== 'object') { shapeOk = false; return null; }
    const wk = num(node.leaf.kind);
    if (wk === WIDGET.SPACER && hasLeafState) { shapeOk = false; return null; }
    if (wk === WIDGET.CHECKBOX && !hasLeafState) { shapeOk = false; return null; }
    if (!num(st.present)) return null;  // hidden leaf not drawn
    const role = node.leaf.role && ROLE_ORDINAL[node.leaf.role] != null
      ? ROLE_ORDINAL[node.leaf.role] : defaultRole;
    if (wk === WIDGET.SPACER) {
      const w = node.leaf.width != null ? num(node.leaf.width) : null;
      return { id: node.id, kind: 'leaf', widget: wk, spacer: true, width: w, flex: isFlex(node) };
    }
    if (wk === WIDGET.CHECKBOX) {
      return { id: node.id, kind: 'leaf', widget: wk, flex: isFlex(node), role,
               text: st.leaf.value || '', checked: !!num(st.leaf.checked),
               command: st.leaf.command != null ? st.leaf.command : null };
    }
    // Label/Field: no leaf state is the resolved drop (not drawn, not an error).
    if (!hasLeafState) return null;
    return { id: node.id, kind: 'leaf', widget: wk, flex: isFlex(node), role,
             text: st.leaf.value || '',
             command: st.leaf.command != null ? st.leaf.command : null };
  };

  const regions = [];
  for (const region of schema.regions) {
    const role = num(region.role);
    const node = build(region.root, REGION_DEFAULT_ROLE[role] != null ? REGION_DEFAULT_ROLE[role] : ROLE_ORDINAL.text);
    regions.push({ role, node });
  }
  if (!shapeOk) return null;
  return { regions };
}

