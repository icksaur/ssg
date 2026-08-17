// The browser client: DOM rendering and WebSocket/keyboard I/O. All pure wire,
// offset, and reconciliation logic lives in reconcile.mjs so a node test can
// exercise it without a DOM. The client renders the semantic model natively --
// document text colored by syntax scope, caret and selection from the semantic
// selection set, a tab strip -- with every color drawn from the theme's roles
// mapped to CSS custom properties, and echoes typed text locally (M3) so a
// keystroke shows before its round trip completes.

import {
  decodeValue, findSections, num, cssColor, byteToIndex, utf8Bytes,
  dropSettled, project, parseEnvelope,
  isPalettePromptOpen, matcherBoundsFromPalette, clampPaletteSelection,
  encodePaletteSubmit, applySessionDeltaSections, applyTreeDelta,
  interpretChrome, firstUnsupportedPrimitive, SIZE, AXIS, WIDGET, SURFACE,
  encodeStatusActionInvocation, shouldResetLocalQuery, pickerEpochFromPalette,
} from '/reconcile.mjs';
import { fuzzyRank } from '/fuzzy.mjs';

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
const ROLE = { text: 0, canvas: 1, caret: 2, selection: 3, statusWarning: 13,
               diffAdded: 19, diffRemoved: 20, diffModified: 21 };
const FOCUS_EDITOR = 0;   // FocusTarget::Editor ordinal.

// GitTreeStatus ordinals (Added, Modified, Deleted, Renamed, Untracked) surfaced
// as a native web affordance: a short status letter and the matching Diff* theme
// role color. Only existing theme roles are used -- the client invents no color --
// and this is presentation the library carries as git_status, not new product
// state. Untracked reuses the "added" role by the usual convention (new content).
const GIT_STATUS = [
  { letter: 'A', role: ROLE.diffAdded },
  { letter: 'M', role: ROLE.diffModified },
  { letter: 'D', role: ROLE.diffRemoved },
  { letter: 'R', role: ROLE.diffModified },
  { letter: 'U', role: ROLE.diffAdded },
];

// Persistent client model: the authoritative sections plus the still-unsettled
// local predictions. Snapshots replace `sections`; deltas mutate it in place.
const state = {
  sections: null,
  pending: [],
  nextEditId: 1,
  // The palette/finder is a client-owned derived view: the browser owns the
  // query text and selection index, and ranks the published candidate universe locally.
  palette: { query: '', selected: 0 },
};

// Is a picker (command palette or file finder) the active prompt? The wire
// field-name coupling lives in isPalettePromptOpen (reconcile.mjs).
function paletteOpen() {
  return isPalettePromptOpen(state.sections);
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

function appendTabs(parent, tabs) {
  const host = document.createElement('div');
  host.className = 'tabs';
  if (tabs && Array.isArray(tabs.tabs)) {
    const activeId = idKey(tabs.active);
    for (const t of tabs.tabs) {
      const el = document.createElement('span');
      el.className = 'tab' + (idKey(t.id) === activeId ? ' active' : '');
      el.textContent = (t.dirty ? '\u25CF ' : '') + (t.label || '');
      host.appendChild(el);
    }
  }
  parent.appendChild(host);
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
function renderChromeNode(node, theme, parentAxis = AXIS.ROW, topLevel = false) {
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
    // A top-level well-known area is a native region: the browser owns its outer
    // geometry. The grid's Exact cell heights (header/footer) are a grid contract,
    // not a web one, so its own published Size is NOT imported as a CSS extent --
    // applying it would clip the bar (an Exact cell height mis-axised to width:1ch
    // once collapsed the whole header to "~"). The region fills its host width and
    // sizes to content; the inner tree's sizes below stay authoritative.
    if (topLevel) {
      div.style.width = '100%';
    } else {
      applySize(div, node.size, parentAxis);
      applyInset(div, node.inset, node.size, parentAxis);
    }
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
  if (node.widget === WIDGET.VIEW) {
    const el = renderSurfaceNode(node);
    applySize(el, node.size, parentAxis);
    return el;
  }
  if (node.widget === WIDGET.STATUS_ACTIONS) {
    const el = renderStatusActionsNode(node);
    applySize(el, node.size, parentAxis);
    return el;
  }
  if (node.widget === WIDGET.TEXT_INPUT) {
    // The header prompt anchor: the render node carries no server text, so the
    // browser-owned local query fills it here (no per-keystroke wire delta).
    const el = document.createElement('span');
    el.className = 'w input-line';
    const sigil = document.createElement('span');
    sigil.textContent = node.sigil || '';
    el.appendChild(sigil);
    el.appendChild(document.createTextNode(state.palette.query || ''));
    const caret = document.createElement('span');
    caret.className = 'caret';
    el.appendChild(caret);
    const color = roleColor(node.role, theme);
    if (color) el.style.color = color;
    applySize(el, node.size, parentAxis);
    return el;
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

function renderDocumentInto(host) {
    const s = state.sections;
    const syntaxColors = applyTheme(s.theme);
    const authText = s.document.text;
    const authCaret = num(s.document.caret);
    const proj = project(authText, authCaret, state.pending);
    const text = proj.text;
    const predBytes = proj.predEnd - proj.predStart;
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
      const color = (scope >= 0 && scope < syntaxColors.length) ? cssColor(syntaxColors[scope]) : '';
      const cls = selectedAt(a) ? ' class="sel"' : '';
      const style = color ? ' style="color:' + color + '"' : '';
      html += '<span' + cls + style + '>' + esc(text.substring(ia, ib)) + '</span>';
    }
    if (caret >= total) html += '<span class="caret"></span>';
    host.innerHTML = html;
}

function renderSurfaceNode(node) {
    const el = document.createElement('div');
    el.className = 'surface surface-' + node.surface;
    const s = state.sections || {};
    if (node.surface === SURFACE.TABVIEW) {
      appendTabs(el, s.tabs);
      const pre = document.createElement('pre');
      pre.className = 'doc-surface';
      pre.tabIndex = 0;
      renderDocumentInto(pre);
      el.appendChild(pre);
    } else if (node.surface === SURFACE.FILETREE || node.surface === SURFACE.GITSTATUS || node.surface === SURFACE.SYMBOLS) {
      renderTreeSurface(el, node.surface, s.tree);
    } else if (node.surface === SURFACE.FINDRESULTS) {
      renderFindResultsSurface(el, s.palette);
    }
    return el;
}

const TREE_KIND = { FILESYSTEM: 0, GIT: 1, SYMBOLS: 2 };
function renderTreeSurface(parent, surface, tree) {
    const want = surface === SURFACE.FILETREE ? TREE_KIND.FILESYSTEM
               : surface === SURFACE.GITSTATUS ? TREE_KIND.GIT : TREE_KIND.SYMBOLS;
    const providers = tree && Array.isArray(tree.providers) ? tree.providers : [];
    const provider = providers.find((p) => num(p.kind) === want);
    const selected = provider ? idKey(provider.selected) : '';
    for (const row of (provider && Array.isArray(provider.nodes) ? provider.nodes : [])) {
      const n = row.node || {};
      const div = document.createElement('div');
      div.className = 'tree-row clickable' + (idKey(n.id) === selected ? ' sel' : '');
      div.style.paddingLeft = (num(row.depth) || 0) * 2 + 'ch';
      // The twisty marks an expandable node's state; a leaf keeps the same column
      // blank so labels align. A closed directory shows the collapsed glyph.
      const twisty = document.createElement('span');
      twisty.className = 'twisty';
      twisty.textContent = n.expandable ? (row.expanded ? '\u25be ' : '\u25b8 ') : '  ';
      div.appendChild(twisty);
      // A git entry carries a status: show its short letter and color the row with
      // the matching Diff* theme role (a native affordance over existing roles).
      const git = (surface === SURFACE.GITSTATUS && n.git_status != null)
        ? GIT_STATUS[num(n.git_status)] : null;
      if (git) {
        const marker = document.createElement('span');
        marker.className = 'git-status';
        marker.textContent = git.letter + ' ';
        div.appendChild(marker);
        const color = roleColor(git.role, state.sections && state.sections.theme);
        if (color) div.style.color = color;
      }
      div.appendChild(document.createTextNode((n.icon ? n.icon + ' ' : '') + (n.label || '')));
      // A click selects then activates the node -- opening a file or toggling a
      // directory -- the same library commands a TUI pointer press dispatches.
      if (typeof n.id === 'string') {
        div.addEventListener('click', () => ws.send('TSEL:' + n.id));
      }
      parent.appendChild(div);
    }
}

function locallyRankedPaletteRows(palette) {
    const candidates = palette && Array.isArray(palette.candidates) ? palette.candidates : [];
    const { params, maxMagnitude, maxCandidateBytes } = matcherBoundsFromPalette(palette);
    const order = fuzzyRank(candidates, state.palette.query || '', params, maxMagnitude, maxCandidateBytes);
    return order.map((i) => candidates[i]);
}

function renderFindResultsSurface(parent, palette) {
    let rows = [];
    try {
      rows = locallyRankedPaletteRows(palette);
    } catch (err) {
      console.error('palette matcher wire error:', err);
      const notice = document.createElement('div');
      notice.className = 'row';
      notice.textContent = 'palette matcher parameters are invalid';
      parent.appendChild(notice);
      return;
    }
    state.palette.selected = clampPaletteSelection(state.palette.selected, rows.length);
    for (let i = 0; i < rows.length; i++) {
      const div = document.createElement('div');
      div.className = 'row' + (i === state.palette.selected ? ' sel' : '');
      div.innerHTML = '<span class="label">' + esc(rows[i].label || '') +
        '</span><span class="detail">' + esc(rows[i].detail || '') + '</span>';
      parent.appendChild(div);
    }
}

function renderStatusActionsNode(node) {
    const el = document.createElement('span');
    el.className = 'status-actions';
    const status = state.sections && state.sections.prompt_status && state.sections.prompt_status.status;
    const items = status && Array.isArray(status.items) ? status.items : [];
    const item = items[status ? (num(status.selected) || 0) : 0];
    for (const action of (item && Array.isArray(item.actions) ? item.actions : [])) {
      const button = document.createElement('button');
      button.textContent = action.accessible_label || action.accessibleLabel || action.id || '';
      const bg = roleColor(ROLE.canvas, state.sections && state.sections.theme);
      const fg = roleColor(ROLE.text, state.sections && state.sections.theme);
      if (bg) button.style.backgroundColor = bg;
      if (fg) button.style.color = fg;
      button.style.borderColor = fg || 'currentColor';
      button.addEventListener('click', () => ws.send(encodeStatusActionInvocation({
        statusId: item.id, actionId: action.id, generation: item.generation,
      })));
      el.appendChild(button);
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
  const presenceSection = sections.ui_presence;
  chromeTopEl.textContent = '';
  chromeBottomEl.textContent = '';
  chromeErrorEl.textContent = '';
  if (!schema || !schema.root) return false;

  const unsupported = firstUnsupportedPrimitive(schema);
  if (unsupported) {
    chromeErrorEl.textContent =
      'unsupported UI ' + unsupported.kind + ' ' + unsupported.ordinal +
      ' -- this client build cannot render the composed chrome';
    return false;
  }
  const interpreted = interpretChrome(schema, stateSection, presenceSection);
  if (!interpreted || !interpreted.root) return false;  // schema/state from different frames; wait

  // The root's children are the well-known areas; render each into its host by
  // its well-known node id. Placement is the tree structure + the id, not a role.
  const root = interpreted.root;
  const areas = root.kind === 'container' ? root.children : [];
  let renderedBody = false;
  for (const area of areas) {
    const host = area.id === 'header' ? chromeTopEl
               : area.id === 'footer' ? chromeBottomEl
               : area.id === 'body' ? docEl : null;
    if (!host) continue;
    if (area.id === 'body') {
      host.textContent = '';
      renderedBody = true;
      tabsEl.textContent = '';
    }
    const el = renderChromeNode(area, sections.theme, AXIS.COLUMN, true);
    if (el) {
      // The body is the flexible region; header/footer size to their content.
      if (area.id === 'body') el.style.flex = '1 1 auto';
      host.appendChild(el);
    }
  }
  return renderedBody;
}

// The legacy overlay host is kept only as a closed shell; the active picker is the
// retained FindResults surface inside the interpreted whole-screen tree.
function renderPalette() {
  const open = paletteOpen();
  paletteEl.classList.toggle('open', false);
  if (!open) paletteEl.textContent = '';
}

function refreshFinder() {
  render();
}

function applyDelta(d) {
  applySessionDeltaSections(state.sections, d);
  // The tree is retained and spliced in place; only a genuinely inexpressible
  // tree transition (snapshot_required, a missed base revision, or a malformed
  // splice) falls back to a full snapshot, so ordinary expand/open/select no
  // longer churns the panel through a resync.
  if (d.tree && !applyTreeDelta(state.sections.tree, d.tree)) {
    statusEl.textContent = 'tree update requires full snapshot; resyncing';
    if (ws.readyState === WebSocket.OPEN) ws.send('SNAP');
    return false;
  }
  return true;
}

// Segment the projected text at every syntax-span edge, selection edge, and the
// caret; color each segment by its syntax scope through the theme and mark
// selected segments and the caret. Authoritative offsets are shifted past any
// predicted text; geometry is the browser's, only offsets are semantic.
function render() {
  const s = state.sections;
  if (!s) return;
  applyTheme(s.theme);
  const bodyRendered = renderChrome(s);
  if (!bodyRendered) {
    renderTabs(s.tabs);
    renderDocumentInto(docEl);
  }
  renderPalette();
}

let wasPaletteOpen = false;
let lastPickerEpoch = 0n;

const ws = new WebSocket('ws://' + location.host + '/session');
ws.binaryType = 'arraybuffer';

ws.onopen = () => { statusEl.textContent = 'attached; awaiting snapshot'; ws.send('SSG1 ATTACH -'); };
ws.onmessage = (e) => {
  if (typeof e.data === 'string') { statusEl.textContent = e.data; return; }
  try {
    const { settledId, sections } = parseEnvelope(e.data);
    let resyncRequested = false;
    let snapshotApplied = false;
    for (const sec of sections) {
      if (sec.tag === 0) {                 // library body: version@0, kind@1, value@2
        const kind = sec.dv.getUint8(1);
        const [payload] = decodeValue(sec.dv, 2);
        if (kind === 1) { state.sections = findSections(payload); snapshotApplied = true; }
        else if (kind === 2) resyncRequested = !applyDelta(payload) || resyncRequested;
      }
    }
    // Settle predictions only against an accepted delta or a replacement snapshot.
    // A resync-rejected delta leaves the authoritative document stale until the
    // requested snapshot arrives, so predicted text must survive until then.
    if (snapshotApplied || !resyncRequested) {
      state.pending = dropSettled(state.pending, settledId);
    }
    if (!state.sections) { statusEl.textContent = 'no sections yet'; return; }

    // Reset the browser-owned query on a fresh open: a closed->open transition, or
    // a reopen at the same open state signalled by a bumped picker epoch.
    const nowOpen = paletteOpen();
    const epoch = pickerEpochFromPalette(state.sections.palette);
    if (shouldResetLocalQuery(nowOpen, wasPaletteOpen, epoch, lastPickerEpoch)) {
      state.palette.query = '';
      state.palette.selected = 0;
    }
    wasPaletteOpen = nowOpen;
    lastPickerEpoch = epoch;

    render();
    if (!resyncRequested) statusEl.textContent = 'live (' + e.data.byteLength + ' bytes)';
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
  // local ranking; Enter submits the selected candidate id; Escape closes via the
  // library keymap (prompt.cancel). Nothing here touches the document.
  if (paletteOpen()) {
    const p = state.palette;
    if (ev.key === 'Enter') {
      ev.preventDefault();
      p.selected = clampPaletteSelection(p.selected, locallyRankedPaletteRows(state.sections && state.sections.palette).length);
      const rows = locallyRankedPaletteRows(state.sections && state.sections.palette);
      const candidate = rows[p.selected];
      const message = candidate && encodePaletteSubmit(candidate.id);
      if (message) ws.send(message);
      return;
    }
    if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
      ev.preventDefault();
      // selected is an absolute ranked index over the locally-ranked rows.
      const rows = locallyRankedPaletteRows(state.sections && state.sections.palette);
      p.selected = clampPaletteSelection(ev.key === 'ArrowDown' ? p.selected + 1 : p.selected - 1, rows.length);
      render();
      return;
    }
    if (ev.key === 'Backspace') {
      ev.preventDefault();
      p.query = Array.from(p.query).slice(0, -1).join('');
      p.selected = 0;
      refreshFinder();
      return;
    }
    if (Array.from(ev.key).length === 1 && !ev.altKey) {
      ev.preventDefault();
      p.query += ev.key;
      p.selected = 0;
      refreshFinder();
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
