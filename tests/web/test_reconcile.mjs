// Reconciliation oracle (node): exercises the REAL reconcile.mjs the browser
// runs, so the local-echo contract is pinned against the shipped client code,
// not a copy. Run by ctest via node when node is available.
//
// Contract pinned: the client shows the authoritative document plus its own
// not-yet-completed predictions. Typed input results complete in FIFO order only
// after their result revision is visible; authoritative state always wins.

import assert from 'node:assert/strict';
import {
  applyDocumentDelta, project, byteToIndex, utf8Bytes, settleInput,
  decodeMessage, browserInboundKind, encodeClientInput, encodeCommandRequest,
  settleCommandResult,
  matcherParametersFromWire, matcherBoundsFromPalette,
  clampPaletteSelection, pickerCandidatesFromPalette, resolvePickerLifecycle,
  PICKER_MODE, encodePickerSubmit, encodeSelectionByteRange, encodeTabAction,
  markedTextByteOffset, encodeTreeActivation, applyTreeDelta,
  applySessionDeltaSections,
  encodeStatusActionInvocation, encodePromptFocus,
  externalModificationFromSections, externalFocusHeld, encodeExternalAction,
  applyExternalModificationDelta, isCurrentGeneration, replayAttachFrame,
  deltaIsContiguous, clearUncertainInputs, reconnectDelay,
} from '../../apps/web/reconcile.mjs';

let checks = 0;
const check = (name, fn) => { fn(); checks++; };

// --- byte <-> UTF-16 mapping, including multi-byte and astral text and EOF ---
check('byteToIndex maps ASCII, 2-byte, and astral boundaries and EOF', () => {
  const text = 'a\u00e9\u{1f600}b';           // a(1) é(2) 😀(4) b(1) = 8 bytes
  const total = utf8Bytes(text);
  assert.equal(total, 8);
  const m = byteToIndex(text, [0, 1, 3, 7, 8]);
  assert.equal(m.get(0), 0);   // start
  assert.equal(m.get(1), 1);   // after 'a'
  assert.equal(m.get(3), 2);   // after 'é'
  assert.equal(m.get(7), 4);   // after astral (surrogate pair => 2 code units)
  assert.equal(m.get(8), 5);   // EOF
});

// --- document delta splice on multi-byte text ---
check('applyDocumentDelta inserts and replaces on byte offsets', () => {
  assert.equal(applyDocumentDelta('', { start: 0, erased_bytes: 0, inserted_text: 'hi' }), 'hi');
  // Replace 'é' (2 bytes at offset 1) with 'X'.
  assert.equal(applyDocumentDelta('a\u00e9b', { start: 1, erased_bytes: 2, inserted_text: 'X' }), 'aXb');
  // Delete the astral char (4 bytes at offset 0).
  assert.equal(applyDocumentDelta('\u{1f600}z', { start: 0, erased_bytes: 4, inserted_text: '' }), 'z');
});

check('settleInput is FIFO and waits for the result revision', () => {
  const queue = [{ predictionId: 1 }, { predictionId: 2 }];
  const pending = [{ id: 1, text: 'a' }, { id: 2, text: 'b' }];
  assert.equal(settleInput(queue, pending, { command: { revision: 4n } }, 3n), null);
  const first = settleInput(queue, pending, { command: { revision: 4n } }, 4n);
  assert.deepEqual(first.inputQueue, [{ predictionId: 2 }]);
  assert.deepEqual(first.pending, [{ id: 2, text: 'b' }]);
});

check('project splices predictions at the caret and moves the caret past them', () => {
  const r = project('ab', 1, [{ id: 1, text: 'X' }, { id: 2, text: 'Y' }]);
  assert.equal(r.text, 'aXYb');
  assert.equal(r.caret, 3);       // 1 + 2 predicted bytes
  assert.equal(r.predStart, 1);
  assert.equal(r.predEnd, 3);
});

// --- The end-to-end reconciliation property over local keystrokes, authoritative
// deltas, and ordered positive input completion. ---
check('typed text remains shown while FIFO results re-base predictions', () => {
  const typed = ['h', 'e', '\u{1f600}', 'y'];
  let pending = [];
  let inputQueue = [];
  let nextId = 1;
  let authText = '';
  let authCaret = 0;
  let revision = 0n;
  let typedSoFar = '';
  const shown = () => project(authText, authCaret, pending).text;
  const typeKey = (ch) => {
    const id = nextId++;
    pending.push({ id, text: ch });
    inputQueue.push({ predictionId: id, text: ch });
    typedSoFar += ch;
    assert.equal(shown(), typedSoFar);
  };
  for (const ch of typed) typeKey(ch);
  for (const ch of typed) {
    revision++;
    authText = applyDocumentDelta(authText, {
      start: authCaret, erased_bytes: 0, inserted_text: ch,
    });
    authCaret += utf8Bytes(ch);
    const settled = settleInput(
      inputQueue, pending, { command: { revision } }, revision);
    inputQueue = settled.inputQueue;
    pending = settled.pending;
    assert.equal(shown(), typedSoFar);
  }
  assert.equal(authText, 'he\u{1f600}y');
  assert.equal(pending.length, 0);
});

check('a rejected input completion removes its prediction', () => {
  const settled = settleInput(
    [{ predictionId: 1 }], [{ id: 1, text: 'x' }],
    { command: { revision: 2n } }, 2n);
  assert.deepEqual(settled.pending, []);
  assert.equal(project('', 0, settled.pending).text, '');
});

check('typed raw input and command requests round-trip through ProtocolValue', () => {
  assert.deepEqual(decodeMessage(encodeClientInput({
    code: 'KeyA', alt: true, shift: false, text: 'a',
  }).buffer), {
    kind: 7,
    payload: {
      stroke: { code: 'KeyA', control: false, alt: true, meta: false, shift: false },
      committed_text: 'a',
    },
  });

  check('browser protocol decoding rejects malformed and over-bound values', () => {
    assert.throws(() => decodeMessage(
      new Uint8Array([1, 9, 0]).buffer), /unsupported protocol frame/);
    assert.throws(() => decodeMessage(
      new Uint8Array([2, 1, 1, 2]).buffer), /malformed protocol bool/);
    assert.throws(() => decodeMessage(
      new Uint8Array([2, 1, 4, 4, 0, 0, 0, 65]).buffer), /truncated/);
    assert.throws(() => decodeMessage(
      new Uint8Array([2, 1, 6, 1, 0, 1, 0]).buffer), /collection length/);
  });

  check('browser ignores additive message kinds without weakening wire versions', () => {
    const decoded = decodeMessage(new Uint8Array([2, 9, 0]).buffer);
    assert.equal(browserInboundKind(decoded.kind), 'ignore');
    assert.equal(browserInboundKind(1), 'snapshot');
    assert.equal(browserInboundKind(8), 'input-result');
  });

  check('reconnect logic is generation-safe, resumes revision, and clears uncertainty', () => {
    assert.equal(isCurrentGeneration(4, 3), false);
    assert.equal(isCurrentGeneration(4, 4), true);
    assert.equal(replayAttachFrame(false, 9n), 'SSG1 ATTACH -');
    assert.equal(replayAttachFrame(true, 9n), 'SSG1 ATTACH 9');
    assert.equal(deltaIsContiguous(9n, { base_revision: 9n }), true);
    assert.equal(deltaIsContiguous(9n, { base_revision: 8n }), false);
    assert.deepEqual(clearUncertainInputs(), { pending: [], inputQueue: [] });
    assert.ok(reconnectDelay(20) < Infinity);
    assert.equal(reconnectDelay(20), reconnectDelay(21));
  });
  assert.deepEqual(decodeMessage(
    encodeCommandRequest('buffer.undo', 9n).buffer), {
    kind: 0,
    payload: { id: 'buffer.undo', base_revision: 9n, payload: null },
  });
});

check('encodeStatusActionInvocation emits the exact StatusActionInvocation wire frame', () => {
  const actual = encodeStatusActionInvocation({ statusId: 7, actionId: 'dismiss', generation: 3 });
  const expected = new Uint8Array([
    2, 5,
    7, 3, 0, 0, 0,
    9, 0, 0, 0, 115, 116, 97, 116, 117, 115, 95, 105, 100,
    3, 7, 0, 0, 0, 0, 0, 0, 0,
    9, 0, 0, 0, 97, 99, 116, 105, 111, 110, 95, 105, 100,
    4, 7, 0, 0, 0, 100, 105, 115, 109, 105, 115, 115,
    10, 0, 0, 0, 103, 101, 110, 101, 114, 97, 116, 105, 111, 110,
    3, 3, 0, 0, 0, 0, 0, 0, 0,
  ]);
  assert.deepEqual(actual, expected);
});

check('command results settle direct command owners in send order', () => {
  const first = settleCommandResult(['other', 'pointer']);
  assert.deepEqual(first, { owner: 'other', queue: ['pointer'] });
  assert.deepEqual(settleCommandResult(first.queue),
                   { owner: 'pointer', queue: [] });
  assert.equal(settleCommandResult([]), null);
});

check('matcherParametersFromWire maps decoded snake_case matcher fields', () => {
  assert.deepEqual(matcherParametersFromWire({
    base_score: 1n, word_boundary_bonus: 2n, contiguity_bonus: 3n,
    exact_case_bonus: 4n, length_cap: 5n,
  }), {
    baseScore: 1, wordBoundaryBonus: 2, contiguityBonus: 3,
    exactCaseBonus: 4, lengthCap: 5,
  });
});

check('matcherBoundsFromPalette requires published wire bounds', () => {
  assert.deepEqual(matcherBoundsFromPalette({
    parameters: {
      base_score: 1n, word_boundary_bonus: 2n, contiguity_bonus: 3n,
      exact_case_bonus: 4n, length_cap: 5n,
    },
    max_parameter_magnitude: 10n,
    max_candidate_bytes: 20n,
  }), {
    params: {
      baseScore: 1, wordBoundaryBonus: 2, contiguityBonus: 3,
      exactCaseBonus: 4, lengthCap: 5,
    },
    maxMagnitude: 10,
    maxCandidateBytes: 20,
  });
  assert.throws(() => matcherBoundsFromPalette({ parameters: {} }), /max_parameter_magnitude/);
  assert.throws(() => matcherBoundsFromPalette({
    parameters: { base_score: 1, word_boundary_bonus: 1, contiguity_bonus: 1, exact_case_bonus: 1, length_cap: 1 },
    max_parameter_magnitude: 10,
  }), /max_candidate_bytes/);
});

check('clampPaletteSelection keeps the persisted selection inside local rows', () => {
  assert.equal(clampPaletteSelection(7, 3), 2);
  assert.equal(clampPaletteSelection(-1, 3), 0);
  assert.equal(clampPaletteSelection(2, 0), 0);
});

check('compound interaction commands carry published identities and revision', () => {
  assert.deepEqual(
    decodeMessage(
      encodePickerSubmit(PICKER_MODE.COMMAND, 'command.open', 5n).buffer).payload,
    { id: 'picker.submit', base_revision: 5n,
      payload: { mode: 4n, candidate_id: 'command.open' } });
  assert.deepEqual(
    decodeMessage(encodeSelectionByteRange(2, 7, 6n).buffer).payload,
    { id: 'select.set_byte_range', base_revision: 6n,
      payload: { anchor_byte_offset: 2n, active_byte_offset: 7n } });
  assert.deepEqual(decodeMessage(encodeTreeActivation('tree:src', 6n).buffer).payload,
    { id: 'tree.activate_node', base_revision: 6n,
      payload: { node_id: 'tree:src' } });
  assert.deepEqual(
    decodeMessage(encodeTabAction('tab.activate', 17n, 6n).buffer).payload,
    { id: 'tab.activate', base_revision: 6n, payload: 17n });
  assert.deepEqual(
    decodeMessage(encodeTabAction('tab.close', 17n, 6n).buffer).payload,
    { id: 'tab.close', base_revision: 6n, payload: 17n });
  assert.throws(
    () => encodeTabAction('tab.close_all', 17n, 6n),
    /unsupported tab action/);
});

check('marked text offsets convert UTF-16 positions to authoritative UTF-8 bytes', () => {
  assert.equal(markedTextByteOffset(10, 'a\u00e9\u{1f642}z', 0), 10);
  assert.equal(markedTextByteOffset(10, 'a\u00e9\u{1f642}z', 2), 13);
  assert.equal(markedTextByteOffset(10, 'a\u00e9\u{1f642}z', 4), 17);
  assert.throws(
    () => markedTextByteOffset(10, 'a\u{1f642}z', 2),
    /surrogate pair/);
});

check('applyTreeDelta splices the retained tree and resyncs only when inexpressible', () => {
  const tree = () => ({ revision: 1, providers: [
    { provider_id: 'fs', kind: 0, nodes: [
      { node: { id: 'a' }, depth: 0 }, { node: { id: 'x' }, depth: 0 }], selected: 'a' }] });
  // A splice erases node x and inserts b, advancing the retained revision.
  let t = tree();
  assert.equal(applyTreeDelta(t, { base_revision: 1, revision: 2, snapshot_required: false,
    provider_order: ['fs'],
    providers: [{ provider_id: 'fs', kind: 0, start: 1, erase_count: 1,
      insert: [{ node: { id: 'b' }, depth: 0 }], selected: 'b' }] }), true);
  assert.equal(t.revision, 2);
  assert.deepEqual(t.providers[0].nodes.map((r) => r.node.id), ['a', 'b']);
  assert.equal(t.providers[0].selected, 'b');
  // A no-op delta (revision equals base) applies cleanly and stays current.
  t = tree();
  assert.equal(applyTreeDelta(t, { base_revision: 1, revision: 1, snapshot_required: false,
    provider_order: ['fs'], providers: [] }), true);
  assert.equal(t.revision, 1);
  // A base revision that does not match the retained tree means a missed delta:
  // the client cannot splice and must resync from a snapshot.
  assert.equal(applyTreeDelta(tree(), { base_revision: 9, revision: 10, snapshot_required: false, providers: [] }), false);
  // An explicit snapshot demand resyncs.
  assert.equal(applyTreeDelta(tree(), { base_revision: 1, revision: 2, snapshot_required: true, providers: [] }), false);
  // A malformed splice (erase past the end) resyncs rather than corrupt the tree.
  assert.equal(applyTreeDelta(tree(), { base_revision: 1, revision: 2, snapshot_required: false,
    providers: [{ provider_id: 'fs', kind: 0, start: 0, erase_count: 9, insert: [], selected: null }] }), false);
  // A brand-new provider inserts at its sorted position, carrying only inserts.
  t = tree();
  assert.equal(applyTreeDelta(t, { base_revision: 1, revision: 2, snapshot_required: false,
    provider_order: ['git', 'fs'],
    providers: [{ provider_id: 'git', kind: 1, start: 0, erase_count: 0,
      insert: [{ node: { id: 'g' }, depth: 0 }], selected: null }] }), true);
  assert.deepEqual(t.providers.map((p) => p.provider_id), ['git', 'fs']);
  // No tree object at all: nothing to do, stays current.
  assert.equal(applyTreeDelta(tree(), undefined), true);
  // Two changes for one provider are malformed (matches C++ replay's reject).
  assert.equal(applyTreeDelta(tree(), { base_revision: 1, revision: 2, snapshot_required: false,
    providers: [
      { provider_id: 'fs', kind: 0, start: 0, erase_count: 0, insert: [], selected: 'a' },
      { provider_id: 'fs', kind: 0, start: 0, erase_count: 0, insert: [], selected: 'a' }] }), false);
  // A removal carrying a nonzero splice payload is malformed.
  assert.equal(applyTreeDelta(tree(), { base_revision: 1, revision: 2, snapshot_required: false,
    providers: [{ provider_id: 'fs', kind: 0, remove_provider: true, start: 1, erase_count: 0, insert: [], selected: null }] }), false);
  // Transactional: a later malformed change leaves the retained tree UNCHANGED.
  t = tree();
  assert.equal(applyTreeDelta(t, { base_revision: 1, revision: 2, snapshot_required: false,
    providers: [
      { provider_id: 'git', kind: 1, start: 0, erase_count: 0, insert: [{ node: { id: 'g' }, depth: 0 }], selected: null },
      { provider_id: 'fs', kind: 0, start: 0, erase_count: 9, insert: [], selected: null }] }), false);
  assert.equal(t.revision, 1);
  assert.deepEqual(t.providers.map((p) => p.provider_id), ['fs']);
  assert.deepEqual(t.providers[0].nodes.map((r) => r.node.id), ['a', 'x']);
});

// --- UI-VM: profile rejection + schema/state interpretation ---
import {
  firstUnsupportedPrimitive, interpretChrome, WEB_UI_PROFILE, WIDGET, SIZE, SURFACE, SCROLL,
  webExtentCss,
  GenerationRetainedCache, gitAffordanceFromNode,
  preferredKeyboardSurface, browserRenderPlan, settlePointerSelection,
} from '../../apps/web/reconcile.mjs';

// A leaf node on the wire: { id, size, leaf: { kind, ..., role?, width? } }.
const leafNode = (id, kind, extra) => ({ id, size: {}, leaf: { kind, ...(extra || {}) } });
// A Row container node over `children`.
const rowNode = (id, children) => ({ id, size: {}, container: { axis: 0, gap: 0, children } });
// A schema is a single root node; placement is tree structure + well-known ids.
const schemaOf = (generation, root) => ({ generation, root });
// The dynamic-state record for a node (presence is a separate section now).
const st = (id, leaf) => ({ id, leaf: leaf || null });
// The presence section corresponding to a schema: one record per node in the tree,
// all present except ids in `hidden`. Built from the schema so it always
// corresponds; the tests that check state mismatch fail on state, not presence.
const presenceForSchema = (generation, root, hidden = []) => {
  const nodes = [];
  const walk = (node) => {
    if (!node) return;
    nodes.push({ id: node.id, present: hidden.includes(node.id) ? 0 : 1 });
    if (node.container && Array.isArray(node.container.children))
      for (const c of node.container.children) walk(c);
  };
  walk(root);
  return { generation, basis: 0, nodes };
};

// Flatten a render tree to its drawn leaves in order (the DOM builder mirrors this).
function drawnLeaves(node, out = []) {
  if (!node) return out;
  if (node.kind === 'container') { for (const c of node.children) drawnLeaves(c, out); }
  else out.push(node);
  return out;
}

check('firstUnsupportedPrimitive accepts a supported header/footer schema', () => {
  const root = { id: 'root', size: {}, container: { axis: 1, gap: 0, children: [
    rowNode('header', [leafNode('a', WIDGET.FIELD), leafNode('b', WIDGET.SPACER)]),
    rowNode('footer', [leafNode('c', WIDGET.CHECKBOX)]),
  ] } };
  assert.equal(firstUnsupportedPrimitive(schemaOf(1, root)), null);
});

check('firstUnsupportedPrimitive accepts the prompt TextInput by default and rejects it when narrowed', () => {
  const root = rowNode('root', [leafNode('input_line', WIDGET.TEXT_INPUT)]);
  assert.equal(firstUnsupportedPrimitive(schemaOf(1, root)), null);
  const profile = { ...WEB_UI_PROFILE, widgets: new Set([...WEB_UI_PROFILE.widgets].filter((w) => w !== WIDGET.TEXT_INPUT)) };
  assert.deepEqual(firstUnsupportedPrimitive(schemaOf(1, root), profile), { kind: 'widget', ordinal: WIDGET.TEXT_INPUT });
});

check('firstUnsupportedPrimitive rejects a View naming a surface unsupported by a narrowed profile', () => {
  const profile = { ...WEB_UI_PROFILE, surfaces: new Set() };
  const root = rowNode('root', [leafNode('v', WIDGET.VIEW, { surface: SURFACE.GITSTATUS })]);
  assert.deepEqual(firstUnsupportedPrimitive(schemaOf(1, root), profile), { kind: 'surface', ordinal: SURFACE.GITSTATUS });
});

check('firstUnsupportedPrimitive accepts a View whose surface the default profile declares', () => {
  const root = rowNode('root', [leafNode('v', WIDGET.VIEW, { surface: SURFACE.GITSTATUS })]);
  assert.equal(firstUnsupportedPrimitive(schemaOf(1, root)), null);
});

check('firstUnsupportedPrimitive accepts Symbols under the default profile', () => {
  const root = rowNode('root', [leafNode('symbols', WIDGET.VIEW, { surface: SURFACE.SYMBOLS })]);
  assert.equal(firstUnsupportedPrimitive(schemaOf(1, root)), null);
});

check('firstUnsupportedPrimitive accepts StatusActions by default and rejects it when narrowed', () => {
  const root = rowNode('root', [leafNode('actions', WIDGET.STATUS_ACTIONS)]);
  assert.equal(firstUnsupportedPrimitive(schemaOf(1, root)), null);
  const profile = { ...WEB_UI_PROFILE, widgets: new Set([...WEB_UI_PROFILE.widgets].filter((w) => w !== WIDGET.STATUS_ACTIONS)) };
  assert.deepEqual(firstUnsupportedPrimitive(schemaOf(1, root), profile), {
    kind: 'widget', ordinal: WIDGET.STATUS_ACTIONS,
  });
});

check('interpretChrome carries a node ScrollAxis so a client derives independent scroll, and degrades an unknown axis to none', () => {
  const scrollContainer = (id, scroll, children) =>
    ({ id, size: {}, container: { axis: 1, gap: 0, scroll, children } });
  const document = leafNode(
    'document', WIDGET.VIEW, { surface: SURFACE.DOCUMENT });
  const editor = scrollContainer('editor', SCROLL.NONE, [
    leafNode('tabbar', WIDGET.VIEW, { surface: SURFACE.TABBAR }),
    scrollContainer('document.viewport', SCROLL.VERTICAL, [document]),
  ]);
  const root = { id: 'root', size: {}, container: { axis: 1, gap: 0, children: [
    rowNode('body', [
      scrollContainer('panel', SCROLL.VERTICAL, [leafNode('filetree', WIDGET.VIEW, { surface: SURFACE.FILETREE })]),
      scrollContainer('content', SCROLL.NONE, [editor]),
    ]),
    scrollContainer('future', 99, [leafNode('x', WIDGET.VIEW, { surface: SURFACE.NOTICE })]),
  ] } };
  const nodes = [];
  const collect = (n) => { nodes.push(st(n.id)); if (n.container) n.container.children.forEach(collect); };
  collect(root);
  const out = interpretChrome(schemaOf(20, root), { generation: 20, nodes }, presenceForSchema(20, root));
  assert.ok(out && out.root);
  const byId = {};
  const walk = (n) => { byId[n.id] = n; if (n.kind === 'container') n.children.forEach(walk); };
  walk(out.root);
  assert.equal(byId.panel.scroll, SCROLL.VERTICAL);
  assert.equal(byId.content.scroll, SCROLL.NONE);
  assert.equal(byId.editor.scroll, SCROLL.NONE);
  assert.equal(byId['document.viewport'].scroll, SCROLL.VERTICAL);
  assert.deepEqual(
    byId['document.viewport'].children.map((child) => child.id),
    ['document']);
  assert.equal(byId.filetree.scroll ?? SCROLL.NONE, SCROLL.NONE);
  assert.equal(byId.document.scroll ?? SCROLL.NONE, SCROLL.NONE);
  assert.equal(byId.body.scroll, SCROLL.NONE);
  assert.equal(byId.future.scroll, SCROLL.NONE);  // unknown axis -> none
});

check('interpretChrome rejects a container whose scroll field is null or the wrong type', () => {
  // Symmetry with the C++ wire decoder: absence (undefined) and an unknown numeric
  // ordinal degrade to none, but a present null or non-numeric scroll is malformed
  // and rejects the frame.
  for (const bad of ['vertical', null]) {
    const root = { id: 'root', size: {}, container: { axis: 1, gap: 0, scroll: bad, children: [
      leafNode('a', WIDGET.VIEW, { surface: SURFACE.DOCUMENT }),
    ] } };
    const nodes = [st('root'), st('a')];
    assert.equal(interpretChrome(schemaOf(21, root), { generation: 21, nodes }, presenceForSchema(21, root)), null);
  }
});

check('interpretChrome applies the per-kind render gate', () => {
  const root = rowNode('root', [
    leafNode('lit', WIDGET.FIELD),      // present with leaf -> drawn
    leafNode('empty', WIDGET.LABEL),    // present, nullopt leaf -> NOT drawn (resolved drop)
    leafNode('sp', WIDGET.SPACER),      // present spacer -> drawn as a gap despite no leaf
    leafNode('box', WIDGET.CHECKBOX),   // checkbox -> drawn
  ]);
  const state = { generation: 4, nodes: [
    st('root'), // the container node
    st('lit', { value: 'hello', label: 'hello', command: 'open.thing' }),
    st('empty', null),
    st('sp', null),
    st('box', { value: 'case', label: 'case', checked: 1 }),
  ] };
  const out = interpretChrome(schemaOf(4, root), state, presenceForSchema(4, root));
  assert.ok(out);
  // The render tree preserves the container; flatten to drawn leaves.
  assert.equal(out.root.kind, 'container');
  const items = drawnLeaves(out.root);
  // lit (drawn), sp (gap), box (checkbox) -- empty Label is dropped.
  assert.deepEqual(items.map((i) => i.id), ['lit', 'sp', 'box']);
  assert.equal(items[0].text, 'hello');
  assert.equal(items[0].command, 'open.thing');
  assert.equal(items[1].spacer, true);
  assert.equal(items[2].checked, true);
  assert.equal(items[2].text, 'case');
});

check('interpretChrome produces View leaves for every widened surface', () => {
  const surfaces = [
    SURFACE.TABBAR, SURFACE.FILETREE, SURFACE.GITSTATUS, SURFACE.FINDRESULTS,
    SURFACE.SYMBOLS, SURFACE.FOOTER_PROMPT, SURFACE.NOTICE,
    SURFACE.EXTERNAL_MODIFICATION, SURFACE.DOCUMENT,
  ];
  const root = rowNode('root', surfaces.map((surface) => leafNode('surface-' + surface, WIDGET.VIEW, { surface })));
  const state = { generation: 8, nodes: [st('root'), ...surfaces.map((surface) => st('surface-' + surface))] };
  const out = interpretChrome(schemaOf(8, root), state, presenceForSchema(8, root));
  assert.ok(out);
  const items = drawnLeaves(out.root);
  assert.deepEqual(items.map((i) => i.surface), surfaces);
  assert.deepEqual(items.map((i) => i.widget), surfaces.map(() => WIDGET.VIEW));
});

check('the complete interpreted root preserves every present child in published order', () => {
  const ids = ['header', 'notice', 'external', 'body', 'footer-prompt', 'footer'];
  const surfaces = [
    SURFACE.DOCUMENT, SURFACE.NOTICE, SURFACE.EXTERNAL_MODIFICATION,
    SURFACE.DOCUMENT, SURFACE.FOOTER_PROMPT, SURFACE.DOCUMENT,
  ];
  const root = rowNode(
    'root', ids.map((id, i) => leafNode(id, WIDGET.VIEW, { surface: surfaces[i] })));
  const states = { generation: 12, nodes: [st('root'), ...ids.map((id) => st(id))] };
  const out = interpretChrome(schemaOf(12, root), states, presenceForSchema(12, root));
  assert.deepEqual(out.root.children.map((child) => child.id), ids);
});

check('web extents use columns horizontally and row height vertically', () => {
  assert.equal(webExtentCss(3, 0), '3ch');
  assert.equal(webExtentCss(3, 1), 'calc(3 * var(--ssg-row))');
});

check('retained surfaces preserve identity within one generation only', () => {
  const cache = new GenerationRetainedCache();
  assert.equal(cache.begin(4), true);
  const first = cache.getOrCreate('prompt', () => ({ version: 1 }));
  const viewport =
    cache.getOrCreate('document.viewport', () => ({ scrollTop: 7 }));
  assert.equal(cache.get('document.viewport'), viewport);
  assert.equal(cache.getOrCreate('prompt', () => ({ version: 2 })), first);
  // Detaching a surface does not touch the cache; reattachment finds the same object.
  assert.equal(cache.getOrCreate('prompt', () => ({ version: 3 })), first);
  assert.equal(cache.begin(4), false);
  assert.equal(cache.begin(5), true);
  assert.notEqual(cache.getOrCreate('prompt', () => ({ version: 4 })), first);
});

check('session deltas dirty only their dependent browser surfaces', () => {
  assert.deepEqual(browserRenderPlan({
    selection: { changed: false, replacement: null },
    tabs: { state: null },
    tree: { base_revision: 4n, revision: 4n },
    palette: null,
    prompt_view: { changed: false, replacement: null },
    notice_view: { changed: false, replacement: null },
    external_modification: { base_revision: 5n, revision: 5n },
    theme: { replacement: null },
    ui: null,
    ui_state: null,
    ui_presence: null,
    prompt_status: { changed: false, replacement: null },
    syntax: { spans: null },
  }), {
    rebuild: false, reconcile: false, repaintTheme: false,
    surfaces: [], localPicker: false,
  });
  assert.deepEqual(browserRenderPlan({ selection: { replacement: {} } }), {
    rebuild: false, reconcile: false, repaintTheme: false,
    surfaces: [SURFACE.DOCUMENT], localPicker: false,
  });
  assert.deepEqual(browserRenderPlan({
    tree: { base_revision: 1n, revision: 2n, providers: [] },
  }), {
    rebuild: false, reconcile: false, repaintTheme: false,
    surfaces: [SURFACE.FILETREE, SURFACE.GITSTATUS, SURFACE.SYMBOLS],
    localPicker: false,
  });
  assert.deepEqual(browserRenderPlan({ ui_presence: { nodes: [] } }), {
    rebuild: false, reconcile: true, repaintTheme: false,
    surfaces: [], localPicker: false,
  });
  assert.deepEqual(browserRenderPlan({ palette: {} }), {
    rebuild: false, reconcile: false, repaintTheme: false,
    surfaces: [SURFACE.FINDRESULTS], localPicker: true,
  });
  assert.deepEqual(browserRenderPlan({ theme: { replacement: {} } }), {
    rebuild: false, reconcile: true, repaintTheme: true,
    surfaces: [
      SURFACE.TABBAR, SURFACE.FILETREE, SURFACE.GITSTATUS,
      SURFACE.FINDRESULTS, SURFACE.SYMBOLS, SURFACE.FOOTER_PROMPT,
      SURFACE.NOTICE, SURFACE.EXTERNAL_MODIFICATION, SURFACE.DOCUMENT,
    ],
    localPicker: true,
  });
});

check('pointer settlement coalesces, retries once, and rejects a stale basis', () => {
  const basis = { text: 'abc', tab: 'tab-a' };
  const first = { anchor: 0, active: 1, basis, retries: 0 };
  const newer = { anchor: 2, active: 3, basis, retries: 0 };
  assert.deepEqual(
    settlePointerSelection(first, newer, 0, basis),
    { dispatch: newer, preview: newer });
  assert.deepEqual(
    settlePointerSelection(first, null, 3, basis),
    {
      dispatch: { ...first, retries: 1 },
      preview: { ...first, retries: 1 },
    });
  assert.deepEqual(
    settlePointerSelection({ ...first, retries: 1 }, null, 3, basis),
    { dispatch: null, preview: null });
  assert.deepEqual(
    settlePointerSelection(first, null, 3, { text: 'abcd', tab: 'tab-a' }),
    { dispatch: null, preview: null });
});

check('the locally owned finder becomes keyboard owner while it replaces the document', () => {
  assert.equal(
    preferredKeyboardSurface([SURFACE.DOCUMENT]), SURFACE.DOCUMENT);
  assert.equal(
    preferredKeyboardSurface([SURFACE.DOCUMENT, SURFACE.FINDRESULTS]),
    SURFACE.FINDRESULTS);
  assert.equal(
    preferredKeyboardSurface([SURFACE.FINDRESULTS]), SURFACE.FINDRESULTS);
  assert.equal(preferredKeyboardSurface([SURFACE.FILETREE]), null);
});

check('git affordance projection preserves deliberately varied published facts', () => {
  assert.deepEqual(
    gitAffordanceFromNode({ git_status: { status: 1, short_label: 'changed!', role: 20 } }),
    { shortLabel: 'changed!', role: 20 });
  assert.equal(gitAffordanceFromNode({ git_status: null }), null);
});

check('interpretChrome produces a StatusActions leaf under the default profile', () => {
  const root = rowNode('root', [leafNode('actions', WIDGET.STATUS_ACTIONS)]);
  const state = { generation: 9, nodes: [st('root'), st('actions')] };
  const out = interpretChrome(schemaOf(9, root), state, presenceForSchema(9, root));
  assert.ok(out);
  const item = drawnLeaves(out.root)[0];
  assert.equal(item.id, 'actions');
  assert.equal(item.widget, WIDGET.STATUS_ACTIONS);
  assert.deepEqual(item.actions, []);
});

check('interpretChrome rejects View or StatusActions leaves with leaf state', () => {
  const root = rowNode('root', [
    leafNode('view', WIDGET.VIEW, { surface: SURFACE.DOCUMENT }),
    leafNode('actions', WIDGET.STATUS_ACTIONS),
  ]);
  const profile = {
    ...WEB_UI_PROFILE,
    widgets: new Set([...WEB_UI_PROFILE.widgets, WIDGET.STATUS_ACTIONS]),
    surfaces: new Set([SURFACE.DOCUMENT]),
  };
  assert.equal(interpretChrome(schemaOf(10, root), { generation: 10, nodes: [
    st('root'), st('view', { value: 'x' }), st('actions'),
  ] }, presenceForSchema(10, root), profile), null);
  assert.equal(interpretChrome(schemaOf(10, root), { generation: 10, nodes: [
    st('root'), st('view'), st('actions', { value: 'x' }),
  ] }, presenceForSchema(10, root), profile), null);
});

check('interpretChrome preserves the left/middle/right grouping and its sizing', () => {
  // Row[ left(Auto container), middle(Flex container w/ center), right(Auto container) ].
  const grp = (id, sizeKind, children) => ({ id, size: { kind: sizeKind }, container: { axis: 0, gap: 0, children } });
  const root = grp('root', SIZE.FLEX, [
    grp('left', SIZE.AUTO, [leafNode('l0', WIDGET.FIELD)]),
    grp('mid', SIZE.FLEX, [{ id: 'c0', size: { kind: SIZE.EXACT, extent: 12 }, leaf: { kind: WIDGET.LABEL } }]),
    grp('right', SIZE.AUTO, [leafNode('r0', WIDGET.FIELD, { command: 'do.it' })]),
  ]);
  const state = { generation: 2, nodes: [
    st('root'), st('left'), st('l0', { value: 'L', label: 'L', role: 11 }),
    st('mid'), st('c0', { value: 'C', label: 'C', role: 11 }),
    st('right'), st('r0', { value: 'R', label: 'R', command: 'do.it', role: 11 }),
  ] };
  const out = interpretChrome(schemaOf(2, root), state, presenceForSchema(2, root));
  assert.ok(out);
  const rootOut = out.root;
  assert.equal(rootOut.kind, 'container');
  assert.equal(rootOut.children.length, 3);
  assert.equal(rootOut.children[1].size.kind, SIZE.FLEX);   // the middle group is Flex-sized
  assert.equal(rootOut.children[0].size.kind, SIZE.AUTO);   // the left group is Auto
  // The middle's center leaf preserves its Exact extent (a fixed center width).
  const center = rootOut.children[1].children[0];
  assert.equal(center.text, 'C');
  assert.equal(center.size.kind, SIZE.EXACT);
  assert.equal(center.size.extent, 12);
  // The published role ordinal flows through to the leaf.
  assert.equal(center.role, 11);
});

check('interpretChrome returns null on a generation or node-id mismatch', () => {
  const schema = schemaOf(4, rowNode('root', [leafNode('a', WIDGET.FIELD)]));
  // Generation mismatch.
  assert.equal(interpretChrome(schema, { generation: 3, nodes: [st('root'), st('a', { value: 'x', label: 'x' })] }, presenceForSchema(4, schema.root)), null);
  // Node-id set mismatch (missing 'a').
  assert.equal(interpretChrome(schema, { generation: 4, nodes: [st('root')] }, presenceForSchema(4, schema.root)), null);
});

check('interpretChrome returns null on a state/schema SHAPE disagreement', () => {
  const schema = schemaOf(5, rowNode('root', [
    leafNode('box', WIDGET.CHECKBOX), leafNode('sp', WIDGET.SPACER),
  ]));
  // Checkbox missing its leaf state -> wait.
  assert.equal(interpretChrome(schema, { generation: 5, nodes: [
    st('root'), st('box', null), st('sp', null),
  ] }, presenceForSchema(5, schema.root)), null);
  // Spacer carrying leaf state -> wait.
  assert.equal(interpretChrome(schema, { generation: 5, nodes: [
    st('root'), st('box', { value: '', label: '', checked: 0 }), st('sp', { value: 'x', label: 'x' }),
  ] }, presenceForSchema(5, schema.root)), null);
  // Container carrying leaf state -> wait.
  assert.equal(interpretChrome(schema, { generation: 5, nodes: [
    st('root', { value: 'x', label: 'x' }), st('box', { value: '', label: '', checked: 0 }), st('sp', null),
  ] }, presenceForSchema(5, schema.root)), null);
  // Checkbox WITH leaf state but MISSING its `checked` -> wait.
  assert.equal(interpretChrome(schema, { generation: 5, nodes: [
    st('root'), st('box', { value: 'c', label: 'c' }), st('sp', null),
  ] }, presenceForSchema(5, schema.root)), null);
});

check('interpretChrome returns null when a Label/Field leaf state carries checked', () => {
  const schema = schemaOf(6, rowNode('root', [leafNode('f', WIDGET.FIELD)]));
  assert.equal(interpretChrome(schema, { generation: 6, nodes: [
    st('root'), st('f', { value: 'x', label: 'x', checked: 1 }),
  ] }, presenceForSchema(6, schema.root)), null);
});

check('interpretChrome Never draws a node the presence section marks absent', () => {
  const root = rowNode('root', [
    leafNode('a', WIDGET.FIELD),
    rowNode('grp', [leafNode('b', WIDGET.FIELD)]),
  ]);
  const state = { generation: 7, nodes: [
    st('root'), st('a', { value: 'A', label: 'A' }),
    st('grp'), st('b', { value: 'B', label: 'B' }),
  ] };
  // Hiding the container 'grp' drops it AND its child 'b'; 'a' still draws.
  const out = interpretChrome(schemaOf(7, root), state, presenceForSchema(7, root, ['grp']));
  assert.ok(out);
  assert.deepEqual(drawnLeaves(out.root).map((i) => i.id), ['a']);
  // Hiding just the leaf 'a' drops only it.
  const out2 = interpretChrome(schemaOf(7, root), state, presenceForSchema(7, root, ['a']));
  assert.deepEqual(drawnLeaves(out2.root).map((i) => i.id), ['b']);
});

check('interpretChrome renders the prompt TextInput as a bare anchor carrying no server query text', () => {
  const root = rowNode('root', [
    leafNode('active_command', WIDGET.FIELD),
    leafNode('input_line', WIDGET.TEXT_INPUT, { role: 'prompt', sigil: '> ' }),
  ]);
  // The input_line node carries NO leaf state -- the browser owns the query, so a
  // keystroke never produces a tree delta.
  const state = { generation: 11, nodes: [
    st('root'), st('active_command', { value: 'INSERT', label: 'INSERT' }), st('input_line', null),
  ] };
  const out = interpretChrome(schemaOf(11, root), state, presenceForSchema(11, root));
  assert.ok(out);
  const items = drawnLeaves(out.root);
  const anchor = items.find((i) => i.id === 'input_line');
  assert.ok(anchor, 'the anchor is drawn');
  assert.equal(anchor.widget, WIDGET.TEXT_INPUT);
  assert.equal(anchor.role, 16);
  assert.equal(anchor.sigil, '> ');
  assert.equal(anchor.text, undefined);  // no server-published query text
});

check('interpretChrome Never draws a prompt TextInput that carries server leaf state', () => {
  const root = rowNode('root', [leafNode('input_line', WIDGET.TEXT_INPUT)]);
  // A TextInput must NOT carry leaf state (the query is browser-owned) -- a frame
  // that publishes one is malformed and never partially drawn.
  const state = { generation: 12, nodes: [st('root'), st('input_line', { value: 'leaked' })] };
  assert.equal(interpretChrome(schemaOf(12, root), state, presenceForSchema(12, root)), null);
});

check('picker inventories are selected only by their published mode', () => {
  const palette = {
    command_candidates: [{ id: 'edit.undo' }],
    file_candidates: [{ id: 'src/main.cpp' }],
  };
  assert.equal(
    pickerCandidatesFromPalette(palette, PICKER_MODE.COMMAND)[0].id,
    'edit.undo');
  assert.equal(
    pickerCandidatesFromPalette(palette, PICKER_MODE.FILE)[0].id,
    'src/main.cpp');
  assert.throws(() => pickerCandidatesFromPalette(palette, 3), /unsupported/);
});

check('picker lifecycle resolves published keymap commands with global precedence', () => {
  const stroke = {
    code: 'KeyP', control: false, alt: true, meta: false, shift: false,
  };
  const palette = {
    command_open_command_id: 'commands.show',
    file_open_command_id: 'files.show',
  };
  const keymap = { bindings: [
    { sequence: [stroke], command_id: 'commands.show', context: 'editor' },
    { sequence: [stroke], command_id: 'files.show', context: '*' },
  ] };
  assert.equal(
    resolvePickerLifecycle(keymap, palette, stroke, 'editor'),
    PICKER_MODE.FILE);
  assert.equal(
    resolvePickerLifecycle(keymap, palette,
      { ...stroke, shift: true }, 'editor'),
    null);
});

// --- Footer prompt: semantic PromptView projection, delta, focus plan, ingress ---
import {
  promptViewFromSections, promptFocusPlan, PROMPT_CONTROL,
} from '../../apps/web/reconcile.mjs';

// A decoded semantic PromptView section, using the encoder's snake_case names.
const findSection = (activeInput = 0) => ({
  prompt_view: {
    kind: 1, accessible_label: 'Find', active_input: activeInput, controls: [
      { kind: PROMPT_CONTROL.INPUT, id: 'find.query', accessible_label: 'Find', value: 'ab', checked: false, command: 'find.update_query' },
      { kind: PROMPT_CONTROL.TOGGLE, id: 'find.case', accessible_label: 'Case', value: '', checked: true, command: 'find.toggle_case' },
      { kind: PROMPT_CONTROL.COUNT, id: 'find.count', accessible_label: 'Matches', value: '3', checked: false, command: '' },
    ],
  },
});

check('promptViewFromSections renders controls from the section, null when closed', () => {
  assert.equal(promptViewFromSections(null), null);
  assert.equal(promptViewFromSections({}), null);
  assert.equal(promptViewFromSections({ prompt_view: null }), null);
  const pv = promptViewFromSections(findSection(0));
  assert.equal(pv.kind, 1);
  assert.equal(pv.label, 'Find');
  assert.equal(pv.activeInput, 0);
  assert.deepEqual(pv.controls.map((c) => c.kind), [PROMPT_CONTROL.INPUT, PROMPT_CONTROL.TOGGLE, PROMPT_CONTROL.COUNT]);
  assert.equal(pv.controls[0].value, 'ab');
  assert.equal(pv.controls[0].command, 'find.update_query');   // per-control command, not hardcoded
  assert.equal(pv.controls[1].checked, true);
  assert.equal(pv.controls[2].value, '3');
});

check('applySessionDeltaSections opens, changes, and CLOSES the footer prompt view', () => {
  const sections = { document: { text: '' }, prompt_view: null };
  // changed=true with a replacement opens/updates it.
  applySessionDeltaSections(sections, { prompt_view: { changed: 1, replacement: findSection(0).prompt_view } });
  assert.ok(sections.prompt_view);
  assert.equal(promptViewFromSections(sections).activeInput, 0);
  // changed=false leaves the prior view intact (no spurious close).
  applySessionDeltaSections(sections, { prompt_view: { changed: 0 } });
  assert.ok(sections.prompt_view);
  // changed=true with a null replacement CLOSES it (replaceWrapped's non-null
  // guard would wrongly keep it -- this is why the delta is changed-flagged).
  applySessionDeltaSections(sections, { prompt_view: { changed: 1, replacement: null } });
  assert.equal(sections.prompt_view, null);
  assert.equal(promptViewFromSections(sections), null);
});

check('promptFocusPlan focuses only on open and active-input change, never per message', () => {
  const closed = { open: false, activeInput: -1 };
  const pv0 = promptViewFromSections(findSection(0));
  const pv1 = promptViewFromSections(findSection(1));
  // Open: capture and focus the container.
  const opened = promptFocusPlan(closed, pv0);
  assert.deepEqual(opened, { open: true, activeInput: 0, focusContainer: true, restoreFocus: false, activeDescendant: 0 });
  // Re-render at the same active input: NO focus move (survives re-render; an
  // unrelated server frame cannot steal deliberate external focus).
  assert.equal(promptFocusPlan({ open: true, activeInput: 0 }, pv0).focusContainer, false);
  // Active-input change: focus the container and point aria at the new input.
  const moved = promptFocusPlan({ open: true, activeInput: 0 }, pv1);
  assert.equal(moved.focusContainer, true);
  assert.equal(moved.activeDescendant, 1);
  // Close: restore the captured focus, drop the container.
  const closedPlan = promptFocusPlan({ open: true, activeInput: 1 }, null);
  assert.deepEqual(closedPlan, { open: false, activeInput: -1, focusContainer: false, restoreFocus: true, activeDescendant: null });
  // Already closed: nothing to restore.
  assert.equal(promptFocusPlan(closed, null).restoreFocus, false);
});

check('prompt focus uses a typed revision-checked compound command', () => {
  assert.deepEqual(decodeMessage(encodePromptFocus(2, 7n).buffer).payload,
    { id: 'prompt.focus_control', base_revision: 7n, payload: { index: 2n } });
});

// --- Draft-conflict notice: semantic NoticeView projection, delta, action ingress ---
import { noticeViewFromSections } from '../../apps/web/reconcile.mjs';

// A decoded semantic NoticeView section, using the encoder's snake_case names.
const noticeSection = () => ({
  notice_view: {
    text: 'Unsaved draft: file changed on disk externally.',
    actions: [
      { id: 'draft.notice.diff', label: 'diff', command: 'draft.diff' },
      { id: 'draft.notice.use_disk', label: 'use disk', command: 'draft.discard' },
      { id: 'draft.notice.dismiss', label: 'dismiss', command: 'draft.dismiss' },
    ],
  },
});

check('noticeViewFromSections renders the notice from the section, null when absent', () => {
  assert.equal(noticeViewFromSections(null), null);
  assert.equal(noticeViewFromSections({}), null);
  assert.equal(noticeViewFromSections({ notice_view: null }), null);
  const nv = noticeViewFromSections(noticeSection());
  assert.equal(nv.text, 'Unsaved draft: file changed on disk externally.');
  assert.deepEqual(nv.actions.map((a) => a.command), ['draft.diff', 'draft.discard', 'draft.dismiss']);
  assert.equal(nv.actions[0].label, 'diff');
});

check('applySessionDeltaSections raises, holds, and CLEARS the notice view', () => {
  const sections = { document: { text: '' }, notice_view: null };
  // changed=true with a replacement raises it.
  applySessionDeltaSections(sections, { notice_view: { changed: 1, replacement: noticeSection().notice_view } });
  assert.ok(sections.notice_view);
  assert.equal(noticeViewFromSections(sections).actions.length, 3);
  // changed=false leaves the prior notice intact (no spurious clear).
  applySessionDeltaSections(sections, { notice_view: { changed: 0 } });
  assert.ok(sections.notice_view);
  // changed=true with a null replacement CLEARS it (a null replacement means the
  // notice cleared, never "unchanged").
  applySessionDeltaSections(sections, { notice_view: { changed: 1, replacement: null } });
  assert.equal(sections.notice_view, null);
  assert.equal(noticeViewFromSections(sections), null);
});

check('a notice action carries the plain command id dispatched through the command ingress', () => {
  const nv = noticeViewFromSections(noticeSection());
  assert.deepEqual(nv.actions.map((a) =>
    decodeMessage(encodeCommandRequest(a.command, 3n).buffer).payload.id),
    ['draft.diff', 'draft.discard', 'draft.dismiss']);
});

function externalSection() {
  return {
    external_modification: {
      revision: 3,
      message: '2 files changed on disk',
      selected: 'external:src/a:b.cpp',
      files: [
        { id: 'external:src/a:b.cpp', path: 'src/a:b.cpp', status: 0,
          accessible_status: 'modified on disk', status_label: 'Δ',
          actions: [
            { action: 0, label: 'Load disk!', command: 'published.reload' },
            { action: 1, label: 'Preserve mine!', command: 'published.keep' },
            { action: 2, label: 'Compare now!', command: 'published.diff' },
          ] },
        { id: 'external:src/removed.cpp', path: 'src/removed.cpp', status: 1,
          accessible_status: 'removed elsewhere', status_label: 'gone',
          actions: [
            { action: 0, label: 'Reload', command: 'external.reload' },
            { action: 2, label: 'Diff', command: 'external.diff' },
          ] },
      ],
    },
  };
}

check('externalModificationFromSections renders one row per file with the selected highlight', () => {
  assert.equal(externalModificationFromSections(null), null);
  assert.equal(externalModificationFromSections({}), null);
  assert.equal(externalModificationFromSections({ external_modification: { files: [] } }), null);
  const bar = externalModificationFromSections(externalSection());
  assert.equal(bar.files.length, 2);
  assert.equal(bar.message, '2 files changed on disk');
  // The selected row is the one whose id matches section.selected; the other is not.
  assert.equal(bar.files[0].selected, true);
  assert.equal(bar.files[1].selected, false);
  // Status labels + offered action affordances pass through from the wire.
  assert.equal(bar.files[0].statusLabel, 'Δ');
  assert.equal(bar.files[1].statusLabel, 'gone');
  assert.deepEqual(
    bar.files[0].actions.map((a) => [a.label, a.command]),
    [
      ['Load disk!', 'published.reload'],
      ['Preserve mine!', 'published.keep'],
      ['Compare now!', 'published.diff'],
    ]);
  assert.deepEqual(bar.files[1].actions.map((a) => a.action), [0, 2]);
});

check('a click on an external action sends a typed validated invocation', () => {
  const bar = externalModificationFromSections(externalSection());
  const file = bar.files[0];
  const action = file.actions[0];
  assert.deepEqual(
    decodeMessage(encodeExternalAction(action.action, file.id, 11n).buffer).payload,
    { id: 'external.invoke_action', base_revision: 11n,
      payload: { file_id: 'external:src/a:b.cpp', action: 0n } });
});

check('the web suppresses document echo when external_focus_held is true, never comparing a focus ordinal', () => {
  // The wire focus never carries ExternalModification; the additive bool is the
  // only signal, so a section with focus=Editor(0) but the bool set is external.
  assert.equal(externalFocusHeld(null), false);
  assert.equal(externalFocusHeld({ focus: 0 }), false);
  assert.equal(externalFocusHeld({ focus: 0, external_focus_held: 1 }), true);
  assert.equal(externalFocusHeld({ external_focus_held: 0 }), false);
});

check('applyExternalModificationDelta merges upserts, removes, and re-homes the selection', () => {
  const section = externalSection().external_modification;
  const merged = applyExternalModificationDelta(section, {
    revision: 4,
    message: '2 files changed on disk',
    removed: ['external:src/removed.cpp'],
    upserted: [{
      id: 'external:src/new.cpp', path: 'src/new.cpp', status: 0,
      accessible_status: 'modified', status_label: 'M',
      actions: [{ action: 0, label: 'Reload', command: 'external.reload' }],
    }],
    selected: 'external:src/new.cpp',
  });
  const ids = merged.files.map((f) => String(f.id));
  assert.ok(!ids.includes('external:src/removed.cpp'));
  assert.ok(ids.includes('external:src/new.cpp'));
  assert.equal(merged.selected, 'external:src/new.cpp');
  assert.equal(merged.revision, 4);
});

console.log('reconcile oracle: ' + checks + ' checks passed');