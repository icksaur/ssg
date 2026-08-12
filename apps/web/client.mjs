// The browser client: DOM rendering and WebSocket/keyboard I/O. All pure wire,
// offset, and reconciliation logic lives in reconcile.mjs so a node test can
// exercise it without a DOM. The client renders the semantic model natively --
// document text colored by syntax scope, caret and selection from the semantic
// selection set, a tab strip -- with every color drawn from the theme's roles
// mapped to CSS custom properties, and echoes typed text locally (M3) so a
// keystroke shows before its round trip completes.

import {
  decodeValue, findSections, num, cssColor, byteToIndex, utf8Bytes,
  applyDocumentDelta, dropSettled, project,
} from '/reconcile.mjs';

const statusEl = document.getElementById('status');
const tabsEl = document.getElementById('tabs');
const docEl = document.getElementById('doc');

const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const idKey = (v) => JSON.stringify(v, (k, x) => typeof x === 'bigint' ? x.toString() : x);

// Role ordinals the renderer maps to CSS custom properties; pinned by
// test_theme's role-ordinal contract so a reorder cannot silently mis-color.
const ROLE = { text: 0, canvas: 1, caret: 2, selection: 3 };
const FOCUS_EDITOR = 0;  // FocusTarget::Editor ordinal.

// Persistent client model: the authoritative sections plus the still-unsettled
// local predictions. Snapshots replace `sections`; deltas mutate it in place.
const state = { sections: null, pending: [], nextEditId: 1 };

function applyTheme(theme) {
  const root = document.documentElement.style;
  const rc = (theme && Array.isArray(theme.role_colors)) ? theme.role_colors : [];
  // Re-derive every property each frame, clearing any set by an earlier theme,
  // so a short or absent role table never leaves a stale color on screen.
  const set = (name, i) => { const c = cssColor(rc[i]); if (c) root.setProperty(name, c); else root.removeProperty(name); };
  set('--ssg-text', ROLE.text);
  set('--ssg-canvas', ROLE.canvas);
  set('--ssg-caret', ROLE.caret);
  set('--ssg-selection', ROLE.selection);
  return (theme && Array.isArray(theme.syntax_colors)) ? theme.syntax_colors : [];
}

function renderTabs(tabs) {
  tabsEl.textContent = '';
  if (!tabs || !Array.isArray(tabs.tabs)) return;
  const activeId = idKey(tabs.active);
  for (const t of tabs.tabs) {
    const el = document.createElement('span');
    el.className = 'tab' + (idKey(t.id) === activeId ? ' active' : '');
    el.textContent = (t.dirty ? '\u25CF ' : '') + (t.label || '');
    tabsEl.appendChild(el);
  }
}

// Apply one authoritative delta to the persistent model, mirroring
// SessionSnapshotCodec::replay for the sections the client renders. A null
// optional field means "unchanged", exactly as the wire encodes it.
function applyDelta(d) {
  const s = state.sections;
  if (!s) return;
  if (d.document) {
    s.document.text = applyDocumentDelta(s.document.text, d.document);
    if (d.document_caret != null) s.document.caret = num(d.document_caret);
  } else if (d.document_caret != null) {
    s.document.caret = num(d.document_caret);
  }
  if (d.selection && d.selection.replacement != null) s.selection = d.selection.replacement;
  if (d.tabs && d.tabs.state != null) s.tabs = d.tabs.state;
  if (d.syntax && d.syntax.spans != null) {
    if (!s.syntax) s.syntax = {};
    s.syntax.spans = d.syntax.spans;
  }
  if (d.theme && d.theme.replacement != null) s.theme = d.theme.replacement;
  // Focus and prompt state gate local prediction and (later) prompt rendering.
  // A null optional means unchanged, so keep the last value; without this the
  // client would keep predicting document inserts after focus moved to a prompt.
  if (d.focus != null) s.focus = num(d.focus);
  if (d.prompt_status && d.prompt_status.replacement != null) s.promptStatus = d.prompt_status.replacement;
  if (d.find_replace && d.find_replace.replacement != null) s.findReplace = d.find_replace.replacement;
}

// Segment the projected text at every syntax-span edge, selection edge, and the
// caret; color each segment by its syntax scope through the theme and mark
// selected segments and the caret. Authoritative offsets are shifted past any
// predicted text; geometry is the browser's, only offsets are semantic.
function render() {
  const s = state.sections;
  if (!s) return;
  const syntaxColors = applyTheme(s.theme);
  renderTabs(s.tabs);

  const authText = s.document.text;
  const authCaret = num(s.document.caret);
  const proj = project(authText, authCaret, state.pending);
  const text = proj.text;
  const predBytes = proj.predEnd - proj.predStart;
  // A span begins after the predicted text (>=), but a span ending at the caret
  // stops before it (>), so predicted text never inherits the preceding scope.
  const shiftBegin = (o) => o >= proj.predStart ? o + predBytes : o;
  const shiftEnd = (o) => o > proj.predStart ? o + predBytes : o;

  const rawSpans = (s.syntax && Array.isArray(s.syntax.spans)) ? s.syntax.spans : [];
  const spans = rawSpans.map((sp) => ({ begin: shiftBegin(num(sp.begin)), end: shiftEnd(num(sp.end)), scope: num(sp.scope) }));

  const sels = (s.selection && Array.isArray(s.selection.selections)) ? s.selection.selections : [];
  const ranges = [];
  for (const sel of sels) {
    const a = shiftEnd(num(sel.anchor.byte_offset)), b = shiftEnd(num(sel.active.byte_offset));
    if (a !== b) ranges.push([Math.min(a, b), Math.max(a, b)]);
  }

  const caret = proj.caret;
  const total = utf8Bytes(text);
  const bounds = new Set([0, total, caret]);
  for (const sp of spans) { bounds.add(sp.begin); bounds.add(sp.end); }
  for (const r of ranges) { bounds.add(r[0]); bounds.add(r[1]); }
  const cuts = [...bounds].filter((b) => b >= 0 && b <= total).sort((a, b) => a - b);
  const map = byteToIndex(text, cuts);

  const scopeAt = (byte) => { for (const sp of spans) { if (byte >= sp.begin && byte < sp.end) return sp.scope; } return -1; };
  const selectedAt = (byte) => ranges.some((r) => byte >= r[0] && byte < r[1]);

  let html = '';
  for (let i = 0; i + 1 < cuts.length; i++) {
    const a = cuts[i], b = cuts[i + 1];
    if (a === caret) html += '<span class="caret"></span>';
    const ia = map.get(a), ib = map.get(b);
    if (ia === undefined || ib === undefined || ib <= ia) continue;
    const scope = scopeAt(a);
    // Span scope and syntax_colors are produced together by one snapshot, so the
    // index is self-consistent; the bounds check only guards a truncated palette.
    const color = (scope >= 0 && scope < syntaxColors.length) ? cssColor(syntaxColors[scope]) : '';
    const cls = selectedAt(a) ? ' class="sel"' : '';
    const style = color ? ' style="color:' + color + '"' : '';
    html += '<span' + cls + style + '>' + esc(text.substring(ia, ib)) + '</span>';
  }
  if (caret >= total) html += '<span class="caret"></span>';
  docEl.innerHTML = html;
}

const ws = new WebSocket('ws://' + location.host + '/session');
ws.binaryType = 'arraybuffer';

ws.onopen = () => { statusEl.textContent = 'attached; awaiting snapshot'; ws.send('SSG1 ATTACH -'); };
ws.onmessage = (e) => {
  if (typeof e.data === 'string') { statusEl.textContent = e.data; return; }
  try {
    const dv = new DataView(e.data);
    // Envelope: 8-byte little-endian settledClientEditId, then an optional
    // library body (version + kind + value). Drop settled predictions first.
    const settledId = dv.getBigUint64(0, true);
    state.pending = dropSettled(state.pending, settledId);
    if (e.data.byteLength > 8) {
      const kind = dv.getUint8(9);      // version at 8, kind at 9, value at 10.
      const [payload] = decodeValue(dv, 10);
      if (kind === 1) {                 // SessionSnapshot
        state.sections = findSections(payload);
      } else if (kind === 2) {          // SessionDelta
        applyDelta(payload);
      }
    }
    if (!state.sections) { statusEl.textContent = 'no sections yet'; return; }
    render();
    statusEl.textContent = 'live (' + e.data.byteLength + ' bytes)';
    docEl.focus();
  } catch (err) { statusEl.textContent = 'render error: ' + err.message; }
};
ws.onclose = () => { statusEl.textContent += ' [closed]'; };
ws.onerror = () => { statusEl.textContent = 'ws error'; };

docEl.addEventListener('keydown', (ev) => {
  // Ctrl/Meta chords belong to the browser: ssg's keymap uses Alt as its chord
  // modifier, so the web client never claims a Ctrl/Meta combo. Letting them
  // through keeps native zoom, copy/paste, and find working -- the browser is a
  // first-class client that may add its own affordances. (The one library action
  // reachable only via Ctrl+Shift+Home/End, select-to-document-extreme, has no
  // Alt twin and is thus unreachable on web until the keymap grows one.)
  if (ev.ctrlKey || ev.metaKey) return;

  const mods = (ev.altKey ? 'a' : '') + (ev.shiftKey ? 's' : '');
  // Array.from counts Unicode scalars, so a supplementary-plane character (two
  // UTF-16 code units in ev.key) still registers as one printable scalar and is
  // sent, consistent with the UTF-8 offset contract.
  const printable = Array.from(ev.key).length === 1;
  const text = printable ? ev.key : '';
  ev.preventDefault();

  // Predict a caret-anchored insertion locally only when the editor has focus
  // and no chord modifier is held (typing into a prompt, or an Alt chord, is not
  // a document insert); the char shows this frame and the host's settlement
  // re-bases it. Everything else round-trips without echo.
  let editId = '';
  const focus = state.sections ? num(state.sections.focus) : -1;
  if (printable && !ev.altKey && focus === FOCUS_EDITOR) {
    const id = state.nextEditId++;
    state.pending.push({ id, text });
    editId = String(id);
    render();
  }
  ws.send('KEY:' + ev.code + ':' + mods + ':' + editId + ':' + text);
});
