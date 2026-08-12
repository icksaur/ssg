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

console.log('reconcile oracle: ' + checks + ' checks passed');
