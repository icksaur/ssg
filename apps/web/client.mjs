// The browser client: DOM rendering and WebSocket/keyboard I/O. All pure wire
// and offset logic lives in reconcile.mjs so a node test can exercise it without
// a DOM. The client renders the semantic snapshot natively -- document text
// colored by syntax scope, caret and selection from the semantic selection set,
// a tab strip -- with every color drawn from the theme's roles mapped to CSS
// custom properties. Keystrokes go back as KEY frames.

import { decodeValue, findSections, num, cssColor, byteToIndex } from '/reconcile.mjs';

const statusEl = document.getElementById('status');
const tabsEl = document.getElementById('tabs');
const docEl = document.getElementById('doc');

const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const idKey = (v) => JSON.stringify(v, (k, x) => typeof x === 'bigint' ? x.toString() : x);

// Role ordinals the renderer maps to CSS custom properties; pinned by
// test_theme's role-ordinal contract so a reorder cannot silently mis-color.
const ROLE = { text: 0, canvas: 1, caret: 2, selection: 3 };

function applyTheme(theme) {
  const root = document.documentElement.style;
  const rc = (theme && Array.isArray(theme.role_colors)) ? theme.role_colors : [];
  // Re-derive every property each snapshot, clearing any set by an earlier theme,
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

// Segment the text at every syntax-span edge, selection edge, and the caret;
// color each segment by its syntax scope through the theme and mark selected
// segments and the caret. Geometry is the browser's; only offsets are semantic.
function renderDoc(sections, syntaxColors) {
  const text = sections.document.text;
  const caret = num(sections.document.caret);
  const spans = (sections.syntax && Array.isArray(sections.syntax.spans)) ? sections.syntax.spans : [];
  const sels = (sections.selection && Array.isArray(sections.selection.selections)) ? sections.selection.selections : [];

  const ranges = [];
  for (const s of sels) {
    const a = num(s.anchor.byte_offset), b = num(s.active.byte_offset);
    if (a !== b) ranges.push([Math.min(a, b), Math.max(a, b)]);
  }

  const total = new TextEncoder().encode(text).length;
  const bounds = new Set([0, total, caret]);
  for (const sp of spans) { bounds.add(num(sp.begin)); bounds.add(num(sp.end)); }
  for (const r of ranges) { bounds.add(r[0]); bounds.add(r[1]); }
  const cuts = [...bounds].filter(b => b >= 0 && b <= total).sort((a, b) => a - b);
  const map = byteToIndex(text, cuts);

  const scopeAt = (byte) => { for (const sp of spans) { if (byte >= num(sp.begin) && byte < num(sp.end)) return num(sp.scope); } return -1; };
  const selectedAt = (byte) => ranges.some(r => byte >= r[0] && byte < r[1]);

  let html = '';
  for (let i = 0; i + 1 < cuts.length; i++) {
    const a = cuts[i], b = cuts[i + 1];
    if (a === caret) html += '<span class="caret"></span>';
    const ia = map.get(a), ib = map.get(b);
    if (ia === undefined || ib === undefined || ib <= ia) continue;
    const scope = scopeAt(a);
    // Both span.scope and syntax_colors come from the same snapshot, so the
    // index is self-consistent (unlike the hardcoded role ordinals); the bounds
    // check only guards a truncated palette, degrading to the default color.
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
    const [tree] = decodeValue(dv, 2);
    const sections = findSections(tree);
    if (!sections) { statusEl.textContent = 'no sections in snapshot'; return; }
    const syntaxColors = applyTheme(sections.theme);
    renderTabs(sections.tabs);
    renderDoc(sections, syntaxColors);
    statusEl.textContent = 'live (' + e.data.byteLength + ' bytes)';
    docEl.focus();
  } catch (err) { statusEl.textContent = 'render error: ' + err.message; }
};
ws.onclose = () => { statusEl.textContent += ' [closed]'; };
ws.onerror = () => { statusEl.textContent = 'ws error'; };

docEl.addEventListener('keydown', (ev) => {
  // Send every keydown as code + modifiers + (printable text, if any); the host
  // resolves bindings and routes text through the one shared input seam.
  const mods = (ev.ctrlKey ? 'c' : '') + (ev.altKey ? 'a' : '') +
               (ev.metaKey ? 'm' : '') + (ev.shiftKey ? 's' : '');
  // Array.from counts Unicode scalars, so a supplementary-plane character (two
  // UTF-16 code units in ev.key) still registers as one printable scalar and is
  // sent, consistent with the UTF-8 offset contract.
  const printable = Array.from(ev.key).length === 1 && !ev.ctrlKey && !ev.metaKey;
  const text = printable ? ev.key : '';
  ev.preventDefault();
  ws.send('KEY:' + ev.code + ':' + mods + ':' + text);
});
