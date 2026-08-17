// Reconciliation oracle (node): exercises the REAL reconcile.mjs the browser
// runs, so the local-echo contract is pinned against the shipped client code,
// not a copy. Run by ctest via node when node is available.
//
// Contract pinned: the client shows the authoritative document plus its own
// not-yet-settled predictions; when the host settles an id, exactly the
// predictions through that id drop and the remainder re-bases onto the
// authoritative document. Applied and rejected settle identically because
// authoritative state wins.

import assert from 'node:assert/strict';
import {
  applyDocumentDelta, dropSettled, project, byteToIndex, utf8Bytes,
  parseEnvelope,
  isPalettePromptOpen, matcherParametersFromWire, matcherBoundsFromPalette,
  clampPaletteSelection, encodePaletteSubmit, applyTreeDelta,
  applySessionDeltaSections,
  encodeStatusActionInvocation,
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

check('dropSettled keeps only predictions beyond the settled id', () => {
  const p = [{ id: 1, text: 'a' }, { id: 2, text: 'b' }, { id: 3, text: 'c' }];
  assert.deepEqual(dropSettled(p, 0).map((x) => x.id), [1, 2, 3]);
  assert.deepEqual(dropSettled(p, 2).map((x) => x.id), [3]);
  assert.deepEqual(dropSettled(p, 3).map((x) => x.id), []);
});

check('project splices predictions at the caret and moves the caret past them', () => {
  const r = project('ab', 1, [{ id: 1, text: 'X' }, { id: 2, text: 'Y' }]);
  assert.equal(r.text, 'aXYb');
  assert.equal(r.caret, 3);       // 1 + 2 predicted bytes
  assert.equal(r.predStart, 1);
  assert.equal(r.predEnd, 3);
});

// --- The end-to-end reconciliation property, over an interleaving of local
// keystrokes, coalesced server settlements, and a background revision bump. ---
check('typed text is always shown, and settled predictions re-base exactly', () => {
  // The user types these characters (one is astral), each predicted at the caret.
  const typed = ['h', 'e', '\u{1f600}', 'y'];

  // Client state.
  let pending = [];
  let nextId = 1;
  const typedQueue = [];           // ids the server has not yet applied

  // Authoritative document (what the server owns).
  let authText = '';
  let authCaret = 0;               // byte offset

  // What the user has committed to typing so far (prediction source of truth).
  let typedSoFar = '';

  const shown = () => project(authText, authCaret, pending).text;

  // Server applies the next `count` queued predictions as one coalesced delta
  // (insert at the caret), then settles up to the last applied id.
  const serverApply = (count) => {
    const batch = typedQueue.splice(0, count);
    if (batch.length === 0) return 0;
    const inserted = batch.map((e) => e.text).join('');
    authText = applyDocumentDelta(authText, { start: authCaret, erased_bytes: 0, inserted_text: inserted });
    authCaret += utf8Bytes(inserted);
    const settledId = batch[batch.length - 1].id;
    pending = dropSettled(pending, settledId);
    return settledId;
  };

  const typeKey = (ch) => {
    const id = nextId++;
    pending.push({ id, text: ch });
    typedQueue.push({ id, text: ch });
    typedSoFar += ch;
    // The predicted char is visible immediately, before any round trip.
    assert.equal(shown(), typedSoFar);
  };

  // Interleave: type two, coalesce-apply two, type two more, a background frame
  // (no settlement, no user content change), then apply the rest one at a time.
  typeKey(typed[0]);
  typeKey(typed[1]);
  assert.equal(shown(), 'he');
  serverApply(2);                            // coalesced settlement of h,e
  assert.equal(shown(), 'he');               // stable across the round trip
  assert.equal(pending.length, 0);

  typeKey(typed[2]);                          // astral
  typeKey(typed[3]);
  assert.equal(shown(), typedSoFar);

  // A background revision bump: authoritative gains no user content and settles
  // nothing; the still-unsettled predictions must survive untouched.
  const before = shown();
  pending = dropSettled(pending, 0);          // settledId sentinel = nothing new
  assert.equal(shown(), before);
  assert.equal(pending.length, 2);

  serverApply(1);                             // settle the astral char only
  assert.equal(shown(), typedSoFar);
  assert.equal(pending.length, 1);
  serverApply(1);                             // settle the last char
  assert.equal(shown(), typedSoFar);
  assert.equal(pending.length, 0);

  // Everything the user typed is now authoritative and shown.
  assert.equal(authText, 'he\u{1f600}y');
  assert.equal(shown(), 'he\u{1f600}y');
});

// --- Authoritative-wins on divergence: a rejected prediction (never applied)
// settles by id and is dropped without ever appearing in the document. ---
check('a rejected prediction is dropped on settlement, authoritative wins', () => {
  let pending = [{ id: 1, text: 'x' }];
  const authText = '';               // the host rejected the insert: no change
  const authCaret = 0;
  assert.equal(project(authText, authCaret, pending).text, 'x');   // predicted
  pending = dropSettled(pending, 1); // host settles id 1 (rejected)
  assert.equal(project(authText, authCaret, pending).text, '');    // re-based away
});

// --- Host envelope framing and the palette stale-report guard ---
check('parseEnvelope splits settledId and tagged sections', () => {
  // [u64 settledId=7][count=2][tag0 len3 'abc'][tag1 len2 '{}']
  const bytes = [];
  const push64 = (v) => { let b = BigInt(v); for (let i = 0; i < 8; i++) { bytes.push(Number(b & 0xffn)); b >>= 8n; } };
  const push32 = (v) => { for (let i = 0; i < 4; i++) bytes.push((v >> (i * 8)) & 0xff); };
  push64(7); bytes.push(2);
  bytes.push(0); push32(3); bytes.push(97, 98, 99);      // tag 0 "abc"
  bytes.push(1); push32(2); bytes.push(123, 125);        // tag 1 "{}"
  const buf = new Uint8Array(bytes).buffer;
  const { settledId, sections } = parseEnvelope(buf);
  assert.equal(settledId, 7n);
  assert.equal(sections.length, 2);
  assert.equal(sections[0].tag, 0);
  assert.equal(sections[0].length, 3);
  assert.equal(sections[1].tag, 1);
  const s1 = new TextDecoder().decode(new Uint8Array(sections[1].dv.buffer, sections[1].dv.byteOffset, sections[1].length));
  assert.equal(s1, '{}');
});

check('encodeStatusActionInvocation emits the exact StatusActionInvocation wire frame', () => {
  const actual = encodeStatusActionInvocation({ statusId: 7, actionId: 'dismiss', generation: 3 });
  const expected = new Uint8Array([
    1, 5,
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

check('encodePaletteSubmit sends the candidate id, not a selected index and query', () => {
  assert.equal(encodePaletteSubmit('command.open'), 'PSUB:command.open');
  assert.equal(encodePaletteSubmit(''), null);
});

check('applyTreeDelta splices the retained tree and resyncs only when inexpressible', () => {
  const tree = () => ({ revision: 1, providers: [
    { provider_id: 'fs', kind: 0, nodes: [
      { node: { id: 'a' }, depth: 0 }, { node: { id: 'x' }, depth: 0 }], selected: 'a' }] });
  // A splice erases node x and inserts b, advancing the retained revision.
  let t = tree();
  assert.equal(applyTreeDelta(t, { base_revision: 1, revision: 2, snapshot_required: false,
    providers: [{ provider_id: 'fs', kind: 0, start: 1, erase_count: 1,
      insert: [{ node: { id: 'b' }, depth: 0 }], selected: 'b' }] }), true);
  assert.equal(t.revision, 2);
  assert.deepEqual(t.providers[0].nodes.map((r) => r.node.id), ['a', 'b']);
  assert.equal(t.providers[0].selected, 'b');
  // A no-op delta (revision equals base) applies cleanly and stays current.
  t = tree();
  assert.equal(applyTreeDelta(t, { base_revision: 1, revision: 1, snapshot_required: false, providers: [] }), true);
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
    providers: [{ provider_id: 'git', kind: 1, start: 0, erase_count: 0,
      insert: [{ node: { id: 'g' }, depth: 0 }], selected: null }] }), true);
  assert.deepEqual(t.providers.map((p) => p.provider_id), ['fs', 'git']);
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

check('palette-prompt detection reads the wire snake_case field names', () => {
  // The decoded sections object uses the encoder's names: prompt_status and
  // active_kind, NOT camelCase. A regression here silently hides the overlay
  // (the picker captures input but nothing renders), which is why it is pinned.
  const editor = { focus: 0, prompt_status: { active_kind: null } };
  assert.equal(isPalettePromptOpen(editor), false);
  // Focus on prompt with the palette kind (5) -> open.
  const palette = { focus: 2, prompt_status: { active_kind: 5 } };
  assert.equal(isPalettePromptOpen(palette), true);
  // BigInt ordinals (as the wire decoder yields) are handled.
  assert.equal(isPalettePromptOpen({ focus: 2n, prompt_status: { active_kind: 5n } }), true);
  // A camelCase object (the old bug) must NOT be seen as open.
  assert.equal(isPalettePromptOpen({ focus: 2, promptStatus: { activeKind: 5 } }), false);
  // Prompt focus but a non-palette prompt kind (e.g. a path prompt) -> closed.
  assert.equal(isPalettePromptOpen({ focus: 2, prompt_status: { active_kind: 1 } }), false);
});

// --- UI-VM: profile rejection + schema/state interpretation ---
import {
  firstUnsupportedPrimitive, interpretChrome, WEB_UI_PROFILE, WIDGET, SIZE, SURFACE,
  shouldResetLocalQuery, pickerEpochFromPalette,
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
  const surfaces = [SURFACE.TABVIEW, SURFACE.FILETREE, SURFACE.GITSTATUS, SURFACE.FINDRESULTS, SURFACE.SYMBOLS];
  const root = rowNode('root', surfaces.map((surface) => leafNode('surface-' + surface, WIDGET.VIEW, { surface })));
  const state = { generation: 8, nodes: [st('root'), ...surfaces.map((surface) => st('surface-' + surface))] };
  const out = interpretChrome(schemaOf(8, root), state, presenceForSchema(8, root));
  assert.ok(out);
  const items = drawnLeaves(out.root);
  assert.deepEqual(items.map((i) => i.surface), surfaces);
  assert.deepEqual(items.map((i) => i.widget), surfaces.map(() => WIDGET.VIEW));
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
    leafNode('view', WIDGET.VIEW, { surface: SURFACE.TABVIEW }),
    leafNode('actions', WIDGET.STATUS_ACTIONS),
  ]);
  const profile = {
    ...WEB_UI_PROFILE,
    widgets: new Set([...WEB_UI_PROFILE.widgets, WIDGET.STATUS_ACTIONS]),
    surfaces: new Set([SURFACE.TABVIEW]),
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

check('shouldResetLocalQuery clears the query on a fresh open and a same-state reopen, not while staying open', () => {
  // Closed -> open: reset.
  assert.equal(shouldResetLocalQuery(true, false, 1n, 0n), true);
  // Open -> still open, same epoch: keep the local query.
  assert.equal(shouldResetLocalQuery(true, true, 1n, 1n), false);
  // Open -> still open, bumped epoch (reopen without a closed frame): reset.
  assert.equal(shouldResetLocalQuery(true, true, 2n, 1n), true);
  // Distinct uint64 epochs above Number.MAX_SAFE_INTEGER must not collapse.
  const high = BigInt(Number.MAX_SAFE_INTEGER) + 1n;
  assert.equal(shouldResetLocalQuery(true, true, high + 1n, high), true);
  // Not open: never reset.
  assert.equal(shouldResetLocalQuery(false, true, 5n, 1n), false);
});

check('pickerEpochFromPalette treats absent as zero and rejects malformed present values', () => {
  assert.equal(pickerEpochFromPalette(null), 0n);
  assert.equal(pickerEpochFromPalette({}), 0n);
  assert.equal(pickerEpochFromPalette({ picker_epoch: 0n }), 0n);
  assert.equal(pickerEpochFromPalette({ picker_epoch: 9 }), 9n);
  assert.throws(() => pickerEpochFromPalette({ picker_epoch: '9' }), /picker_epoch/);
  assert.throws(() => pickerEpochFromPalette({ picker_epoch: -1 }), /picker_epoch/);
  assert.throws(() => pickerEpochFromPalette({ picker_epoch: Number.MAX_SAFE_INTEGER + 1 }), /picker_epoch/);
});

// --- Footer prompt: semantic PromptView projection, delta, focus plan, ingress ---
import {
  promptViewFromSections, promptFocusPlan, promptFocusControlMessage,
  PROMPT_CONTROL,
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

check('promptFocusControlMessage carries the clicked input index to prompt.focus_control', () => {
  assert.equal(promptFocusControlMessage(0), 'PFOC:0');
  assert.equal(promptFocusControlMessage(2), 'PFOC:2');
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
  // The client dispatches each action as 'CMD:'+command; assert the wire string a
  // click would send matches the already-registered draft commands.
  assert.deepEqual(nv.actions.map((a) => 'CMD:' + a.command),
    ['CMD:draft.diff', 'CMD:draft.discard', 'CMD:draft.dismiss']);
});

console.log('reconcile oracle: ' + checks + ' checks passed');
