// Reconciliation oracle (node): exercises the REAL reconcile.mjs the browser
// runs, so the local-echo contract is pinned against the shipped client code,
// not a copy. Run by ctest via node when node is available.
//
// Contract pinned: the client shows the authoritative document plus its own
// not-yet-completed predictions. Typed input results complete in FIFO order only
// after their result revision is visible; authoritative state always wins.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import {
  applyDocumentDelta, project, byteToIndex, utf8Bytes, settleInput, cssColor,
  decodeMessage, browserInboundKind, encodeClientInput,
  encodeViewNavigationInput, encodeResolvedSelectionInput, encodeCommandRequest,
  encodeUiNodeActivationCommand,
  BrowserKeyDispatchTracker,
  settleCommandResult,
  matcherParametersFromWire, matcherBoundsFromPalette,
  clampPaletteSelection, pickerCandidatesFromPalette, resolvePickerLifecycle,
  effectivePickerMode, queuePickerSubmit, settlePickerLifecycle,
  pickerPresentationFromSubmit,
  resolveKeyCommand, predictPickerInput, applyPickerInputPrediction,
  CLIENT_OWNED_INPUT, deleteLastGrapheme, deleteLastWord,
  PICKER_MODE, encodePickerPointerInput, encodeDocumentPointerInput,
  encodeScrollLinesInput, encodeScrollFractionInput,
  encodeTabPointerInput, markedTextByteOffset, encodeTreePointerInput,
  applyTreeDelta, normalizeTreeActiveBinding,
  applySessionDeltaSections, applySessionDeltaCopy, applySessionDelta,
  applyUiFrameDelta,
  findSections,
  encodeStatusActionPointerInput, encodePromptControlPointerInput,
  encodePublishedUiActionPointerInput, encodeNoticeActionPointerInput,
  externalModificationFromSections,
  encodeExternalActionPointerInput,
  applyExternalModificationDelta, isCurrentGeneration, replayAttachFrame,
  deltaIsContiguous, clearUncertainInputs, reconnectDelay,
  predictPromptValue,
  settlePromptPrediction,
  settlePromptPresentation, deferPromptDocumentSurface,
  mergeBrowserRenderPlans,
} from '../../apps/web/reconcile.mjs';

let checks = 0;
const check = (name, fn) => { fn(); checks++; };

const fixtureBytes = (name) => {
  const hex = fs.readFileSync(
    new URL('../fixtures/protocol/' + name, import.meta.url), 'utf8').trim();
  return Uint8Array.from(
    hex.match(/../g).map((pair) => Number.parseInt(pair, 16)));
};
const fixtureMessage = (name) => decodeMessage(fixtureBytes(name).buffer).payload;

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

check('prompt prediction appends and deletes one grapheme immediately', () => {
  assert.equal(predictPromptValue('nee', 'x', false), 'neex');
  assert.equal(predictPromptValue('a\u0301b', 'Backspace', false), 'a\u0301');
  assert.equal(predictPromptValue('a\u0301', 'Backspace', false), '');
  assert.equal(predictPromptValue('', 'Backspace', false), null);
  assert.equal(predictPromptValue('', 'Escape', false), null);
  assert.equal(predictPromptValue('two words', 'Backspace', true), null);
});

check('prompt prediction remains until its last serialized input settles', () => {
  const prediction = { controlId: 'find.query', value: 'abc' };
  assert.equal(
    settlePromptPrediction(prediction,
                           [{ promptPrediction: true, promptInput: true }],
                           { promptPrediction: true }),
    prediction);
  assert.equal(
    settlePromptPrediction(prediction,
                           [{ promptPrediction: false, promptInput: true }],
                           { promptPrediction: true }),
    prediction);
  assert.equal(
    settlePromptPrediction(prediction, [], { promptPrediction: true }), null);
  assert.equal(
    settlePromptPrediction(prediction, [], { promptPrediction: false }),
    null);
  assert.equal(
    settlePromptPrediction(prediction,
                           [{ promptPrediction: false, promptInput: false }],
                           { promptPrediction: false }),
    null);
});

check('picker submit waits for its authoritative open and dispatches once', () => {
  const candidate = {
    mode: PICKER_MODE.FILE, candidateId: 'src/main.cpp',
    query: 'main', selected: 2,
  };
  const activation = { mode: PICKER_MODE.FILE, id: 17n };
  assert.deepEqual(
    queuePickerSubmit(candidate, null, true, null),
    { send: null, queued: candidate });
  assert.deepEqual(
    queuePickerSubmit(candidate, activation, true, null),
    { send: null, queued: candidate });
  assert.deepEqual(
    queuePickerSubmit({ ...candidate, candidateId: 'other.cpp' },
                      null, true, candidate),
    { send: null, queued: candidate });
  assert.deepEqual(
    settlePickerLifecycle(
      { pickerOpenMode: PICKER_MODE.FILE }, activation, activation, candidate),
    {
      activation,
      submit: { activation, candidateId: 'src/main.cpp' },
      preservePresentation: true,
});
  assert.deepEqual(
    settlePickerLifecycle(
      { pickerOpenMode: PICKER_MODE.FILE }, activation, activation, null),
    {
      activation, submit: null, preservePresentation: true,
    });

check('picker submit cancels when authoritative open fails or changes mode', () => {
  const candidate = {
    mode: PICKER_MODE.FILE, candidateId: 'src/main.cpp',
    query: 'main', selected: 0,
  };
  const fileActivation = { mode: PICKER_MODE.FILE, id: 21n };
  const commandActivation = { mode: PICKER_MODE.COMMAND, id: 22n };
  assert.deepEqual(
    settlePickerLifecycle(
      { pickerOpenMode: PICKER_MODE.FILE }, null, null, candidate),
    { activation: null, submit: null, preservePresentation: false });
  assert.deepEqual(
    settlePickerLifecycle(
      { pickerOpenMode: PICKER_MODE.FILE }, commandActivation,
      fileActivation, candidate),
    {
      activation: commandActivation, submit: null,
      preservePresentation: false,
    });
  assert.deepEqual(
    queuePickerSubmit(candidate, fileActivation, false, null),
    {
      send: { activation: fileActivation, candidateId: candidate.candidateId },
      queued: null,
    });

});

check('queued picker submit never retargets to a later same-mode activation', () => {
  const intended = { mode: PICKER_MODE.FILE, id: 41n };
  const replacement = { mode: PICKER_MODE.FILE, id: 42n };
  const candidate = {
    mode: PICKER_MODE.FILE, candidateId: 'src/main.cpp',
    query: 'main', selected: 0,
  };
  assert.deepEqual(
    settlePickerLifecycle(
      { pickerOpenMode: PICKER_MODE.FILE }, replacement, intended, candidate),
    {
      activation: replacement, submit: null,
      preservePresentation: false,
    });
});

check('picker submit settlement adopts success and preserves rejected intent', () => {
  const activation = { mode: PICKER_MODE.COMMAND, id: 31n };
  const previous = {
    mode: PICKER_MODE.COMMAND,
    activationId: 31n,
    query: 'tog',
    selected: 3,
    pendingSubmit: { activation, candidateId: 'panel.toggle' },
  };
  assert.deepEqual(pickerPresentationFromSubmit(0, null, previous), {
    mode: null,
    activationId: null,
    query: '',
    selected: 0,
    pendingSubmit: null,
  });
  assert.deepEqual(pickerPresentationFromSubmit(1, activation, previous), {
    mode: PICKER_MODE.COMMAND,
    activationId: 31n,
    query: 'tog',
    selected: 3,
    pendingSubmit: null,
  });
  assert.deepEqual(
    pickerPresentationFromSubmit(
      1, { mode: PICKER_MODE.COMMAND, id: 32n }, previous),
    {
      mode: PICKER_MODE.COMMAND,
      activationId: 32n,
      query: '',
      selected: 0,
      pendingSubmit: null,
    });
});

check('picker pointer input carries authoritative activation identity', () => {
  const decoded = decodeMessage(
    encodePickerPointerInput(
      { mode: PICKER_MODE.FILE, id: 41n }, 'src/main.cpp').buffer);
  assert.equal(decoded.kind, 7);
  assert.deepEqual(decoded.payload, {
    kind: 3n, button: 0n, phase: 0n,
    picker_mode: BigInt(PICKER_MODE.FILE),
    activation_id: 41n, candidate_id: 'src/main.cpp',
  });
});

check('picker query lifetime follows activation identity', () => {
  const previous = {
    mode: PICKER_MODE.FILE,
    activationId: 51n,
    query: 'main',
    selected: 1,
    pendingSubmit: null,
  };
  assert.deepEqual(
    pickerPresentationFromSubmit(
      1, { mode: PICKER_MODE.FILE, id: 52n }, previous),
    {
      mode: PICKER_MODE.FILE,
      activationId: 52n,
      query: '',
      selected: 0,
      pendingSubmit: null,
    });
  });
});

check('pending prompt search defers document rendering and honors user scroll', () => {
  assert.deepEqual(deferPromptDocumentSurface([8, 1], true), [1]);
  assert.deepEqual(deferPromptDocumentSurface([8, 1], false), [8, 1]);
  assert.deepEqual(
    settlePromptPresentation(
      { controlId: 'find.query', value: 'abc' },
      [{ promptPrediction: true, promptInput: true }],
      { promptPrediction: true }, true),
    { prediction: { controlId: 'find.query', value: 'abc' },
      renderDocument: false, revealDocument: false });
  assert.deepEqual(
    settlePromptPresentation(
      { controlId: 'find.query', value: 'abc' },
      [], { promptPrediction: true }, true),
    { prediction: null, renderDocument: true, revealDocument: false });
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

check('a view-owned result settles without a semantic revision advance', () => {
  const settled = settleInput(
    [{ predictionId: null }], [],
    {
      outcome: 4n,
      command: {
        revision: 7n,
        view_action: {
          view_id: 1n,
          semantic_revision: 7n,
          action: { kind: 0n, target: 0n, rows: 1n },
        },
      },
    },
    7n);
  assert.deepEqual(settled, { inputQueue: [], pending: [] });
});

check('typed raw input and command requests round-trip through ProtocolValue', () => {
  assert.deepEqual(decodeMessage(encodeClientInput({
    code: 'KeyA', alt: true, shift: false, text: 'a',
  }).buffer), {
    kind: 7,
    payload: {
      kind: 0n,
      stroke: { code: 'KeyA', control: false, alt: true, meta: false, shift: false },
      committed_text: 'a',
    },
  });
  assert.deepEqual(decodeMessage(encodeViewNavigationInput(9n).buffer), {
    kind: 7,
    payload: { kind: 12n, basis_revision: 9n },
  });
  assert.deepEqual(
    decodeMessage(encodeResolvedSelectionInput(
      9n, 4n, 12n, [{ anchor: 3, active: 8 }]).buffer),
    {
      kind: 7,
      payload: {
        kind: 14n,
        basis_revision: 9n,
        active_tab: 4n,
        document_revision: 12n,
        selections: [{ anchor: 3n, active: 8n }],
      },
    });

  check('browser protocol decoding rejects malformed and over-bound values', () => {
    assert.throws(() => decodeMessage(
      new Uint8Array([1, 9, 0]).buffer), /unsupported protocol frame/);
    assert.throws(() => decodeMessage(
      new Uint8Array([4, 1, 1, 2]).buffer), /malformed protocol bool/);
    assert.throws(() => decodeMessage(
      new Uint8Array([4, 1, 4, 4, 0, 0, 0, 65]).buffer), /truncated/);
    assert.throws(() => decodeMessage(
      new Uint8Array([4, 1, 6, 1, 0, 1, 0]).buffer), /collection length/);
  });

  check('browser ignores additive message kinds without weakening wire versions', () => {
    const decoded = decodeMessage(new Uint8Array([4, 9, 0]).buffer);
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
  assert.deepEqual(decodeMessage(
    encodeUiNodeActivationCommand('footer.action', 4n, 9n).buffer), {
    kind: 0,
    payload: {
      id: 'ui.activate',
      base_revision: 9n,
      payload: { generation: 4n, node_id: 'footer.action' },
    },
  });
});

check('browser key tracker falls back only when an Alt keydown was consumed', () => {
  const keys = new BrowserKeyDispatchTracker();
  assert.equal(keys.keydown('AltLeft'), false);
  assert.equal(keys.keyup('KeyB', true), true);
  assert.equal(keys.keyup('KeyB', true), true);
  assert.equal(keys.keydown('KeyJ'), true);
  assert.equal(keys.keyup('KeyJ', true), false);
  keys.keydown('KeyK');
  keys.clear();
  assert.equal(keys.keyup('KeyK', true), false);
  assert.equal(keys.keyup('ShiftLeft', true), false);
});

check('picker key prediction follows the published keymap and Unicode edits', () => {
  const keymap = { bindings: [
    { context: 'prompt', command_id: 'prompt.submit',
      sequence: [{ code: 'Enter' }] },
    { context: 'prompt', command_id: 'prompt.next',
      sequence: [{ code: 'ArrowDown' }] },
    { context: 'prompt', command_id: 'prompt.previous',
      sequence: [{ code: 'ArrowUp' }] },
    { context: 'prompt', command_id: 'prompt.cancel',
      sequence: [{ code: 'Escape' }] },
    { context: 'editor', command_id: 'select.document_start',
      sequence: [{ code: 'Home', control: true, shift: true }] },
  ] };
  assert.equal(resolveKeyCommand(
    keymap, { code: 'Home', control: true, shift: true }, 'editor'),
  'select.document_start');
  assert.deepEqual(
    predictPickerInput(keymap, { code: 'ArrowDown' }, ''),
    { kind: CLIENT_OWNED_INPUT.SELECT_NEXT, text: '' });
  assert.deepEqual(
    predictPickerInput(keymap, { code: 'KeyA' }, 'a'),
    { kind: CLIENT_OWNED_INPUT.APPEND_TEXT, text: 'a' });
  assert.deepEqual(
    predictPickerInput(keymap, { code: 'Backspace', alt: true }, ''),
    { kind: CLIENT_OWNED_INPUT.DELETE_WORD_BACKWARD, text: '' });
  assert.deepEqual(
    predictPickerInput(keymap, { code: 'Escape' }, ''), { close: true });

  assert.equal(deleteLastGrapheme('a\u0301b'), 'a\u0301');
  assert.equal(deleteLastGrapheme('a\u0301'), '');
  assert.equal(deleteLastWord('alpha beta  '), 'alpha ');
  assert.equal(deleteLastWord('alpha \u{1f642}'), '');
  assert.deepEqual(
    applyPickerInputPrediction(
      { query: 'a\u0301b', selected: 2, error: 'old' },
      { kind: CLIENT_OWNED_INPUT.DELETE_GRAPHEME_BACKWARD, text: '' }, 3),
    { query: 'a\u0301', selected: 0, error: '' });
});

check('status action uses the common semantic input envelope', () => {
  const decoded = decodeMessage(encodeStatusActionPointerInput(
    { statusId: 7, actionId: 'dismiss', generation: 3 }, 11n).buffer);
  assert.equal(decoded.kind, 7);
  assert.deepEqual(decoded.payload, {
    kind: 6n, button: 0n, phase: 0n, basis_revision: 11n,
    invocation: { status_id: 7n, action_id: 'dismiss', generation: 3n },
  });
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

check('browser-local picker mode backs a null authoritative mode', () => {
  assert.equal(effectivePickerMode({ active_mode: null }, PICKER_MODE.FILE),
               PICKER_MODE.FILE);
  assert.equal(effectivePickerMode(
                 { active_mode: BigInt(PICKER_MODE.COMMAND) },
                 PICKER_MODE.FILE),
               PICKER_MODE.COMMAND);
});

check('semantic pointer inputs carry published identities and revision', () => {
  assert.deepEqual(
    decodeMessage(
      encodePickerPointerInput(
        { mode: PICKER_MODE.COMMAND, id: 13n },
        'command.open').buffer).payload,
    { kind: 3n, button: 0n, phase: 0n,
      picker_mode: 4n, activation_id: 13n, candidate_id: 'command.open' });
  assert.deepEqual(
    decodeMessage(encodeDocumentPointerInput(
      7, 6n, { phase: 1 }).buffer).payload,
    { kind: 9n, button: 0n, phase: 1n, basis_revision: 6n,
      position: 7n, additive: false, select_word: false, edge: 0n });
  assert.deepEqual(
    decodeMessage(encodeTreePointerInput('tree:src', 6n).buffer).payload,
    { kind: 2n, button: 0n, phase: 0n,
      basis_revision: 6n, node_id: 'tree:src' });
  assert.deepEqual(
    decodeMessage(encodeTabPointerInput(17n, 6n).buffer).payload,
    { kind: 1n, button: 0n, phase: 0n,
      basis_revision: 6n, tab_id: 17n });
  assert.deepEqual(
    decodeMessage(encodeTabPointerInput(17n, 6n, 1).buffer).payload,
    { kind: 1n, button: 1n, phase: 0n,
      basis_revision: 6n, tab_id: 17n });
});

check('browser semantic input bytes match the C++ canonical frames', () => {
  const cases = [
    ['client_input_tab.hex', encodeTabPointerInput(17n, 6n)],
    ['client_input_tree.hex', encodeTreePointerInput('tree:src', 6n)],
    ['client_input_picker.hex',
      encodePickerPointerInput(
        { mode: PICKER_MODE.COMMAND, id: 13n }, 'command.open')],
    ['client_input_prompt_control.hex',
      encodePromptControlPointerInput('replace.replacement', 7n)],
    ['client_input_external_action.hex',
      encodeExternalActionPointerInput(0, 'external:src/a:b.cpp', 11n)],
    ['client_input_status_action.hex',
      encodeStatusActionPointerInput(
        { statusId: 7, actionId: 'dismiss', generation: 3 }, 11n)],
    ['client_input_ui_action.hex',
      encodePublishedUiActionPointerInput('header.help', 4n, 12n)],
    ['client_input_notice_action.hex',
      encodeNoticeActionPointerInput('draft.notice.dismiss', 12n)],
    ['client_input_document.hex',
      encodeDocumentPointerInput(8, 15n, { additive: true })],
    ['client_input_scroll_lines.hex',
      encodeScrollLinesInput(1, -4, 16n)],
    ['client_input_scroll_fraction.hex',
      encodeScrollFractionInput(0, 3, 8, 17n)],
    ['client_input_resolved_selection.hex',
      encodeResolvedSelectionInput(
        18n, 4n, 12n, [{ anchor: 3n, active: 8n }])],
  ];
  for (const [fixture, encoded] of cases) {
    assert.deepEqual(encoded, fixtureBytes(fixture));
  }
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
  assert.deepEqual(t.active_binding, { provider_id: 'fs', kind: 0 });
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
  assert.deepEqual(t.active_binding, { provider_id: 'git', kind: 1 });
  // An explicit binding must identify exactly one provider in the resulting inventory.
  t = tree();
  const beforeInvalidBinding = structuredClone(t);
  assert.equal(applyTreeDelta(t, {
    base_revision: 1, revision: 2, snapshot_required: false,
    provider_order: ['fs'], providers: [],
    active_binding: { provider_id: 'missing', kind: 0 },
  }), false);
  assert.deepEqual(t, beforeInvalidBinding);
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
  firstUnsupportedPrimitive, interpretChrome, WEB_UI_PROFILE, WIDGET, SIZE,
  SURFACE, SCROLL, AXIS,
  webExtentCss, applyNodeSemanticStyle, firstMalformedNodeStyle,
  getOrCreateStyledNode,
  GenerationRetainedCache, gitAffordanceFromNode,
  preferredKeyboardSurface, browserRenderPlan, settlePointerSelection,
  applyPalettePresenceOverlay, PALETTE_PRESENCE_OP,
  predictedFocusCapture, resolveUiFocusPath, focusUiNode,
  responsiveSurvivors, responsiveFlexCss,
} from '../../apps/web/reconcile.mjs';

// A leaf node on the wire: { id, size, leaf: { kind, ..., role?, width? } }.
const leafNode = (id, kind, extra) => ({ id, size: {}, leaf: { kind, ...(extra || {}) } });
const focusLeafNode = (id, kind, focusContext, extra) => ({
  ...leafNode(id, kind, extra),
  focus_context: focusContext,
});
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

check('focus path resolves the present top while allowing a hidden base', () => {
    const root = rowNode('root', [
      focusLeafNode('editor', WIDGET.VIEW, 0, { surface: SURFACE.DOCUMENT }),
      focusLeafNode('input_line', WIDGET.TEXT_INPUT, 2),
    ]);
    const schema = schemaOf(21, root);
    const presence = presenceForSchema(21, root, ['editor']);
    const state = {
      generation: 21,
      nodes: [st('root'), st('editor'), st('input_line')],
      focus_path: ['editor', 'input_line'],
    };
    assert.deepEqual(resolveUiFocusPath(schema, state, presence), {
      path: ['editor', 'input_line'],
      effective: 'input_line',
      context: 'prompt',
      captured: true,
    });
});

check('predicted picker focus is a disposable overlay on the authoritative path', () => {
    const root = rowNode('root', [
      focusLeafNode('editor', WIDGET.VIEW, 0, { surface: SURFACE.DOCUMENT }),
      focusLeafNode('input_line', WIDGET.TEXT_INPUT, 2),
    ]);
    const schema = schemaOf(22, root);
    const presence = presenceForSchema(22, root, ['editor']);
    const state = {
      generation: 22,
      nodes: [st('root'), st('editor'), st('input_line')],
      focus_path: ['editor'],
    };
    const predicted = predictedFocusCapture({
      localMode: PICKER_MODE.FILE,
      authoritativeActivation: null,
    });
    assert.equal(predicted, 'input_line');
    assert.deepEqual(resolveUiFocusPath(schema, state, presence, predicted), {
      path: ['editor', 'input_line'],
      effective: 'input_line',
      context: 'prompt',
      captured: true,
    });
    assert.equal(resolveUiFocusPath(schema, state, presence), null);
    assert.equal(predictedFocusCapture({
      localMode: PICKER_MODE.FILE,
      authoritativeActivation: { mode: PICKER_MODE.FILE, id: 1n },
    }), null);
});

check('focus path rejects missing nodes and a hidden effective node', () => {
    const root = rowNode('root', [
      focusLeafNode('editor', WIDGET.VIEW, 0, { surface: SURFACE.DOCUMENT }),
      focusLeafNode('input_line', WIDGET.TEXT_INPUT, 2),
    ]);
    const schema = schemaOf(23, root);
    const state = {
      generation: 23,
      nodes: [st('root'), st('editor'), st('input_line')],
      focus_path: ['editor', 'missing'],
    };
    assert.equal(
      resolveUiFocusPath(schema, state, presenceForSchema(23, root)), null);
    state.focus_path = ['editor', 'input_line'];
    assert.equal(
      resolveUiFocusPath(
        schema, state, presenceForSchema(23, root, ['input_line'])), null);
});

check('focus path rejects a directly present endpoint under a hidden ancestor', () => {
    const root = rowNode('root', [
      focusLeafNode('editor', WIDGET.VIEW, 0, {
        surface: SURFACE.DOCUMENT,
      }),
      rowNode('prompt.host', [
        focusLeafNode('input_line', WIDGET.TEXT_INPUT, 2),
      ]),
    ]);
    const schema = schemaOf(24, root);
    const state = {
      generation: 24,
      nodes: [st('root'), st('editor'), st('prompt.host'), st('input_line')],
      focus_path: ['editor', 'input_line'],
    };
    const presence = presenceForSchema(24, root, ['prompt.host']);
    assert.equal(
      Boolean(presence.nodes.find(
        (node) => node.id === 'input_line').present), true);
    assert.equal(resolveUiFocusPath(schema, state, presence), null);
});

check('shared C++ focus fixtures resolve every browser keymap context', () => {
    const fixtures = [
      ['session_focus_editor.hex', 'editor'],
      ['session_focus_panel.hex', 'panel'],
      ['session_focus_prompt.hex', 'prompt'],
      ['session_focus_external.hex', 'external'],
    ];
    for (const [name, expected] of fixtures) {
      const sections = findSections(fixtureMessage(name));
      assert.ok(sections);
      const frame = sections.ui_frame;
      assert.equal(resolveUiFocusPath(
        frame.schema, frame.state, frame.presence).context, expected);
    }
});

check('focusUiNode always prevents scroll', () => {
    let options = null;
    const node = {
      tabIndex: 0,
      focus(value) { options = value; },
    };
    assert.equal(focusUiNode('input_line', (id) =>
      id === 'input_line' ? node : null), true);
    assert.deepEqual(options, { preventScroll: true });
    assert.equal(node.tabIndex, -1);
    assert.equal(focusUiNode('missing', () => null), false);
});

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
  const root = rowNode('root', [leafNode('v', WIDGET.VIEW, { surface: SURFACE.TREE })]);
  assert.deepEqual(firstUnsupportedPrimitive(schemaOf(1, root), profile), { kind: 'surface', ordinal: SURFACE.TREE });
});

check('firstUnsupportedPrimitive accepts a View whose surface the default profile declares', () => {
  const root = rowNode('root', [leafNode('v', WIDGET.VIEW, { surface: SURFACE.TREE })]);
  assert.equal(firstUnsupportedPrimitive(schemaOf(1, root)), null);
});

check('firstUnsupportedPrimitive accepts the generic tree under the default profile', () => {
  const root = rowNode('root', [leafNode('tree', WIDGET.VIEW, { surface: SURFACE.TREE })]);
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
      scrollContainer('panel', SCROLL.VERTICAL, [leafNode('tree', WIDGET.VIEW, { surface: SURFACE.TREE })]),
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
  assert.equal(byId.tree.scroll ?? SCROLL.NONE, SCROLL.NONE);
  assert.equal(byId.document.scroll ?? SCROLL.NONE, SCROLL.NONE);
  assert.equal(byId.body.scroll, SCROLL.NONE);
  assert.equal(byId.future.scroll, SCROLL.NONE);  // unknown axis -> none
});

check('interpretChrome resolves semantic style per channel and preserves widget foreground precedence', () => {
  const inherited = leafNode('inherited', WIDGET.FIELD);
  const overridden = leafNode(
    'overridden', WIDGET.FIELD, { role: 'footer' });
  const group = rowNode('group', [inherited, overridden]);
  const root = rowNode('root', [group]);
  root.style = { foreground: 10, background: 1 };
  group.style = { background: 4 };
  const state = {
    generation: 22,
    nodes: [
      st('root'),
      st('group'),
      st('inherited', { value: 'base', label: 'base', role: 11 }),
      st('overridden', { value: 'override', label: 'override', role: 11 }),
    ],
  };

  const out = interpretChrome(
    schemaOf(22, root), state, presenceForSchema(22, root));
  assert.ok(out);
  assert.deepEqual(out.root.style, { foreground: 10, background: 1 });
  assert.deepEqual(out.root.children[0].style,
    { foreground: 10, background: 4 });
  assert.deepEqual(out.root.children[0].children[0].style,
    { foreground: 10, background: 4 });
  assert.equal(out.root.children[0].children[0].role, null);
  assert.equal(out.root.children[0].children[1].role, 11);
});

check('node semantic style uses widget foreground and clears removed retained channels', () => {
  const colors = Array.from({ length: 28 }, (_, i) => ({
    red: i, green: i + 1, blue: i + 2,
  }));
  const target = { color: 'stale', backgroundColor: 'stale' };

  applyNodeSemanticStyle(target, {
    style: { foreground: 10, background: 4 },
    role: 11,
  }, { role_colors: colors });
  assert.equal(target.color, cssColor(colors[11]));
  assert.equal(target.backgroundColor, cssColor(colors[4]));

  applyNodeSemanticStyle(target, { style: {} }, { role_colors: colors });
  assert.equal(target.color, '');
  assert.equal(target.backgroundColor, '');
});

check('schema style replacement clears channels on the retained render node', () => {
  const colors = Array.from({ length: 28 }, (_, i) => ({
    red: i, green: i + 1, blue: i + 2,
  }));
  const theme = { role_colors: colors };
  const styledRoot = rowNode('root', []);
  styledRoot.style = { foreground: 10, background: 4 };
  const plainRoot = rowNode('root', []);
  const state = { generation: 23, nodes: [st('root')] };
  const cache = new GenerationRetainedCache();
  cache.begin(23);

  const first = interpretChrome(
    schemaOf(23, styledRoot), state, presenceForSchema(23, styledRoot));
  const element = getOrCreateStyledNode(
    cache, first.root, theme, () => ({ style: {} }));
  assert.equal(element.style.color, cssColor(colors[10]));
  assert.equal(element.style.backgroundColor, cssColor(colors[4]));

  const second = interpretChrome(
    schemaOf(23, plainRoot), state, presenceForSchema(23, plainRoot));
  const retained = getOrCreateStyledNode(
    cache, second.root, theme, () => ({ style: {} }));
  assert.equal(retained, element);
  assert.equal(retained.style.color, '');
  assert.equal(retained.style.backgroundColor, '');
});

check('malformed node style is identified separately from a stale frame', () => {
  for (const style of [null, 'bad', { foreground: 28 },
                       { background: 'canvas' }]) {
    const root = rowNode('root', []);
    root.style = style;
    assert.deepEqual(firstMalformedNodeStyle(schemaOf(24, root)), {
      kind: 'style', id: 'root',
    });
  }
  const valid = rowNode('root', []);
  valid.style = { foreground: 0, background: 27 };
  assert.equal(firstMalformedNodeStyle(schemaOf(24, valid)), null);
});

check('picker presence overlay preserves header siblings and panel while replacing editor content', () => {
  const input = leafNode('input_line', WIDGET.TEXT_INPUT);
  const header = rowNode('header', [
    leafNode('path', WIDGET.FIELD), input, leafNode('branch', WIDGET.FIELD),
  ]);
  const panel = rowNode('panel', [
    leafNode('tree', WIDGET.VIEW, { surface: SURFACE.TREE }),
  ]);
  const editor = rowNode('editor', [
    leafNode('tabbar', WIDGET.VIEW, { surface: SURFACE.TABBAR }),
    leafNode('document', WIDGET.VIEW, { surface: SURFACE.DOCUMENT }),
  ]);
  const results = rowNode('findresults.viewport', [
    leafNode('findresults', WIDGET.VIEW, { surface: SURFACE.FINDRESULTS }),
  ]);
  const content = rowNode('content', [editor, results]);
  const root = rowNode('root', [header, rowNode('body', [panel, content])]);
  const schema = schemaOf(9, root);
  const authoritative = presenceForSchema(
    9, root, ['input_line', 'findresults.viewport']);
  const overlay = {
    generation: 9,
    ops: [
      { kind: PALETTE_PRESENCE_OP.SHOW, target: 'input_line' },
      { kind: PALETTE_PRESENCE_OP.HIDE, target: 'editor' },
      { kind: PALETTE_PRESENCE_OP.SHOW, target: 'findresults.viewport' },
    ],
  };
  const applied = applyPalettePresenceOverlay(
    schema, authoritative, overlay);
  assert.equal(applied.error, null);
  assert.equal(applied.stale, false);
  assert.notEqual(applied.presence, authoritative);
  assert.equal(
    authoritative.nodes.find((record) => record.id === 'input_line').present,
    0);

  const nodes = [];
  const collect = (node) => {
    const resolved =
      node.id === 'path' || node.id === 'branch'
        ? { value: node.id, label: node.id }
        : null;
    nodes.push(st(node.id, resolved));
    if (node.container) node.container.children.forEach(collect);
  };
  collect(root);
  const rendered = interpretChrome(
    schema, { generation: 9, nodes }, applied.presence);
  const ids = drawnLeaves(rendered.root).map((node) => node.id);
  assert.deepEqual(ids, ['path', 'input_line', 'branch', 'tree', 'findresults']);

  const twice = applyPalettePresenceOverlay(
    schema, applied.presence, overlay);
  assert.deepEqual(twice.presence, applied.presence);
  assert.equal(
    applyPalettePresenceOverlay(
      schema, authoritative, { ...overlay, generation: 10 }).stale,
    true);
  assert.match(
    applyPalettePresenceOverlay(schema, authoritative, {
      ...overlay,
      ops: [{ kind: PALETTE_PRESENCE_OP.SHOW, target: 'missing' }],
    }).error,
    /invalid/);
  assert.match(
    applyPalettePresenceOverlay(schema, authoritative, {
      ...overlay,
      ops: [
        { kind: PALETTE_PRESENCE_OP.SHOW, target: 'input_line' },
        { kind: PALETTE_PRESENCE_OP.HIDE, target: 'input_line' },
      ],
    }).error,
    /invalid/);
  assert.match(
    applyPalettePresenceOverlay(schema, authoritative, {
      ...overlay, ops: [{ kind: 99, target: 'input_line' }],
    }).error,
    /invalid/);
  assert.match(
    applyPalettePresenceOverlay(schema, authoritative, {
      ...overlay,
      ops: [
        { kind: PALETTE_PRESENCE_OP.HIDE, target: 'header' },
        { kind: PALETTE_PRESENCE_OP.SHOW, target: 'input_line' },
      ],
    }).error,
    /contradictory/);
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

check('interpretChrome produces View leaves for every supported surface', () => {
  const surfaces = [
    SURFACE.TABBAR, SURFACE.TREE, SURFACE.FINDRESULTS, SURFACE.NOTICE,
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
    SURFACE.DOCUMENT, null, SURFACE.DOCUMENT,
  ];
  const root = rowNode(
    'root', ids.map((id, i) => surfaces[i] == null
      ? rowNode(id, [])
      : leafNode(id, WIDGET.VIEW, { surface: surfaces[i] })));
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
    ui_frame_delta: null,
    prompt_status: { changed: false, replacement: null },
    syntax: { spans: null },
  }), {
    rebuild: false, reconcile: false, responsive: false, repaintTheme: false,
    surfaces: [],
  });
  assert.deepEqual(browserRenderPlan({ selection: { replacement: {} } }), {
    rebuild: false, reconcile: false, responsive: false, repaintTheme: false,
    surfaces: [SURFACE.DOCUMENT],
  });
  assert.deepEqual(browserRenderPlan({
    tree: { base_revision: 1n, revision: 2n, providers: [] },
  }), {
    rebuild: false, reconcile: false, responsive: false, repaintTheme: false,
    surfaces: [SURFACE.TREE],
  });
  assert.deepEqual(browserRenderPlan({
    ui_frame_delta: {
      kind: 'changes', presence: { nodes: [{ id: 'panel', present: false }] },
    },
  }), {
    rebuild: false, reconcile: true, responsive: true, repaintTheme: false,
    surfaces: [],
  });
  assert.deepEqual(browserRenderPlan({ palette: {} }), {
    rebuild: false, reconcile: false, responsive: false, repaintTheme: false,
    surfaces: [SURFACE.FINDRESULTS],
  });
  assert.deepEqual(browserRenderPlan({ theme: { replacement: {} } }), {
    rebuild: false, reconcile: true, responsive: false, repaintTheme: true,
    surfaces: [
      SURFACE.TABBAR, SURFACE.FINDRESULTS, SURFACE.NOTICE,
      SURFACE.EXTERNAL_MODIFICATION, SURFACE.DOCUMENT, SURFACE.TREE,
    ],
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
  assert.equal(preferredKeyboardSurface([SURFACE.TREE]), null);
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

check('responsive child selection preserves floors and drops optional children last-first', () => {
  const optional = (id, minimum, preferred) => ({
    id, size: { kind: SIZE.RESPONSIVE, minimum, extent: preferred,
                growth: 0, optional: true },
  });
  const required = (id, minimum) => ({
    id, size: { kind: SIZE.RESPONSIVE, minimum, extent: minimum,
                growth: 1, optional: false },
  });
  const children = [optional('left', 5, 15), required('content', 10),
                    optional('right', 5, 15)];
  assert.deepEqual([...responsiveSurvivors(children, 100, 2)],
                   ['left', 'content', 'right']);
  assert.deepEqual([...responsiveSurvivors(children, 35, 2)],
                   ['left', 'content', 'right']);
  assert.deepEqual([...responsiveSurvivors(children, 17, 2)],
                   ['left', 'content']);
  assert.deepEqual([...responsiveSurvivors(children, 10, 2)], ['content']);
  assert.equal(responsiveSurvivors(
    [required('a', 10), required('b', 10)], 19, 0), null);
  assert.deepEqual(
    responsiveFlexCss(optional('panel', 12, 24).size, AXIS.ROW),
    { flex: '0 0.5 24ch', minimumProperty: 'minWidth',
      minimumValue: '12ch' });
  assert.deepEqual(
    responsiveFlexCss(required('content', 20).size, AXIS.ROW),
    { flex: '1 0 20ch', minimumProperty: 'minWidth',
      minimumValue: '20ch' });
});

check('UI frame fixture retains responsive panel and content sizes', () => {
  const sections = findSections(fixtureMessage('session_semantic_base.hex'));
  const body = sections.ui_frame.schema.root.container.children.find(
    (node) => node.id === 'body');
  const panel = body.container.children.find((node) => node.id === 'panel');
  const content = body.container.children.find((node) => node.id === 'content');
  assert.deepEqual(panel.size, {
    kind: 3n, extent: 24n, minimum: 12n, growth: 0n, optional: true,
  });
  assert.deepEqual(content.size, {
    kind: 3n, extent: 20n, minimum: 20n, growth: 1n, optional: false,
  });
});

check('interpretChrome rejects unknown and malformed responsive sizes', () => {
  const make = (size) => {
    const root = rowNode('root', [leafNode('a', WIDGET.FIELD)]);
    root.size = size;
    return interpretChrome(
      schemaOf(31, root),
      { generation: 31, nodes: [st('root'), st('a', { value: 'a', label: 'a' })] },
      presenceForSchema(31, root));
  };
  const unknown = rowNode('unknown', []);
  unknown.size = { kind: 99, extent: 0 };
  assert.deepEqual(
    firstUnsupportedPrimitive(schemaOf(31, unknown)),
    { kind: 'size', ordinal: 99 });
  assert.equal(make({ kind: 99, extent: 0 }), null);
  assert.equal(make({ kind: SIZE.RESPONSIVE, extent: 20, minimum: 10,
                      growth: 1, optional: true }), null);
  assert.ok(make({ kind: SIZE.RESPONSIVE, extent: 20, minimum: 10,
                   growth: 0, optional: true }));
  const mixed = rowNode('mixed', [
    { ...leafNode('responsive', WIDGET.FIELD),
      size: { kind: SIZE.RESPONSIVE, extent: 20, minimum: 10,
              growth: 0, optional: true } },
    { ...leafNode('auto', WIDGET.FIELD), size: { kind: SIZE.AUTO } },
  ]);
  assert.equal(interpretChrome(
    schemaOf(32, mixed),
    { generation: 32, nodes: [
      st('mixed'), st('responsive', { value: 'r', label: 'r' }),
      st('auto', { value: 'a', label: 'a' }),
    ] },
    presenceForSchema(32, mixed)), null);
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

check('interpretChrome rejects a stateful TextInput without typed active state', () => {
  const root = rowNode('root', [leafNode('input_line', WIDGET.TEXT_INPUT)]);
  const state = { generation: 12, nodes: [st('root'), st('input_line', { value: 'leaked' })] };
  assert.equal(interpretChrome(schemaOf(12, root), state, presenceForSchema(12, root)), null);
});

check('interpretChrome preserves footer prompt column and options row', () => {
  const input = leafNode('footer.prompt.control.query', WIDGET.TEXT_INPUT,
    { id: 'find.query', role: 'prompt' });
  const toggle = leafNode('footer.prompt.control.case', WIDGET.CHECKBOX,
    { id: 'find.toggle_case', role: 'prompt' });
  const count = leafNode('footer.prompt.control.matches', WIDGET.LABEL,
    { id: 'find.matches', role: 'prompt' });
  const options = {
    id: 'footer.prompt.options', size: { kind: SIZE.EXACT, extent: 1 },
    container: { axis: 0, gap: 0, children: [toggle, count] },
  };
  const root = {
    id: 'footer.prompt', size: { kind: SIZE.AUTO },
    container: { axis: 1, gap: 0, children: [input, options] },
  };
  const state = { generation: 13, nodes: [
    st('footer.prompt'),
    st('footer.prompt.control.query',
      { value: 'needle', label: 'Find text', active: true, role: 16 }),
    st('footer.prompt.options'),
    st('footer.prompt.control.case',
      { value: 'Case', label: 'Case', checked: false, role: 16 }),
    st('footer.prompt.control.matches',
      { value: '1/3', label: 'Matches', role: 16 }),
  ] };
  const out = interpretChrome(
    schemaOf(13, root), state, presenceForSchema(13, root));
  assert.ok(out);
  assert.equal(out.root.axis, 1);
  assert.equal(out.root.children[1].axis, 0);
  assert.deepEqual(out.root.children[1].children.map((node) => node.id),
                   ['footer.prompt.control.case',
                    'footer.prompt.control.matches']);
  assert.equal(out.root.children[0].controlId, 'find.query');
  assert.equal(out.root.children[0].active, true);
});

check('interpretChrome preserves ordinary status action node identity and command', () => {
  const actionId = 'footer.status_action/77/9/7265747279';
  const action = leafNode(actionId, WIDGET.FIELD, {
    id: actionId, role: 'status_info',
  });
  const root = rowNode('footer.status_actions', [action]);
  const state = { generation: 14, nodes: [
    st('footer.status_actions'),
    st(actionId, {
      value: 'Retry', label: 'Retry', command: 'build.retry', role: 12,
    }),
  ] };
  const out = interpretChrome(
    schemaOf(14, root), state, presenceForSchema(14, root));
  assert.ok(out);
  assert.equal(out.root.children[0].id, actionId);
  assert.equal(out.root.children[0].text, 'Retry');
  assert.equal(out.root.children[0].label, 'Retry');
  assert.equal(out.root.children[0].command, 'build.retry');
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

check('applySessionDeltaSections opens, changes, and CLOSES the footer prompt view', () => {
  const sections = { document: { text: '' }, prompt_view: null };
  const promptView = { kind: 1, active_input: 0, controls: [] };
  // changed=true with a replacement opens/updates it.
  applySessionDeltaSections(
    sections, { prompt_view: { changed: 1, replacement: promptView } });
  assert.ok(sections.prompt_view);
  assert.equal(sections.prompt_view.active_input, 0);
  // changed=false leaves the prior view intact (no spurious close).
  applySessionDeltaSections(sections, { prompt_view: { changed: 0 } });
  assert.ok(sections.prompt_view);
  // changed=true with a null replacement CLOSES it (replaceWrapped's non-null
  // guard would wrongly keep it -- this is why the delta is changed-flagged).
  applySessionDeltaSections(sections, { prompt_view: { changed: 1, replacement: null } });
  assert.equal(sections.prompt_view, null);
});

check('applySessionDeltaCopy retains unchanged large sections for a caret update', () => {
  const document = { text: 'large document', caret: 0 };
  const syntax = { spans: [{ begin: 0, end: 14, scope: 1 }] };
  const palette = { command_candidates: [{ id: 'command' }] };
  const sections = { document, syntax, palette, selection: { selections: [] } };
  const next = applySessionDeltaCopy(sections, { document_caret: 4 });
  assert.notEqual(next, sections);
  assert.notEqual(next.document, document);
  assert.equal(next.document.text, document.text);
  assert.equal(next.syntax, syntax);
  assert.equal(next.palette, palette);
});

check('UI frame replay restores hidden focus only in one atomic change', () => {
  const schema = schemaOf(4, rowNode('root', [
    focusLeafNode('editor', WIDGET.VIEW, 0),
    focusLeafNode('input_line', WIDGET.TEXT_INPUT, 2),
  ]));
  const frame = {
    version: { generation: 4n, presence_basis: 2n },
    schema,
    state: {
      generation: 4n,
      nodes: [st('root'), st('editor'), st('input_line')],
      focus_path: ['editor', 'input_line'],
    },
    presence: {
      generation: 4n, basis: 2n,
      nodes: [
        { id: 'root', present: true },
        { id: 'editor', present: false },
        { id: 'input_line', present: true },
      ],
    },
  };
  const restoration = {
    base: frame.version,
    target: { generation: 4n, presence_basis: 3n },
    kind: 'changes',
    state: {
      generation: 4n, nodes: [], focus_path: ['editor'],
    },
    presence: {
      generation: 4n, basis: 3n,
      nodes: [{ id: 'editor', present: true }],
    },
  };
  const restored = applyUiFrameDelta(frame, restoration);
  assert.ok(restored);
  assert.deepEqual(restored.state.focus_path, ['editor']);
  assert.equal(restored.presence.nodes[1].present, true);

  const invalidPop = structuredClone(restoration);
  invalidPop.target.presence_basis = 2n;
  invalidPop.presence.basis = 2n;
  invalidPop.presence.nodes = [];
  assert.equal(applyUiFrameDelta(frame, invalidPop), null);
  assert.deepEqual(frame.state.focus_path, ['editor', 'input_line']);
  assert.equal(frame.presence.nodes[1].present, false);
});

check('preceding focus-only delta becomes the authoritative frame path', () => {
  const base = findSections(fixtureMessage('session_focus_editor.hex'));
  delete base.focus;
  delete base.external_focus_held;
  const replayed = applySessionDeltaCopy(base, { focus: 1n });
  assert.ok(replayed);
  assert.equal(replayed.focus, undefined);
  assert.equal(replayed.external_focus_held, undefined);
  assert.equal(resolveUiFocusPath(
    replayed.ui_frame.schema, replayed.ui_frame.state,
    replayed.ui_frame.presence).context, 'panel');
});

check('browser rejects focus compatibility forms rejected by C++', () => {
  const fixture = () =>
    structuredClone(findSections(fixtureMessage('session_focus_editor.hex')));

  const promptBase = fixture();
  promptBase.ui_frame.state.focus_path = ['input_line'];
  promptBase.focus = 2n;
  assert.equal(findSections(promptBase), null);

  const missingFocus = fixture();
  delete missingFocus.focus;
  assert.equal(findSections(missingFocus), null);

  const malformedSnapshotBool = fixture();
  malformedSnapshotBool.external_focus_held = 2n;
  assert.equal(findSections(malformedSnapshotBool), null);

  const base = fixture();
  delete base.focus;
  delete base.external_focus_held;
  assert.equal(
    applySessionDeltaCopy(base, { external_focus_held: 2n }), null);
});

check('session delta replay permits switching to an older document revision', () => {
  const sections = {
    document: {
      revision: 2n, text: 'dirty scratch', caret: 13n,
      diff_file_identity: null,
    },
  };
  const delta = {
    base_revision: 10n,
    revision: 11n,
    document: {
      base_revision: 2n,
      revision: 1n,
      start: 0n,
      erased_bytes: 13n,
      inserted_text: 'older buffer',
      diff_file_identity: null,
    },
    document_caret: 0n,
  };
  const replayed = applySessionDelta(sections, 10n, delta);
  assert.equal(replayed.kind, 'accepted');
  assert.deepEqual(replayed.sections.document, {
    revision: 1n, text: 'older buffer', caret: 0n,
    diff_file_identity: null,
  });

  assert.equal(
    applySessionDelta(sections, 9n, delta).kind, 'revision-gap');
  const malformed = structuredClone(delta);
  malformed.document.revision = 2n;
  assert.equal(
    applySessionDelta(sections, 10n, malformed).kind,
    'semantic-rejection');
});

check('semantic manifest and C++ fixture replay every browser section atomically', () => {
  const manifest = JSON.parse(fs.readFileSync(
    new URL('../fixtures/protocol/session_semantic_fields.json', import.meta.url),
    'utf8'));
  const base = findSections(fixtureMessage('session_semantic_base.hex'));
  const target = findSections(fixtureMessage('session_semantic_target.hex'));
  const delta = fixtureMessage('session_semantic_delta.hex');
  assert.deepEqual(
    manifest.map((entry) => entry.snapshot), Object.keys(base));
  for (const { snapshot } of manifest) {
    assert.notDeepEqual(base[snapshot], target[snapshot],
      snapshot + ' fixture must independently change');
  }
  const replayed = applySessionDeltaCopy(base, delta);
  assert.ok(replayed);
  delete target.focus;
  delete target.external_focus_held;
  assert.equal(normalizeTreeActiveBinding(target.tree), true);
  assert.deepEqual(replayed, target);

  const rejectsWithoutMutation = (mutate) => {
    const malformed = structuredClone(delta);
    mutate(malformed);
    const retained = structuredClone(base);
    assert.equal(applySessionDeltaCopy(retained, malformed), null);
    assert.deepEqual(retained, base);
  };
  for (const name of [
    'document', 'search', 'diff', 'external_modification', 'tree',
    'lsp_sync', 'lsp_features',
  ]) {
    rejectsWithoutMutation((malformed) => {
      malformed[name].base_revision = 999n;
    });
  }
  rejectsWithoutMutation((malformed) => {
    malformed.settings.changes[0].before.value.value = 999n;
  });
  rejectsWithoutMutation((malformed) => {
    malformed.text_encoding.before.status.encoding = 999n;
  });
  rejectsWithoutMutation((malformed) => {
    malformed.selection.changed = 0n;
  });
  rejectsWithoutMutation((malformed) => {
    malformed.syntax.spans = [{ begin: 1n, end: 7n, scope: 0n }];
  });
  rejectsWithoutMutation((malformed) => {
    malformed.ui_frame_delta.base.presence_basis = 999n;
  });
  rejectsWithoutMutation((malformed) => {
    delete malformed.ui_frame_delta.frame;
  });
  rejectsWithoutMutation((malformed) => {
    malformed.focus = 1n;
  });
});

check('mergeBrowserRenderPlans preserves every dirty surface and strongest work', () => {
  assert.deepEqual(
    mergeBrowserRenderPlans(
      { rebuild: false, reconcile: true, repaintTheme: false, surfaces: [8] },
      { rebuild: true, reconcile: false, repaintTheme: true, surfaces: [3, 8] }),
    { rebuild: true, reconcile: true, responsive: false,
      repaintTheme: true, surfaces: [8, 3] });
});

check('prompt focus uses a typed revision-checked semantic input', () => {
  assert.deepEqual(
    decodeMessage(
      encodePromptControlPointerInput('replace.replacement', 7n).buffer).payload,
    { kind: 4n, button: 0n, phase: 0n, basis_revision: 7n,
      control_id: 'replace.replacement' });
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
    decodeMessage(
      encodeExternalActionPointerInput(
        action.action, file.id, 11n).buffer).payload,
    { kind: 5n, button: 0n, phase: 0n, basis_revision: 11n,
      invocation: { file_id: 'external:src/a:b.cpp', action: 0n } });
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