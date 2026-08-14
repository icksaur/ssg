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
  parseEnvelope, paletteReportIsFresh, paletteSelectedWindowRow,
  isPalettePromptOpen,
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

check('a stale palette report (older requestId) is rejected', () => {
  // Client has sent up to request 5; a report for 3 must not be applied.
  assert.equal(paletteReportIsFresh({ requestId: 5 }, 5), true);
  assert.equal(paletteReportIsFresh({ requestId: 6 }, 5), true);
  assert.equal(paletteReportIsFresh({ requestId: 3 }, 5), false);
});

check('palette selection highlight uses the absolute index minus the window start', () => {
  // rows is the visible window; selected is an ABSOLUTE ranked index.
  const rows = [{}, {}, {}]; // a 3-row window
  // Window starts at 10, selection 12 -> visible row 2.
  assert.equal(paletteSelectedWindowRow({ selected: 12, firstVisible: 10, rows }), 2);
  // Window starts at 10, selection 10 -> visible row 0.
  assert.equal(paletteSelectedWindowRow({ selected: 10, firstVisible: 10, rows }), 0);
  // Selection above the window -> not highlighted.
  assert.equal(paletteSelectedWindowRow({ selected: 4, firstVisible: 10, rows }), -1);
  // Selection below the window -> not highlighted.
  assert.equal(paletteSelectedWindowRow({ selected: 13, firstVisible: 10, rows }), -1);
  // No selection.
  assert.equal(paletteSelectedWindowRow({ selected: -1, firstVisible: 0, rows }), -1);
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
  firstUnsupportedPrimitive, interpretChrome, WEB_UI_PROFILE, WIDGET, REGION,
} from '../../apps/web/reconcile.mjs';

// A leaf node on the wire: { id, size, leaf: { kind, ..., role?, width? } }.
const leafNode = (id, kind, extra) => ({ id, size: {}, leaf: { kind, ...(extra || {}) } });
// A region whose root is a Row container over `children`. The container's id 'r'+role
// gets a state record too, so correspondence holds.
const rowRegion = (role, children) => ({
  role,
  root: { id: 'r' + role, size: {}, container: { axis: 0, gap: 0, children } },
});
// The dynamic-state record for a node.
const st = (id, leaf) => ({ id, present: 1, leaf: leaf || null });

// Flatten a render tree to its drawn leaves in order (the DOM builder mirrors this).
function drawnLeaves(node, out = []) {
  if (!node) return out;
  if (node.kind === 'container') { for (const c of node.children) drawnLeaves(c, out); }
  else out.push(node);
  return out;
}

check('firstUnsupportedPrimitive accepts a supported header/footer schema', () => {
  const schema = { generation: 1, regions: [
    rowRegion(REGION.TOP, [leafNode('a', WIDGET.FIELD), leafNode('b', WIDGET.SPACER)]),
    rowRegion(REGION.BOTTOM, [leafNode('c', WIDGET.CHECKBOX)]),
  ] };
  assert.equal(firstUnsupportedPrimitive(schema), null);
});

check('firstUnsupportedPrimitive rejects an unsupported widget kind (TextInput)', () => {
  const schema = { generation: 1, regions: [
    rowRegion(REGION.TOP, [leafNode('a', WIDGET.TEXT_INPUT)]),
  ] };
  assert.deepEqual(firstUnsupportedPrimitive(schema), { kind: 'widget', ordinal: WIDGET.TEXT_INPUT });
});

check('firstUnsupportedPrimitive rejects an unsupported region role (Overlay)', () => {
  const schema = { generation: 1, regions: [ rowRegion(REGION.OVERLAY, []) ] };
  assert.deepEqual(firstUnsupportedPrimitive(schema), { kind: 'region', ordinal: REGION.OVERLAY });
});

check('interpretChrome applies the per-kind render gate', () => {
  const schema = { generation: 4, regions: [
    rowRegion(REGION.TOP, [
      leafNode('lit', WIDGET.FIELD),      // present with leaf -> drawn
      leafNode('empty', WIDGET.LABEL),    // present, nullopt leaf -> NOT drawn (resolved drop)
      leafNode('sp', WIDGET.SPACER),      // present spacer -> drawn as a gap despite no leaf
      leafNode('box', WIDGET.CHECKBOX),   // checkbox -> drawn
    ]),
  ] };
  const state = { generation: 4, nodes: [
    st('r0'), // the container node
    st('lit', { value: 'hello', label: 'hello', command: 'open.thing' }),
    st('empty', null),
    st('sp', null),
    st('box', { value: 'case', label: 'case', checked: 1 }),
  ] };
  const out = interpretChrome(schema, state);
  assert.ok(out);
  assert.equal(out.regions.length, 1);
  // The render tree preserves the container; flatten to drawn leaves.
  assert.equal(out.regions[0].node.kind, 'container');
  const items = drawnLeaves(out.regions[0].node);
  // lit (drawn), sp (gap), box (checkbox) -- empty Label is dropped.
  assert.deepEqual(items.map((i) => i.id), ['lit', 'sp', 'box']);
  assert.equal(items[0].text, 'hello');
  assert.equal(items[0].command, 'open.thing');
  assert.equal(items[1].spacer, true);
  assert.equal(items[2].checked, true);
  assert.equal(items[2].text, 'case');
});

check('interpretChrome preserves the left/middle/right grouping and flex packing', () => {
  // Row[ left(Auto container), middle(Flex container w/ center), right(Auto container) ].
  const grp = (id, sizeKind, children) => ({ id, size: { kind: sizeKind }, container: { axis: 0, gap: 0, children } });
  const schema = { generation: 2, regions: [ {
    role: REGION.BOTTOM,
    root: grp('root', 1 /*Flex*/, [
      grp('left', 2 /*Auto*/, [leafNode('l0', WIDGET.FIELD)]),
      grp('mid', 1 /*Flex*/, [{ id: 'c0', size: { kind: 1 }, leaf: { kind: WIDGET.LABEL } }]),
      grp('right', 2 /*Auto*/, [leafNode('r0', WIDGET.FIELD, { command: 'do.it' })]),
    ]),
  } ] };
  const state = { generation: 2, nodes: [
    st('root'), st('left'), st('l0', { value: 'L', label: 'L' }),
    st('mid'), st('c0', { value: 'C', label: 'C' }),
    st('right'), st('r0', { value: 'R', label: 'R', command: 'do.it' }),
  ] };
  const out = interpretChrome(schema, state);
  assert.ok(out);
  const root = out.regions[0].node;
  assert.equal(root.kind, 'container');
  assert.equal(root.children.length, 3);
  assert.equal(root.children[1].flex, true);   // the middle group is Flex-sized
  assert.equal(root.children[0].flex, false);   // the left group is Auto
  // The middle's center leaf is present and flex.
  assert.equal(root.children[1].children[0].text, 'C');
});

check('interpretChrome returns null on a generation or node-id mismatch', () => {
  const schema = { generation: 4, regions: [ rowRegion(REGION.TOP, [leafNode('a', WIDGET.FIELD)]) ] };
  // Generation mismatch.
  assert.equal(interpretChrome(schema, { generation: 3, nodes: [st('r0'), st('a', { value: 'x', label: 'x' })] }), null);
  // Node-id set mismatch (missing 'a').
  assert.equal(interpretChrome(schema, { generation: 4, nodes: [st('r0')] }), null);
});

check('interpretChrome returns null on a state/schema SHAPE disagreement', () => {
  const schema = { generation: 5, regions: [ rowRegion(REGION.TOP, [
    leafNode('box', WIDGET.CHECKBOX), leafNode('sp', WIDGET.SPACER),
  ]) ] };
  // Checkbox missing its leaf state -> wait.
  assert.equal(interpretChrome(schema, { generation: 5, nodes: [
    st('r0'), st('box', null), st('sp', null),
  ] }), null);
  // Spacer carrying leaf state -> wait.
  assert.equal(interpretChrome(schema, { generation: 5, nodes: [
    st('r0'), st('box', { value: '', label: '', checked: 0 }), st('sp', { value: 'x', label: 'x' }),
  ] }), null);
  // Container carrying leaf state -> wait.
  assert.equal(interpretChrome(schema, { generation: 5, nodes: [
    st('r0', { value: 'x', label: 'x' }), st('box', { value: '', label: '', checked: 0 }), st('sp', null),
  ] }), null);
});

console.log('reconcile oracle: ' + checks + ' checks passed');
