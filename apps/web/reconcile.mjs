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
// Wire ordinals, pinned by the C++ enums (WidgetKind, RegionRole). The schema's
// leaves carry `kind`; regions carry `role`.
export const WIDGET = { CONTAINER: 0, LABEL: 1, FIELD: 2, CHECKBOX: 3, TEXT_INPUT: 4, SPACER: 5 };
export const REGION = { TOP: 0, BOTTOM: 1, LEADING: 2, TRAILING: 3, OVERLAY: 4 };

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

// Interpret the schema (structure) + dynamic state (resolved values/presence) into
// an ordered, per-region list of draw instructions the DOM builder applies, keyed by
// node id. Presence is necessary but not sufficient: a Label/Field draws only when
// present AND it has resolved leaf state (a present-but-stateless leaf is the
// resolved drop); a Checkbox draws whenever present (its state always exists); a
// Spacer draws a structural gap whenever present; a Container lays out its present
// children. Returns null (do not interpret; wait for a consistent frame) when the
// schema and state are from different frames -- different generation, or a node-id
// set that does not correspond -- or when a primitive is unsupported.
export function interpretChrome(schema, state, profile = WEB_UI_PROFILE) {
  if (!schema || !Array.isArray(schema.regions) || !state) return null;
  if (firstUnsupportedPrimitive(schema, profile)) return null;
  if (num(schema.generation) !== num(state.generation)) return null;

  const stateById = new Map();
  for (const n of (state.nodes || [])) stateById.set(n.id, num(n.present) ? n : { ...n, present: false });

  // Collect the schema's node ids to require one-to-one correspondence.
  const schemaIds = new Set();
  const collect = (node) => {
    if (!node) return;
    schemaIds.add(node.id);
    if (node.container && Array.isArray(node.container.children))
      for (const c of node.container.children) collect(c);
  };
  for (const region of schema.regions) collect(region.root);
  if (schemaIds.size !== (state.nodes || []).length) return null;
  for (const id of schemaIds) if (!stateById.has(id)) return null;

  const regions = [];
  for (const region of schema.regions) {
    const items = [];
    const walk = (node) => {
      const st = stateById.get(node.id);
      if (!st || !st.present) return;  // hidden subtree is not drawn
      if (node.container && typeof node.container === 'object') {
        for (const child of (node.container.children || [])) walk(child);
        return;
      }
      if (!node.leaf) return;
      const kind = num(node.leaf.kind);
      const leaf = st.leaf && typeof st.leaf === 'object' ? st.leaf : null;
      if (kind === WIDGET.SPACER) { items.push({ id: node.id, kind, spacer: true }); return; }
      if (kind === WIDGET.CHECKBOX) {
        if (!leaf) return;  // a checkbox always resolves; defensively skip if absent
        items.push({ id: node.id, kind, text: leaf.value || '', checked: !!num(leaf.checked),
                     command: leaf.command != null ? leaf.command : null, role: node.leaf.role || null });
        return;
      }
      // Label/Field: draw only when a leaf state exists (the resolved drop is nullopt).
      if (!leaf) return;
      items.push({ id: node.id, kind, text: leaf.value || '',
                   command: leaf.command != null ? leaf.command : null, role: node.leaf.role || null });
    };
    walk(region.root);
    regions.push({ role: num(region.role), items });
  }
  return { regions };
}

