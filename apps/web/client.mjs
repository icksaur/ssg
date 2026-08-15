// The browser client: DOM rendering and WebSocket/keyboard I/O. All pure wire,
// offset, and reconciliation logic lives in reconcile.mjs so a node test can
// exercise it without a DOM. The client renders the semantic model natively --
// document text colored by syntax scope, caret and selection from the semantic
// selection set, a tab strip -- with every color drawn from the theme's roles
// mapped to CSS custom properties, and echoes typed text locally (M3) so a
// keystroke shows before its round trip completes.

import {
  decodeValue, findSections, num, cssColor, byteToIndex, utf8Bytes,
  applyDocumentDelta, dropSettled, project, parseEnvelope, paletteReportIsFresh,
  paletteSelectedWindowRow, isPalettePromptOpen,
  interpretChrome, firstUnsupportedPrimitive, SIZE, AXIS,
} from '/reconcile.mjs';

const statusEl = document.getElementById('status');
const tabsEl = document.getElementById('tabs');
const docEl = document.getElementById('doc');
const paletteEl = document.getElementById('palette');
const chromeTopEl = document.getElementById('chrome-top');
const chromeBottomEl = document.getElementById('chrome-bottom');
const chromeErrorEl = document.getElementById('chrome-error');

const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const idKey = (v) => JSON.stringify(v, (k, x) => typeof x === 'bigint' ? x.toString() : x);

// Role ordinals the renderer maps to CSS custom properties; pinned by
// test_theme's role-ordinal contract so a reorder cannot silently mis-color.
const ROLE = { text: 0, canvas: 1, caret: 2, selection: 3, statusWarning: 13 };
const FOCUS_EDITOR = 0;   // FocusTarget::Editor ordinal.

// Persistent client model: the authoritative sections plus the still-unsettled
// local predictions. Snapshots replace `sections`; deltas mutate it in place.
const state = {
  sections: null,
  pending: [],
  nextEditId: 1,
  // The palette/finder is a client-owned derived view: the browser owns the
  // query text and selection index and the host ranks them (PaletteSearcher).
  palette: { query: '', selected: 0, requestId: 0, report: null },
};

// Is a picker (command palette or file finder) the active prompt? The wire
// field-name coupling lives in isPalettePromptOpen (reconcile.mjs).
function paletteOpen() {
  return isPalettePromptOpen(state.sections);
}

// Send the current query+selection and ask the host to rank. The monotonic
// requestId lets a stale report be dropped when responses arrive out of order.
function requestPalette() {
  state.palette.requestId += 1;
  ws.send('PICK:' + state.palette.requestId + ':' + state.palette.selected +
          ':' + state.palette.query);
}

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
  set('--ssg-status-warning', ROLE.statusWarning);
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

// The color for a SemanticRole ordinal, from the live theme's role_colors table, or
// '' to inherit. Every chrome color comes through the theme this way; the client never
// invents one.
function roleColor(ordinal, theme) {
  const rc = (theme && Array.isArray(theme.role_colors)) ? theme.role_colors : [];
  return ordinal != null && ordinal >= 0 && ordinal < rc.length ? cssColor(rc[ordinal]) : '';
}

// Build the DOM for one interpreted render node, mirroring the generic tree: a
// container becomes a flex div on its axis (a Flex node grows, a gap spaces its
// children); a leaf becomes a span. `parentAxis` is the axis the node's own Size
// measures along (a child sizes along its parent's main axis). Returns null for an
// omitted node.
function renderChromeNode(node, theme, parentAxis = AXIS.ROW) {
  if (!node) return null;
  if (node.kind === 'container') {
    const div = document.createElement('div');
    div.className = 'group';
    div.style.display = 'flex';
    div.style.flexDirection = node.axis === 1 ? 'column' : 'row';  // Axis: Row=0, Column=1
    // stretch: a child shares the parent's cross extent (the Row/Column contract),
    // rather than shrinking to its content on the cross axis.
    div.style.alignItems = 'stretch';
    div.style.boxSizing = 'border-box';  // inset stays inside the published extent
    applySize(div, node.size, parentAxis);
    applyInset(div, node.inset, node.size, parentAxis);
    if (node.gap) div.style.gap = node.gap + 'ch';
    for (const child of node.children) {
      const el = renderChromeNode(child, theme, node.axis);
      if (el) div.appendChild(el);
    }
    return div;
  }
  // leaf
  if (node.spacer) {
    const gap = document.createElement('span');
    gap.className = 'w spacer';
    gap.style.display = 'inline-block';
    if (node.width != null) {
      gap.style[parentAxis === AXIS.COLUMN ? 'height' : 'width'] = node.width + 'ch';
      gap.style.flex = '0 0 auto';
    } else applySize(gap, node.size, parentAxis);
    return gap;
  }
  const el = document.createElement('span');
  el.className = 'w' + (node.command ? ' clickable' : '');
  el.textContent = (node.checked != null ? (node.checked ? '\u2611 ' : '\u2610 ') : '') + (node.text || '');
  const color = roleColor(node.role, theme);
  if (color) el.style.color = color;
  applySize(el, node.size, parentAxis);
  if (node.command) {
    el.title = node.command;
    el.addEventListener('click', () => ws.send('CMD:' + node.command));
  }
  return el;
}

// Apply a published Size to a flex child ALONG the parent's main axis: Exact => a
// fixed extent that neither grows nor shrinks (including a literal zero extent, whose
// content is CLIPPED so it truly occupies zero), Flex => grow to fill (extent is the
// grow weight, default 1), Auto => content-sized. A Row parent measures width; a
// Column parent measures height. SIZE ordinals mirror the C++ SizeKind enum.
function applySize(el, size, parentAxis) {
  if (!size) return;
  const dim = parentAxis === AXIS.COLUMN ? 'height' : 'width';
  if (size.kind === SIZE.EXACT) {
    el.style.flex = '0 0 auto';
    el.style[dim] = (size.extent || 0) + 'ch';  // Exact(0) is a real zero extent
    el.style.overflow = 'hidden';  // content beyond the extent is clipped, not overflowed
  } else if (size.kind === SIZE.FLEX) {
    el.style.flex = (size.extent > 0 ? size.extent : 1) + ' 1 0';
  } else {
    el.style.flex = '0 0 auto';  // Auto: content extent
  }
}

// Apply a container Inset as padding (cells => ch). When the node has an Exact
// main-axis extent, the same-axis inset is clamped so their sum never exceeds the
// extent -- otherwise the CSS used border-box size floors at the padding and an
// Exact(0)+inset frame would occupy nonzero space. The cross-axis inset is not
// extent-bound. `parentAxis` names the axis the extent measures along.
function applyInset(el, inset, size, parentAxis) {
  if (!inset) return;
  let top = inset.top || 0, right = inset.right || 0;
  let bottom = inset.bottom || 0, left = inset.left || 0;
  // Clamp the same-axis inset pair so it cannot exceed an Exact extent (else the
  // used border-box size floors at the padding and the extent is violated).
  if (size && size.kind === SIZE.EXACT) {
    const extent = size.extent || 0;
    if (parentAxis === AXIS.COLUMN) {
      if (top + bottom > extent) { top = Math.min(top, extent); bottom = Math.max(0, extent - top); }
    } else {
      if (left + right > extent) { left = Math.min(left, extent); right = Math.max(0, extent - left); }
    }
  }
  el.style.padding = top + 'ch ' + right + 'ch ' + bottom + 'ch ' + left + 'ch';
}

// Interpret the published UI-VM schema + dynamic node state into the Top/Bottom
// chrome, mirroring the generic tree (structure, values, roles, triggers are the
// library's; geometry is the browser's flex layout). A schema using a primitive this
// build does not implement is a loud, visible refusal -- never a silently dropped
// element.
function renderChrome(sections) {
  const schema = sections.ui;
  const stateSection = sections.ui_state;
  chromeTopEl.textContent = '';
  chromeBottomEl.textContent = '';
  chromeErrorEl.textContent = '';
  if (!schema || !schema.root) return;

  const unsupported = firstUnsupportedPrimitive(schema);
  if (unsupported) {
    chromeErrorEl.textContent =
      'unsupported UI ' + unsupported.kind + ' ' + unsupported.ordinal +
      ' -- this client build cannot render the composed chrome';
    return;
  }
  const interpreted = interpretChrome(schema, stateSection);
  if (!interpreted || !interpreted.root) return;  // schema/state from different frames; wait

  // The root's children are the well-known areas; render each into its host by
  // its well-known node id. Placement is the tree structure + the id, not a role.
  const root = interpreted.root;
  const areas = root.kind === 'container' ? root.children : [];
  for (const area of areas) {
    const host = area.id === 'header' ? chromeTopEl
               : area.id === 'footer' ? chromeBottomEl : null;
    if (!host) continue;
    const el = renderChromeNode(area, sections.theme);
    if (el) host.appendChild(el);
  }
}

// Render the palette/finder overlay from the host's ranked report: a query line
// and the candidate rows the library ranker returned, the selected row
// highlighted. The browser never ranks -- it only shows what the host ranked.
function renderPalette() {
  const open = paletteOpen();
  paletteEl.classList.toggle('open', open);
  if (!open) { paletteEl.textContent = ''; return; }
  const report = state.palette.report;
  let html = '<div class="query">' + esc(state.palette.query || '') +
             '<span style="opacity:.4">' + esc(report ? (report.ghost || '') : '') +
             '</span></div>';
  const rows = report && Array.isArray(report.rows) ? report.rows : [];
  const selectedRow = paletteSelectedWindowRow(report);
  for (let i = 0; i < rows.length; i++) {
    const cls = 'row' + (i === selectedRow ? ' sel' : '');
    html += '<div class="' + cls + '"><span class="label">' + esc(rows[i].label || '') +
            '</span><span class="detail">' + esc(rows[i].detail || '') + '</span></div>';
  }
  paletteEl.innerHTML = html;
}

// Adopt a host palette report unless it is stale (an older requestId than the
// latest the client sent), so an out-of-order response never overwrites newer
// query/selection state.
function applyPaletteReport(report) {
  if (!paletteReportIsFresh(report, state.palette.requestId)) return;
  state.palette.report = report;
  if (report.selected != null && report.selected >= 0) {
    state.palette.selected = report.selected;
  }
  renderPalette();
}
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
  // Focus and prompt state gate local prediction and prompt rendering. A null
  // optional means unchanged, so keep the last value; the field names match the
  // wire's snake_case (the sections object is the decoded ProtocolValue tree).
  if (d.focus != null) s.focus = num(d.focus);
  if (d.prompt_status && d.prompt_status.replacement != null) s.prompt_status = d.prompt_status.replacement;
  if (d.find_replace && d.find_replace.replacement != null) s.find_replace = d.find_replace.replacement;
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
  renderChrome(s);

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
  renderPalette();
}

let wasPaletteOpen = false;

const ws = new WebSocket('ws://' + location.host + '/session');
ws.binaryType = 'arraybuffer';

ws.onopen = () => { statusEl.textContent = 'attached; awaiting snapshot'; ws.send('SSG1 ATTACH -'); };
ws.onmessage = (e) => {
  if (typeof e.data === 'string') { statusEl.textContent = e.data; return; }
  try {
    const { settledId, sections } = parseEnvelope(e.data);
    state.pending = dropSettled(state.pending, settledId);
    for (const sec of sections) {
      if (sec.tag === 0) {                 // library body: version@0, kind@1, value@2
        const kind = sec.dv.getUint8(1);
        const [payload] = decodeValue(sec.dv, 2);
        if (kind === 1) state.sections = findSections(payload);
        else if (kind === 2) applyDelta(payload);
      } else if (sec.tag === 1) {          // host palette report (JSON)
        const bytes = new Uint8Array(sec.dv.buffer, sec.dv.byteOffset, sec.length);
        applyPaletteReport(JSON.parse(new TextDecoder().decode(bytes)));
      }
    }
    if (!state.sections) { statusEl.textContent = 'no sections yet'; return; }

    // On the transition into an open picker, reset the browser-owned query and
    // ask the host for the first ranking; on close, clear it.
    const nowOpen = paletteOpen();
    if (nowOpen && !wasPaletteOpen) {
      state.palette.query = '';
      state.palette.selected = 0;
      state.palette.report = null;
      requestPalette();
    } else if (!nowOpen && wasPaletteOpen) {
      state.palette.report = null;
    }
    wasPaletteOpen = nowOpen;

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

  // When a picker is open, the browser owns its query and selection (a
  // client-owned derived view). Query edits and selection moves re-request a
  // host ranking; Enter submits the selected candidate; Escape closes via the
  // library keymap (prompt.cancel). Nothing here touches the document.
  if (paletteOpen()) {
    const p = state.palette;
    if (ev.key === 'Enter') {
      ev.preventDefault();
      ws.send('PSUB:' + p.selected + ':' + p.query);
      return;
    }
    if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
      ev.preventDefault();
      // selected is an absolute ranked index; move it optimistically and let the
      // host clamp to the candidate count and window it in the returned report.
      p.selected = ev.key === 'ArrowDown'
        ? p.selected + 1
        : Math.max(p.selected - 1, 0);
      requestPalette();
      return;
    }
    if (ev.key === 'Backspace') {
      ev.preventDefault();
      p.query = Array.from(p.query).slice(0, -1).join('');
      p.selected = 0;
      requestPalette();
      renderPalette();
      return;
    }
    if (Array.from(ev.key).length === 1 && !ev.altKey) {
      ev.preventDefault();
      p.query += ev.key;
      p.selected = 0;
      requestPalette();
      renderPalette();
      return;
    }
    // Escape and other keys fall through to the keymap (Escape -> prompt.cancel).
  }

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
