// The browser client: DOM rendering and WebSocket/keyboard I/O. All pure wire,
// offset, and reconciliation logic lives in reconcile.mjs so a node test can
// exercise it without a DOM. The client renders the semantic model natively --
// document text colored by syntax scope, caret and selection from the semantic
// selection set, a tab strip -- with every color drawn from the theme's roles
// mapped to CSS custom properties, and echoes typed text locally (M3) so a
// keystroke shows before its round trip completes.

import {
  findSections, num, cssColor, byteToIndex, utf8Bytes, project, decodeMessage,
  browserInboundKind,
  matcherBoundsFromPalette, clampPaletteSelection, pickerCandidatesFromPalette,
  resolvePickerLifecycle, effectivePickerMode, encodePickerSubmit,
  encodeSelectionByteRange, encodeTabAction, markedTextByteOffset,
  applySessionDeltaSections, applyTreeDelta,
  interpretChrome, firstUnsupportedPrimitive, SIZE, AXIS, WIDGET, SURFACE, SCROLL,
  webExtentCss, applyNodeSemanticStyle, getOrCreateStyledNode,
  firstMalformedNodeStyle, roleColor,
  GenerationRetainedCache, gitAffordanceFromNode,
  preferredKeyboardSurface, browserRenderPlan, settlePointerSelection,
  applyPalettePresenceOverlay,
  BrowserKeyDispatchTracker,
  encodeCommandRequest, encodeClientInput, encodeTreeActivation,
  encodeStatusActionInvocation,
  encodePromptFocus,
  noticeViewFromSections,
  externalModificationFromSections, externalFocusHeld, encodeExternalAction,
  settleCommandResult, settleInput, isCurrentGeneration, replayAttachFrame,
  deltaIsContiguous,
  clearUncertainInputs, reconnectDelay,
  predictPromptValue,
  settlePromptPresentation, deferPromptDocumentSurface,
} from '/reconcile.mjs';
import { fuzzyRank } from '/fuzzy.mjs';

const statusEl = document.getElementById('status');
const chromeErrorEl = document.getElementById('chrome-error');
const uiRootEl = document.getElementById('ui-root');

const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const idKey = (v) => JSON.stringify(v, (k, x) => typeof x === 'bigint' ? x.toString() : x);

// Role ordinals the renderer maps to CSS custom properties; pinned by
// test_theme's role-ordinal contract so a reorder cannot silently mis-color.
const ROLE = { text: 0, canvas: 1, caret: 2, selection: 3, statusWarning: 13,
               tabActive: 6, tabInactive: 7, statusInfo: 12,
               scrollbarTrack: 17, scrollbarThumb: 18,
               diffAdded: 19, diffRemoved: 20, diffModified: 21 };
const FOCUS_EDITOR = 0;   // FocusTarget::Editor ordinal.
const DOCUMENT_VIEWPORT_NODE_ID = 'document.viewport';

// Persistent client model: the authoritative sections plus the still-unsettled
// local predictions. Snapshots replace `sections`; deltas mutate it in place.
const state = {
  sections: null,
  revision: 0n,
  pending: [],
  inputQueue: [],
  nextEditId: 1,
  promptPrediction: null,
  promptScrollOverride: false,
  skipNextCaretReveal: false,
  // The palette/finder is a client-owned derived view: the browser owns the
  // query text and selection index, and ranks the published candidate universe locally.
  palette: { mode: null, query: '', selected: 0, returnFocus: null },
};
const retainedNodes = new GenerationRetainedCache();
let renderedSurfaceKinds = new Set();
let renderedFooterPromptHost = null;
let renderedFooterPromptActiveId = null;
let pickerInputElement = null;
const allSurfaceKinds = Object.values(SURFACE);
const fullRenderPlan = () => ({
  rebuild: true,
  reconcile: true,
  repaintTheme: true,
  surfaces: allSurfaceKinds,
});
const surfaceRenderPlan = (...surfaces) => ({
  rebuild: false,
  reconcile: false,
  repaintTheme: false,
  surfaces,
});

function paletteOpen() {
  return state.palette.mode != null;
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
  set('--ssg-scrollbar-track', ROLE.scrollbarTrack);
  set('--ssg-scrollbar-thumb', ROLE.scrollbarThumb);
  return (theme && Array.isArray(theme.syntax_colors)) ? theme.syntax_colors : [];
}

function renderTabsInto(host, tabs, theme) {
  host.textContent = '';
  if (tabs && Array.isArray(tabs.tabs)) {
    const activeId = idKey(tabs.active);
    for (const t of tabs.tabs) {
      const el = document.createElement('button');
      el.type = 'button';
      el.className = 'tab' + (idKey(t.id) === activeId ? ' active' : '');
      el.style.color = roleColor(
        idKey(t.id) === activeId ? ROLE.tabActive : ROLE.tabInactive, theme);
      el.textContent = (t.dirty ? '\u25CF ' : '') + (t.label || '');
      el.setAttribute(
        'aria-label', (t.label || '') + (t.dirty ? ', modified' : ''));
      if (idKey(t.id) === activeId) el.setAttribute('aria-current', 'page');
      el.addEventListener('click', () => {
        sendCommandFrame(encodeTabAction('tab.activate', t.id, state.revision));
      });
      el.addEventListener('mousedown', (event) => {
        if (event.button === 1) event.preventDefault();
      });
      el.addEventListener('auxclick', (event) => {
        if (event.button !== 1) return;
        event.preventDefault();
        sendCommandFrame(encodeTabAction('tab.close', t.id, state.revision));
      });
      host.appendChild(el);
    }
  }
}

function renderPickerInput(el, sigil) {
  el.className = 'w input-line';
  el.textContent = '';
  const marker = document.createElement('span');
  marker.textContent = sigil || '';
  el.appendChild(marker);
  el.appendChild(document.createTextNode(state.palette.query || ''));
  const caret = document.createElement('span');
  caret.className = 'caret';
  el.appendChild(caret);
}

// Build the DOM for one interpreted render node, mirroring the generic tree: a
// container becomes a flex div on its axis (a Flex node grows, a gap spaces its
// children); a leaf becomes a span. `parentAxis` is the axis the node's own Size
// measures along (a child sizes along its parent's main axis). Returns null for an
// omitted node.
function reconcileChildren(parent, children) {
  const retained = new Set(children);
  for (const child of [...parent.childNodes]) {
    if (!retained.has(child)) child.remove();
  }
  let cursor = parent.firstChild;
  for (const child of children) {
    if (child === cursor) {
      cursor = cursor.nextSibling;
    } else {
      parent.insertBefore(child, cursor);
    }
  }
}

function renderChromeNode(node, theme, plan, parentAxis = AXIS.ROW,
                          inFooterPrompt = false) {
  if (!node) return null;
  if (node.kind === 'container') {
    const div = getOrCreateStyledNode(
      retainedNodes, node, theme, () => document.createElement('div'));
    const isFooterPrompt = node.id === 'footer.prompt';
    div.className = 'group' + (isFooterPrompt ? ' prompt-surface' : '');
    if (isFooterPrompt) {
      div.tabIndex = 0;
      div.setAttribute('role', 'group');
      renderedFooterPromptHost = div;
    }
    div.dataset.nodeId = node.id;
    div.style.display = 'flex';
    div.style.flexDirection = node.axis === 1 ? 'column' : 'row';  // Axis: Row=0, Column=1
    // stretch: a child shares the parent's cross extent (the Row/Column contract),
    // rather than shrinking to its content on the cross axis.
    div.style.alignItems = 'stretch';
    div.style.boxSizing = 'border-box';  // inset stays inside the published extent
    applySize(div, node.size, parentAxis);
    applyInset(div, node.inset, node.size, parentAxis);
    // An independent scroll viewport (the panel and content containers): clip
    // content to this node's bounded extent and scroll within it. min-height:0
    // lets this flex child shrink so overflow-y:auto actually clips rather than
    // growing the parent. Derived from the tree's ScrollAxis, never hard-coded.
    if (node.scroll === SCROLL.VERTICAL) {
      div.style.overflowY = 'auto';
      div.style.minHeight = '0';
    }
    if (node.id === DOCUMENT_VIEWPORT_NODE_ID) {
      const noteUserScroll = () => {
        if (state.promptPrediction) state.promptScrollOverride = true;
      };
      div.onwheel = noteUserScroll;
      div.onpointerdown = noteUserScroll;
    }
    if (node.gap) div.style.gap = extentCss(node.gap, node.axis);
    const children = node.children
      .map((child) => renderChromeNode(
        child, theme, plan, node.axis, inFooterPrompt || isFooterPrompt))
      .filter(Boolean);
    reconcileChildren(div, children);
    return div;
  }
  // leaf
  if (node.spacer) {
    const gap = retainedNodes.getOrCreate(
      node.id, () => document.createElement('span'));
    gap.className = 'w spacer';
    gap.style.display = 'inline-block';
    applyNodeSemanticStyle(gap.style, node, theme);
    if (node.width != null) {
      gap.style[parentAxis === AXIS.COLUMN ? 'height' : 'width'] =
        extentCss(node.width, parentAxis);
      gap.style.flex = '0 0 auto';
    } else applySize(gap, node.size, parentAxis);
    return gap;
  }
  if (node.widget === WIDGET.VIEW) {
    const el = renderSurfaceNode(node, plan);
    applyNodeSemanticStyle(el.style, node, theme);
    applySize(el, node.size, parentAxis);
    return el;
  }
  if (node.widget === WIDGET.STATUS_ACTIONS) {
    const el = retainedNodes.getOrCreate(
      node.id, () => document.createElement('span'));
    if (plan.rebuild || plan.reconcile || plan.repaintTheme) {
      renderStatusActionsNode(el, theme);
    }
    applyNodeSemanticStyle(el.style, node, theme);
    applySize(el, node.size, parentAxis);
    return el;
  }
  if (node.widget === WIDGET.TEXT_INPUT) {
    const el = retainedNodes.getOrCreate(
      node.id, () => document.createElement('span'));
    if (node.active != null) {
      const predicted = state.promptPrediction &&
        state.promptPrediction.controlId === node.controlId
          ? state.promptPrediction.value : null;
      el.className = 'prompt-control prompt-input' +
        (node.active ? ' active' : '');
      el.id = 'prompt-input-' + node.controlId;
      el.setAttribute('role', 'textbox');
      el.setAttribute('aria-label', node.label || '');
      el.textContent = predicted == null ? (node.text || '') : predicted;
      if (node.active) {
        el.appendChild(document.createElement('span')).className = 'caret';
        renderedFooterPromptActiveId = node.controlId;
      }
      el.onmousedown = (event) => {
        event.preventDefault();
        if (renderedFooterPromptHost) {
          renderedFooterPromptHost.focus({ preventScroll: true });
        }
        sendCommandFrame(encodePromptFocus(node.controlId, state.revision));
      };
    } else {
      pickerInputElement = el;
      el._ssgPickerSigil = node.sigil || '';
      renderPickerInput(el, el._ssgPickerSigil);
    }
    applyNodeSemanticStyle(el.style, node, theme);
    applySize(el, node.size, parentAxis);
    return el;
  }
  const el = retainedNodes.getOrCreate(
    node.id, () => document.createElement('span'));
  el.className = inFooterPrompt
    ? 'prompt-control' +
      (node.checked != null ? ' prompt-toggle' : ' prompt-count') +
      (node.checked ? ' checked' : '')
    : 'w' + (node.command ? ' clickable' : '');
  el.textContent = (node.checked != null ? (node.checked ? '\u2611 ' : '\u2610 ') : '') + (node.text || '');
  applyNodeSemanticStyle(el.style, node, theme);
  applySize(el, node.size, parentAxis);
  el.title = node.command || '';
  el.onclick = node.command ? () => sendCommand(node.command) : null;
  if (inFooterPrompt && node.checked != null) {
    el.setAttribute('role', 'checkbox');
    el.setAttribute('aria-checked', node.checked ? 'true' : 'false');
  }
  if (inFooterPrompt && node.label) el.setAttribute('aria-label', node.label);
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
    const ranges = [];
    const preview = pointerSelection.preview;
    if (preview && samePointerBasis(preview.basis, currentPointerBasis())) {
      const a = shiftEnd(preview.anchor), b = shiftEnd(preview.active);
      if (a !== b) ranges.push([Math.min(a, b), Math.max(a, b)]);
    } else {
      const sels = (s.selection && Array.isArray(s.selection.selections))
        ? s.selection.selections : [];
      for (const sel of sels) {
        const a = shiftEnd(num(sel.anchor.byte_offset));
        const b = shiftEnd(num(sel.active.byte_offset));
        if (a !== b) ranges.push([Math.min(a, b), Math.max(a, b)]);
      }
    }
    const caret = preview ? shiftEnd(preview.active) : proj.caret;
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
      if (a === caret) {
        html += '<span class="caret" data-byte-start="' + a + '"></span>';
      }
      const ia = map.get(a), ib = map.get(b);
      if (ia === undefined || ib === undefined || ib <= ia) continue;
      const scope = scopeAt(a);
      const color = (scope >= 0 && scope < syntaxColors.length) ? cssColor(syntaxColors[scope]) : '';
      const cls = selectedAt(a) ? ' class="sel"' : '';
      const style = color ? ' style="color:' + color + '"' : '';
      html += '<span data-byte-start="' + a + '"' + cls + style + '>' +
        esc(text.substring(ia, ib)) + '</span>';
    }
    if (caret >= total) {
      html += '<span class="caret" data-byte-start="' + total + '"></span>';
    }
    host.innerHTML = html;
    host._ssgProjection = {
      predStart: proj.predStart, predEnd: proj.predEnd,
      predBytes, authoritativeBytes: utf8Bytes(authText),
    };
    host.onpointerdown = (event) => beginPointerSelection(host, event);
    host.onpointermove = (event) => updatePointerSelection(host, event);
    host.onpointerup = (event) => endPointerSelection(host, event);
    host.onpointercancel = cancelPointerGesture;
}

function renderSurfaceNode(node, plan) {
    let created = false;
    const el = retainedNodes.getOrCreate(node.id, () => {
      created = true;
      const value = document.createElement('div');
      return value;
    });
    if (created) {
      if (node.surface === SURFACE.DOCUMENT ||
         node.surface === SURFACE.FINDRESULTS) {
       el.tabIndex = 0;
      }
      el.dataset.surface = String(node.surface);
    }
    renderedSurfaceKinds.add(node.surface);
    if (created) el.className = 'surface surface-' + node.surface;
    if (created || !el.isConnected || plan.surfaces.includes(node.surface)) {
      renderSurfaceContent(el, node.surface);
    }
    return el;
}

function renderSurfaceContent(el, surface) {
  const s = state.sections || {};
  if (surface === SURFACE.TABBAR) {
    el.classList.add('tabs');
    renderTabsInto(el, s.tabs, s.theme);
  } else if (surface === SURFACE.DOCUMENT) {
    el.classList.add('doc-surface');
    renderDocumentInto(el);
  } else if (surface === SURFACE.FILETREE ||
             surface === SURFACE.GITSTATUS ||
             surface === SURFACE.SYMBOLS) {
    el.textContent = '';
    renderTreeSurface(el, surface, s.tree);
  } else if (surface === SURFACE.FINDRESULTS) {
    el.textContent = '';
    el.classList.add('find-results-surface');
    renderFindResultsSurface(
      el, s.palette, effectivePickerMode(s.palette, state.palette.mode));
  } else if (surface === SURFACE.NOTICE) {
    el.textContent = '';
    el.classList.add('notice-surface');
    renderNotice(el, s);
  } else if (surface === SURFACE.EXTERNAL_MODIFICATION) {
    el.textContent = '';
    el.classList.add('external-surface');
    renderExternalModification(el, s);
  } else {
    throw new Error('unsupported retained surface ' + surface);
  }
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
      const git = surface === SURFACE.GITSTATUS ? gitAffordanceFromNode(n) : null;
      if (git) {
        const marker = document.createElement('span');
        marker.className = 'git-status';
        marker.textContent = git.shortLabel + ' ';
        div.appendChild(marker);
        const color = roleColor(num(git.role), state.sections && state.sections.theme);
        if (color) div.style.color = color;
      }
      div.appendChild(document.createTextNode((n.icon ? n.icon + ' ' : '') + (n.label || '')));
      // A click selects then activates the node -- opening a file or toggling a
      // directory -- the same library commands a TUI pointer press dispatches.
      if (typeof n.id === 'string') {
        div.addEventListener('click', () =>
          sendCommandFrame(encodeTreeActivation(n.id, state.revision)));
      }
      parent.appendChild(div);
    }
}

function locallyRankedPaletteRows(palette, mode = state.palette.mode) {
    const candidates = pickerCandidatesFromPalette(palette, mode);
    const { params, maxMagnitude, maxCandidateBytes } = matcherBoundsFromPalette(palette);
    const order = fuzzyRank(candidates, state.palette.query || '', params, maxMagnitude, maxCandidateBytes);
    return order.map((i) => candidates[i]);
}

function submitPaletteCandidate(mode, candidate) {
    if (!candidate) return;
    sendCommandFrame(
      encodePickerSubmit(mode, candidate.id, state.revision));
    const returnFocus = state.palette.returnFocus;
    state.palette.mode = null;
    state.palette.query = '';
    state.palette.selected = 0;
    state.palette.returnFocus = null;
    render({ ...surfaceRenderPlan(), reconcile: true });
    if (returnFocus && returnFocus.isConnected) {
      returnFocus.focus({ preventScroll: true });
    } else {
      editorFocusElement().focus({ preventScroll: true });
    }
}

function renderFindResultsSurface(parent, palette, mode = state.palette.mode) {
    let rows = [];
    try {
      rows = locallyRankedPaletteRows(palette, mode);
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
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'row' + (i === state.palette.selected ? ' sel' : '');
      button.innerHTML = '<span class="label">' + esc(rows[i].label || '') +
        '</span><span class="detail">' + esc(rows[i].detail || '') + '</span>';
      button.addEventListener('click', () => submitPaletteCandidate(mode, rows[i]));
      parent.appendChild(button);
    }
}

function renderStatusActionsNode(el, theme) {
    el.textContent = '';
    el.className = 'status-actions';
    const status = state.sections && state.sections.prompt_status && state.sections.prompt_status.status;
    const items = status && Array.isArray(status.items) ? status.items : [];
    const item = items[status ? (num(status.selected) || 0) : 0];
    for (const action of (item && Array.isArray(item.actions) ? item.actions : [])) {
      const button = document.createElement('button');
      button.textContent = action.accessible_label || action.accessibleLabel || action.id || '';
      button.style.color = roleColor(ROLE.statusInfo, theme);
      button.addEventListener('click', () =>
        sendCommandFrame(encodeStatusActionInvocation({
          statusId: item.id, actionId: action.id, generation: item.generation,
        })));
      el.appendChild(button);
  }
}

// Apply a published Size to a flex child ALONG the parent's main axis: Exact => a
// fixed extent that neither grows nor shrinks (including a literal zero extent, whose
// content is CLIPPED so it truly occupies zero), Flex => grow to fill (extent is the
// grow weight, default 1), Auto => content-sized. A Row parent measures width; a
// Column parent measures height. SIZE ordinals mirror the C++ SizeKind enum.
function extentCss(value, axis) {
  return webExtentCss(value, axis);
}

function applySize(el, size, parentAxis) {
  if (!size) return;
  const dim = parentAxis === AXIS.COLUMN ? 'height' : 'width';
  if (size.kind === SIZE.EXACT) {
    el.style.flex = '0 0 auto';
    el.style[dim] = extentCss(size.extent || 0, parentAxis);
    el.style.overflow = 'hidden';  // content beyond the extent is clipped, not overflowed
  } else if (size.kind === SIZE.FLEX) {
    el.style.flex = (size.extent > 0 ? size.extent : 1) + ' 1 0';
  } else {
    el.style.flex = '0 0 auto';  // Auto: content extent
  }
}

// Apply a container Inset as axis-aware per-side padding. When the node has an Exact
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
  el.style.paddingTop = extentCss(top, AXIS.COLUMN);
  el.style.paddingRight = extentCss(right, AXIS.ROW);
  el.style.paddingBottom = extentCss(bottom, AXIS.COLUMN);
  el.style.paddingLeft = extentCss(left, AXIS.ROW);
}

// Interpret the entire published UI-VM tree into one generic mount. Retained View
// elements move between rebuilt structural wrappers without leaving the connected
// document, preserving native focus and control state within a schema generation.
// A schema using a primitive this
// build does not implement is a loud, visible refusal -- never a silently dropped
// element.
function renderChrome(sections, plan) {
  const schema = sections.ui;
  const stateSection = sections.ui_state;
  let presenceSection = sections.ui_presence;
  let overlayFailureFocus = null;
  chromeErrorEl.textContent = '';
  if (!schema || !schema.root) return false;

  const malformedStyle = firstMalformedNodeStyle(schema);
  if (malformedStyle) {
    chromeErrorEl.textContent =
      'unsupported UI style on ' + malformedStyle.id +
      ' -- this client build cannot render the composed UI';
    uiRootEl.textContent = '';
    return false;
  }
  const unsupported = firstUnsupportedPrimitive(schema);
  if (unsupported) {
    chromeErrorEl.textContent =
      'unsupported UI ' + unsupported.kind + ' ' + unsupported.ordinal +
      ' -- this client build cannot render the composed UI';
    uiRootEl.textContent = '';
    return false;
  }
  if (paletteOpen()) {
    const applied = applyPalettePresenceOverlay(
      schema, presenceSection,
      sections.palette && sections.palette.presence_overlay);
    if (applied.error) {
      chromeErrorEl.textContent = applied.error;
      overlayFailureFocus = state.palette.returnFocus;
      state.palette.mode = null;
      state.palette.query = '';
      state.palette.selected = 0;
      state.palette.returnFocus = null;
    } else if (!applied.stale) {
      presenceSection = applied.presence;
    }
  }
  const interpreted = interpretChrome(schema, stateSection, presenceSection);
  if (!interpreted || !interpreted.root) return false;  // schema/state from different frames; wait

  const generation = num(schema.generation);
  const generationChanged = retainedNodes.begin(generation);
  if (generationChanged) {
    pickerInputElement = null;
  }
  renderedSurfaceKinds = new Set();
  renderedFooterPromptHost = null;
  renderedFooterPromptActiveId = null;
  const nextRoot = renderChromeNode(
    interpreted.root, sections.theme, plan, AXIS.COLUMN);
  if (!nextRoot) return false;
  if (generationChanged || nextRoot.parentElement !== uiRootEl) {
    reconcileChildren(uiRootEl, [nextRoot]);
  }
  reconcileFooterPromptFocus(renderedFooterPromptHost,
                             renderedFooterPromptActiveId,
                             generationChanged);
  if (overlayFailureFocus !== null) {
    const target =
      overlayFailureFocus.isConnected
        ? overlayFailureFocus
        : editorFocusElement();
    target.focus({ preventScroll: true });
  }
  return true;
}

// The footer prompt renders into a retained View element. The structural tree may
// be rebuilt around it, but the focused element stays connected and focus is moved
// only on open and on active-input change, never per message. The container holds
// keyboard focus (tabindex=0, aria-activedescendant naming the active input); its
// child inputs are role=textbox but non-focusable, so a screen reader tracks the
// active input without the browser moving focus into it. The container is a
// role=group so it can own aria-activedescendant across every prompt kind.
let footerPromptOpen = false;
let footerPromptActiveInput = null;
let savedFocusEl = null;

function editorFocusElement() {
  const connected = [...retainedNodes.values()].filter(
    (el) => el.isConnected && el.dataset.surface != null);
  const preferred = preferredKeyboardSurface(
    connected.map((el) => Number(el.dataset.surface)));
  const target = connected.find(
    (el) => Number(el.dataset.surface) === preferred);
  if (target) return target;
  return uiRootEl;
}

function reconcileFooterPromptFocus(host, activeInput, forceFocus = false) {
  if (!host) {
    if (footerPromptOpen) {
      const restore = (savedFocusEl && document.contains(savedFocusEl))
        ? savedFocusEl : editorFocusElement();
      restore.focus({ preventScroll: true });
      savedFocusEl = null;
    }
    footerPromptOpen = false;
    footerPromptActiveInput = null;
    return;
  }
  if (!footerPromptOpen) savedFocusEl = document.activeElement;
  if (activeInput != null) {
    host.setAttribute('aria-activedescendant',
                      'prompt-input-' + activeInput);
  } else {
    host.removeAttribute('aria-activedescendant');
  }
  if (forceFocus || !footerPromptOpen ||
      activeInput !== footerPromptActiveInput) {
    host.focus({ preventScroll: true });
  }
  footerPromptOpen = true;
  footerPromptActiveInput = activeInput;
}

// Reconcile the retained notice surface against the published semantic NoticeView.
// active document raises no draft-conflict notice). The bar shows the message and
// clickable bracketed action labels; each action dispatches its command id through
// the shared command ingress. The notice captures no keyboard focus -- it is
// intrinsic-height chrome above the document, not an input surface.
function renderNotice(host, sections) {
  const nv = noticeViewFromSections(sections);
  if (!nv) {
    host.removeAttribute('role');
    host.removeAttribute('aria-label');
    return;
  }
  host.setAttribute('role', 'status');
  host.setAttribute('aria-label', nv.text);
  const text = document.createElement('span');
  text.className = 'notice-text';
  text.textContent = nv.text;
  host.appendChild(text);
  for (const action of nv.actions) {
    const el = document.createElement('button');
    el.className = 'notice-action';
    el.setAttribute('aria-label', action.label);
    el.textContent = '[' + action.label + ']';
    el.addEventListener('click', () => sendCommand(action.command));
    host.appendChild(el);
  }
}

// Reconcile the retained external-modification surface against its semantic section.
// when no file is externally changed). Renders the message plus one row per file
// (status glyph + path + its offered action buttons), highlighting the selected
// row. A click sends the published file/action identity through the typed route;
// forwarded keystrokes resolve in the library's external context.
function renderExternalModification(host, sections) {
  const view = externalModificationFromSections(sections);
  if (!view) {
    host.removeAttribute('role');
    host.removeAttribute('aria-label');
    return;
  }
  host.setAttribute('role', 'status');
  host.setAttribute('aria-label', view.message);
  const header = document.createElement('div');
  header.className = 'external-header';
  header.textContent = view.message;
  host.appendChild(header);
  for (const file of view.files) {
    const row = document.createElement('div');
    row.className = 'external-row' + (file.selected ? ' selected' : '');
    const label = document.createElement('span');
    label.className = 'external-file';
    label.textContent = file.statusLabel + ' ' + file.path;
    label.setAttribute('aria-label', file.accessibleStatus + ' ' + file.path);
    row.appendChild(label);
    for (const action of file.actions) {
      const el = document.createElement('button');
      el.className = 'external-action';
      el.setAttribute('aria-label', action.label);
      el.textContent = '[' + action.label + ']';
      el.addEventListener('click', () =>
        sendCommandFrame(
          encodeExternalAction(action.action, file.id, state.revision)));
      row.appendChild(el);
    }
    host.appendChild(row);
  }
}

function refreshFinder() {
  if (pickerInputElement && pickerInputElement.isConnected) {
    renderPickerInput(
      pickerInputElement, pickerInputElement._ssgPickerSigil);
  }
  render(surfaceRenderPlan(SURFACE.FINDRESULTS));
}

function applyDelta(d) {
  if (!state.sections || !deltaIsContiguous(state.revision, d)) return false;
  const pointerBasis = currentPointerBasis();
  const next = structuredClone(state.sections);
  applySessionDeltaSections(next, d);
  // The tree is retained and spliced in place; only a genuinely inexpressible
  // tree transition (snapshot_required, a missed base revision, or a malformed
  // splice) falls back to a full snapshot, so ordinary expand/open/select no
  // longer churns the panel through a resync.
  if (d.tree && !applyTreeDelta(next.tree, d.tree)) return false;
  state.sections = next;
  state.revision = BigInt(d.revision);
  if (!samePointerBasis(pointerBasis, currentPointerBasis())) {
    invalidatePointerOffsets();
  }
  return true;
}

// Segment the projected text at every syntax-span edge, selection edge, and the
// caret; color each segment by its syntax scope through the theme and mark
// selected segments and the caret. Authoritative offsets are shifted past any
// predicted text; geometry is the browser's, only offsets are semantic.
function render(plan = fullRenderPlan()) {
  const s = state.sections;
  if (!s) return;
  const effectivePlan = state.promptPrediction && !plan.rebuild
    ? { ...plan,
        surfaces: deferPromptDocumentSurface(plan.surfaces, true) }
    : plan;
  if (effectivePlan.rebuild || effectivePlan.repaintTheme) applyTheme(s.theme);
  if (effectivePlan.rebuild || effectivePlan.reconcile) {
    renderChrome(s, effectivePlan);
  } else {
    const dirty = new Set(effectivePlan.surfaces);
    for (const el of retainedNodes.values()) {
      const surface = Number(el.dataset.surface);
      if (el.isConnected && dirty.has(surface)) {
        renderSurfaceContent(el, surface);
      }
    }
  }
  if (effectivePlan.rebuild ||
      effectivePlan.surfaces.includes(SURFACE.DOCUMENT)) {
    revealDocumentCaret();
  }
}

let lastCaretRevealKey = '';
const pointerSelection = {
  dragging: false,
  pointerId: null,
  anchor: null,
  active: null,
  basis: null,
  released: false,
  preview: null,
  inFlight: null,
  queued: null,
  point: null,
};
let commandRequests = [];

function currentPointerBasis() {
  const sections = state.sections || {};
  return {
    text: sections.document ? sections.document.text : '',
    tab: sections.tabs ? idKey(sections.tabs.active) : '',
  };
}

function samePointerBasis(left, right) {
  return !!left && !!right &&
    left.text === right.text && left.tab === right.tab;
}

function renderPointerPreview() {
  render(surfaceRenderPlan(SURFACE.DOCUMENT));
}

function clearPointerGesture() {
  pointerSelection.dragging = false;
  pointerSelection.pointerId = null;
  pointerSelection.anchor = null;
  pointerSelection.active = null;
  pointerSelection.basis = null;
  pointerSelection.released = false;
  pointerSelection.point = null;
}

function cancelPointerGesture() {
  const host = uiRootEl.querySelector('.doc-surface');
  if (host && pointerSelection.pointerId != null &&
      host.hasPointerCapture(pointerSelection.pointerId)) {
    host.releasePointerCapture(pointerSelection.pointerId);
  }
  clearPointerGesture();
  const retained = pointerSelection.queued || pointerSelection.inFlight;
  pointerSelection.preview =
    retained && samePointerBasis(retained.basis, currentPointerBasis())
      ? retained : null;
  renderPointerPreview();
}

function cancelPointerSelection() {
  clearPointerGesture();
  pointerSelection.preview = null;
  pointerSelection.inFlight = null;
  pointerSelection.queued = null;
}

function invalidatePointerOffsets() {
  clearPointerGesture();
  pointerSelection.preview = null;
  pointerSelection.queued = null;
}

function displayByteOffsetAtPoint(host, x, y) {
  let node = null;
  let offset = 0;
  if (typeof document.caretPositionFromPoint === 'function') {
    const position = document.caretPositionFromPoint(x, y);
    if (position) {
      node = position.offsetNode;
      offset = position.offset;
    }
  } else if (typeof document.caretRangeFromPoint === 'function') {
    const range = document.caretRangeFromPoint(x, y);
    if (range) {
      node = range.startContainer;
      offset = range.startOffset;
    }
  }
  if (!node || !host.contains(node)) return null;
  if (node.nodeType !== Node.TEXT_NODE) {
    const after = node.childNodes[offset] || null;
    const before = offset > 0 ? node.childNodes[offset - 1] : null;
    const candidate = after || before;
    if (candidate && candidate.nodeType === Node.ELEMENT_NODE) {
      const marked = candidate.matches('[data-byte-start]')
        ? candidate
        : candidate.querySelector('[data-byte-start]');
      if (marked) {
        const start = Number(marked.dataset.byteStart);
        return after
          ? start
          : markedTextByteOffset(start, marked.textContent,
                                 marked.textContent.length);
      }
    }
    return null;
  }
  const marked = node.parentElement &&
    node.parentElement.closest('[data-byte-start]');
  if (!marked || !host.contains(marked)) return null;
  const start = Number(marked.dataset.byteStart);
  return markedTextByteOffset(start, node.data, offset);
}

function authoritativePointerOffset(host, event) {
  const displayed = displayByteOffsetAtPoint(
    host, event.clientX, event.clientY);
  if (displayed == null) return null;
  const projection = host._ssgProjection;
  if (!projection) return displayed;
  if (displayed <= projection.predStart) return displayed;
  if (displayed <= projection.predEnd) return projection.predStart;
  return Math.min(
    projection.authoritativeBytes, displayed - projection.predBytes);
}

function retainPointerPoint(host, event) {
  pointerSelection.point = {
    host, clientX: event.clientX, clientY: event.clientY,
  };
  flushPointerPoint();
}

function flushPointerPoint() {
  if (!pointerSelection.point || state.inputQueue.length || state.pending.length) {
    return;
  }
  let { host, clientX, clientY } = pointerSelection.point;
  if (!host.isConnected) {
    host = uiRootEl.querySelector('.doc-surface');
  }
  if (!host) return;
  const offset = authoritativePointerOffset(
    host, { clientX, clientY });
  if (offset == null) return;
  pointerSelection.point = null;
  if (pointerSelection.anchor == null) {
    pointerSelection.anchor = offset;
    pointerSelection.basis = currentPointerBasis();
  }
  pointerSelection.active = offset;
  pointerSelection.preview = {
    anchor: pointerSelection.anchor,
    active: offset,
    basis: pointerSelection.basis,
    retries: 0,
  };
  renderPointerPreview();
  if (pointerSelection.released) finishPointerGesture();
}

function dispatchPointerRange(range) {
  const sent = sendCommandFrame(encodeSelectionByteRange(
    range.anchor, range.active, state.revision), 'pointer');
  if (!sent) {
    cancelPointerSelection();
    return false;
  }
  pointerSelection.inFlight = range;
  return true;
}

function finishPointerGesture() {
  if (pointerSelection.anchor == null || pointerSelection.active == null ||
      state.inputQueue.length || state.pending.length) {
    pointerSelection.released = true;
    return;
  }
  const range = {
    anchor: pointerSelection.anchor,
    active: pointerSelection.active,
    basis: pointerSelection.basis,
    retries: 0,
  };
  clearPointerGesture();
  pointerSelection.preview = range;
  if (pointerSelection.inFlight) {
    pointerSelection.queued = range;
  } else {
    dispatchPointerRange(range);
  }
}

function settlePointerRange(error) {
  const settled = settlePointerSelection(
    pointerSelection.inFlight, pointerSelection.queued,
    error, currentPointerBasis());
  pointerSelection.inFlight = null;
  pointerSelection.queued = null;
  pointerSelection.preview = settled.preview;
  if (settled.dispatch) dispatchPointerRange(settled.dispatch);
}

function beginPointerSelection(host, event) {
  if (event.button !== 0) return;
  event.preventDefault();
  pointerSelection.dragging = true;
  pointerSelection.pointerId = event.pointerId;
  pointerSelection.anchor = null;
  pointerSelection.active = null;
  pointerSelection.basis = null;
  pointerSelection.released = false;
  pointerSelection.point = null;
  host.setPointerCapture(event.pointerId);
  retainPointerPoint(host, event);
}

function updatePointerSelection(host, event) {
  if (!pointerSelection.dragging ||
      event.pointerId !== pointerSelection.pointerId) {
    return;
  }
  event.preventDefault();
  retainPointerPoint(host, event);
}

function endPointerSelection(host, event) {
  if (!pointerSelection.dragging ||
      event.pointerId !== pointerSelection.pointerId) {
    return;
  }
  event.preventDefault();
  retainPointerPoint(host, event);
  if (host.hasPointerCapture(event.pointerId)) {
    host.releasePointerCapture(event.pointerId);
  }
  pointerSelection.dragging = false;
  pointerSelection.pointerId = null;
  finishPointerGesture();
}

function revealDocumentCaret() {
  const activeTab = state.sections && state.sections.tabs
    ? idKey(state.sections.tabs.active)
    : '';
  const key = activeTab + ':' + String(state.sections.document.caret) +
    ':' + state.pending.map((item) => item.text).join('');
  if (state.skipNextCaretReveal) {
    state.skipNextCaretReveal = false;
    lastCaretRevealKey = key;
    return;
  }
  const caret = uiRootEl.querySelector('.doc-surface .caret');
  if (!caret) return;
  if (key === lastCaretRevealKey) return;
  lastCaretRevealKey = key;
  const viewport = retainedNodes.get(DOCUMENT_VIEWPORT_NODE_ID);
  if (!viewport) return;
  const caretRect = caret.getBoundingClientRect();
  const viewportRect = viewport.getBoundingClientRect();
  if (caretRect.top < viewportRect.top) {
    viewport.scrollTop -= viewportRect.top - caretRect.top;
  } else if (caretRect.bottom > viewportRect.bottom) {
    viewport.scrollTop += caretRect.bottom - viewportRect.bottom;
  }
  if (caretRect.left < viewportRect.left) {
    viewport.scrollLeft -= viewportRect.left - caretRect.left;
  } else if (caretRect.right > viewportRect.right) {
    viewport.scrollLeft += caretRect.right - viewportRect.right;
  }
}

let ws = null;
let socketGeneration = 0;
let reconnectAttempts = 0;
let reconnectTimer = null;

function sendTyped(frame) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return false;
  ws.send(frame);
  return true;
}

function sendCommandFrame(frame, owner = 'other') {
  if (!sendTyped(frame)) return false;
  commandRequests.push(owner);
  return true;
}

function sendCommand(id, payload = null) {
  return sendCommandFrame(
    encodeCommandRequest(id, state.revision, payload));
}

function discardPredictions() {
  const cleared = clearUncertainInputs();
  state.pending = cleared.pending;
  state.inputQueue = cleared.inputQueue;
  state.promptPrediction = null;
  state.promptScrollOverride = false;
  state.skipNextCaretReveal = true;
  render({
    ...surfaceRenderPlan(SURFACE.DOCUMENT),
    reconcile: true,
  });
}

function reconnect(reason) {
  statusEl.textContent = reason + '; reconnecting';
  if (ws) ws.close();
}

function applyProtocolFrame(buffer) {
  frameRenderPlan = surfaceRenderPlan();
  const { kind, payload } = decodeMessage(buffer);
  const inbound = browserInboundKind(kind);
  if (inbound === 'snapshot') {
    state.sections = findSections(payload);
    state.revision = BigInt(payload.revision);
    reconnectAttempts = 0;
    frameRenderPlan = fullRenderPlan();
    invalidatePointerOffsets();
  } else if (inbound === 'delta') {
    if (!applyDelta(payload)) {
      reconnect('state gap');
      return false;
    }
    frameRenderPlan = browserRenderPlan(payload);
  } else if (inbound === 'command-result') {
    if (payload.revision != null && BigInt(payload.revision) > state.revision) {
      reconnect('command result preceded state');
      return false;
    }
    const settled = settleCommandResult(commandRequests);
    if (!settled) {
      reconnect('unexpected command result');
      return false;
    }
    commandRequests = settled.queue;
    if (settled.owner === 'pointer') {
      settlePointerRange(num(payload.error));
      frameRenderPlan = surfaceRenderPlan(SURFACE.DOCUMENT);
    }
  } else if (inbound === 'input-result') {
    const completedInput = state.inputQueue[0];
    const settled = settleInput(
      state.inputQueue, state.pending, payload, state.revision);
    if (!settled) {
      reconnect('input result preceded state');
      return false;
    }
    state.inputQueue = settled.inputQueue;
    state.pending = settled.pending;
    if (state.promptPrediction) {
      const promptPresentation = settlePromptPresentation(
        state.promptPrediction, state.inputQueue, completedInput,
        state.promptScrollOverride);
      state.promptPrediction = promptPresentation.prediction;
      if (promptPresentation.renderDocument) {
        state.skipNextCaretReveal = !promptPresentation.revealDocument;
        state.promptScrollOverride = false;
      }
      frameRenderPlan = {
        ...(promptPresentation.renderDocument
          ? surfaceRenderPlan(SURFACE.DOCUMENT)
          : surfaceRenderPlan()),
        reconcile: true,
      };
    } else {
      frameRenderPlan = surfaceRenderPlan(SURFACE.DOCUMENT);
    }
  } else {
    // Additive server messages are safe to ignore. Required incompatible
    // semantics must use a new wire version, which decodeMessage rejects.
    return true;
  }
  return true;
}

let frameRenderPlan = fullRenderPlan();

function connect() {
  const generation = ++socketGeneration;
  const socket = new WebSocket('ws://' + location.host + '/session');
  ws = socket;
  socket.binaryType = 'arraybuffer';
  socket.onopen = () => {
    if (!isCurrentGeneration(socketGeneration, generation)) return;
    statusEl.textContent = 'attached; awaiting state';
    socket.send(replayAttachFrame(!!state.sections, state.revision));
  };
  socket.onmessage = (e) => {
    if (!isCurrentGeneration(socketGeneration, generation)) return;
    if (typeof e.data === 'string') {
      statusEl.textContent = e.data;
      return;
    }
  try {
    if (!applyProtocolFrame(e.data)) return;
    if (!state.sections) { statusEl.textContent = 'no sections yet'; return; }

    render(frameRenderPlan);
    flushPointerPoint();
    statusEl.textContent = '';
    // Keep the document focused for the common editor case, but never steal focus
    // from an open footer prompt: it owns the keyboard while it is up, and
    // renderFooterPrompt has already placed focus on its container.
    if (!footerPromptOpen &&
        (document.activeElement === document.body || document.activeElement === statusEl)) {
      editorFocusElement().focus({ preventScroll: true });
    }
    } catch (err) {
      reconnect('protocol error: ' + err.message);
    }
  };
  socket.onclose = () => {
    if (!isCurrentGeneration(socketGeneration, generation)) return;
    discardPredictions();
    commandRequests = [];
    cancelPointerSelection();
    if (reconnectAttempts >= 8) {
      statusEl.textContent = 'connection unavailable';
      return;
    }
    const delay = reconnectDelay(reconnectAttempts++);
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(connect, delay);
  };
  socket.onerror = () => {
    if (isCurrentGeneration(socketGeneration, generation)) {
      statusEl.textContent = 'ws error';
    }
  };
}

connect();
// Device input is a client boundary, not a property of whichever retained
// surface last held focus. Browser accelerators may consume an Alt keydown after
// delivering its keyup, so the adapter falls back to that keyup only when no
// matching keydown reached the page.
const browserKeys = new BrowserKeyDispatchTracker();
window.addEventListener('keydown', (ev) => {
  if (ev.code === 'AltLeft' || ev.code === 'AltRight') {
    ev.preventDefault();
  }
  if (browserKeys.keydown(ev.code)) handleKeydown(ev);
}, { capture: true });
window.addEventListener('keyup', (ev) => {
  if (ev.code === 'AltLeft' || ev.code === 'AltRight') {
    ev.preventDefault();
  }
  if (browserKeys.keyup(ev.code, ev.altKey)) handleKeydown(ev);
}, { capture: true });
window.addEventListener('blur', () => browserKeys.clear());

function handleKeydown(ev) {
  if (ev.key === 'Escape' && pointerSelection.dragging) {
    ev.preventDefault();
    cancelPointerGesture();
  }

  // Ctrl/Meta chords belong to the browser: ssg's keymap uses Alt as its chord
  // modifier, so the web client never claims a Ctrl/Meta combo. Letting them
  // through keeps native zoom, copy/paste, and find working -- the browser is a
  // first-class client that may add its own affordances. (The one library action
  // reachable only via Ctrl+Shift+Home/End, select-to-document-extreme, has no
  // Alt twin and is thus unreachable on web until the keymap grows one.)
  if (ev.ctrlKey || ev.metaKey) return;

  const inputStroke = {
    code: ev.code, control: ev.ctrlKey, alt: ev.altKey,
    meta: ev.metaKey, shift: ev.shiftKey,
  };
  const promptActive = state.sections &&
    state.sections.prompt_status &&
    state.sections.prompt_status.active_kind != null;
  if (!paletteOpen() && !promptActive && state.sections) {
    const focus = externalFocusHeld(state.sections)
      ? 'external'
      : ['editor', 'panel', 'prompt', 'external'][num(state.sections.focus)];
    const mode = resolvePickerLifecycle(
      state.sections.keymap, state.sections.palette, inputStroke, focus);
    if (mode != null) {
      ev.preventDefault();
      state.palette.returnFocus = document.activeElement;
      state.palette.mode = mode;
      state.palette.query = '';
      state.palette.selected = 0;
      render({
        ...surfaceRenderPlan(SURFACE.FINDRESULTS),
        reconcile: true,
      });
      editorFocusElement().focus({ preventScroll: true });
      return;
    }
  }

  // When a picker is open, the browser owns its query and selection (a
  // client-owned derived view). Query edits and selection moves re-request a
  // local ranking; Enter submits the selected candidate id; Escape closes via the
  // library keymap (prompt.cancel). Nothing here touches the document.
  if (paletteOpen()) {
    const p = state.palette;
    if (ev.key === 'Escape') {
      ev.preventDefault();
      const returnFocus = p.returnFocus;
      p.mode = null;
      p.query = '';
      p.selected = 0;
      p.returnFocus = null;
      render({ ...surfaceRenderPlan(), reconcile: true });
      if (returnFocus && returnFocus.isConnected) {
        returnFocus.focus({ preventScroll: true });
      } else {
        editorFocusElement().focus({ preventScroll: true });
      }
      return;
    }
    if (ev.key === 'Enter') {
      ev.preventDefault();
      p.selected = clampPaletteSelection(p.selected, locallyRankedPaletteRows(state.sections && state.sections.palette).length);
      const rows = locallyRankedPaletteRows(state.sections && state.sections.palette);
      const candidate = rows[p.selected];
      submitPaletteCandidate(p.mode, candidate);
      return;
    }
    if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
      ev.preventDefault();
      // selected is an absolute ranked index over the locally-ranked rows.
      const rows = locallyRankedPaletteRows(state.sections && state.sections.palette);
      p.selected = clampPaletteSelection(ev.key === 'ArrowDown' ? p.selected + 1 : p.selected - 1, rows.length);
      render(surfaceRenderPlan(SURFACE.FINDRESULTS));
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
    // Unhandled keys remain browser input while the local picker owns focus.
    ev.preventDefault();
    return;
  }

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
  let predictionId = null;
  const priorPromptPrediction = state.promptPrediction;
  let promptPrediction = null;
  const focus = state.sections ? num(state.sections.focus) : -1;
  // Suppress local echo when the external-modification bar is the effective focus:
  // the wire `focus` field never carries ExternalModification (it is legacy-
  // projected), so the additive external_focus_held bool is the only signal that
  // the keystroke drives the bar's selection/actions, not a document insert.
  if (printable && !ev.altKey && focus === FOCUS_EDITOR &&
      !externalFocusHeld(state.sections)) {
    const id = state.nextEditId++;
    state.pending.push({ id, text });
    predictionId = id;
  }
  if (promptActive && renderedFooterPromptActiveId != null) {
    const activeElement = document.getElementById(
      'prompt-input-' + renderedFooterPromptActiveId);
    const currentValue =
      state.promptPrediction &&
      state.promptPrediction.controlId === renderedFooterPromptActiveId
        ? state.promptPrediction.value
        : (activeElement ? activeElement.textContent : '');
    const predictedValue =
      predictPromptValue(currentValue, ev.key, ev.altKey);
    if (predictedValue != null) {
      promptPrediction = {
        controlId: renderedFooterPromptActiveId,
        value: predictedValue,
      };
    }
  }
  const sent = sendTyped(encodeClientInput({
    code: ev.code, alt: ev.altKey, shift: ev.shiftKey, text,
  }));
  if (!sent) {
    if (predictionId != null) {
      state.pending = state.pending.filter((item) => item.id !== predictionId);
    }
    state.promptPrediction = priorPromptPrediction;
    return;
  }
  state.inputQueue.push({
    predictionId,
    promptPrediction: promptPrediction != null,
    promptInput: promptActive,
  });
  if (predictionId != null) {
    invalidatePointerOffsets();
    render(surfaceRenderPlan(SURFACE.DOCUMENT));
  }
  if (promptPrediction != null) {
    if (state.promptPrediction == null) state.promptScrollOverride = false;
    state.promptPrediction = promptPrediction;
    render({ ...surfaceRenderPlan(), reconcile: true });
  }
}
