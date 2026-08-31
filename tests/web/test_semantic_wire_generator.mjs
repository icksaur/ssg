import assert from 'node:assert/strict';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import manifest from '../../protocol/schema/semantic_wire.mjs';
import {
  checkOutputs,
  renderOutputs,
  run,
  validateManifest,
  writeOutputs,
} from '../../protocol/schema/generate_semantic_wire.mjs';

let checks = 0;
const check = async (name, test) => {
  await test();
  checks++;
};

const temporary = await fsp.mkdtemp(
  path.join(os.tmpdir(), 'ssg-semantic-wire-'));
try {
  await check('rendering is deterministic and check detects drift', async () => {
    const first = renderOutputs(manifest);
    const second = renderOutputs(manifest);
    assert.deepEqual(first, second);
    const destinations = {
      cpp: path.join(temporary, 'generated', 'manifest.h'),
      uiCpp: path.join(temporary, 'generated', 'ui-schema.h'),
      js: path.join(temporary, 'generated', 'manifest.mjs'),
    };
    await writeOutputs(first, destinations);
    await checkOutputs(second, destinations);
    const generated = await import(pathToFileURL(destinations.js).href);
    assert.equal(Object.isFrozen(generated.MESSAGE_KINDS[0]), true);
    assert.equal(Object.isFrozen(generated.SEMANTIC_SECTIONS[0]), true);
    assert.equal(Object.isFrozen(generated.SEMANTIC_SECTIONS[0].delta), true);
    assert.equal(Object.isFrozen(generated.WIDGET_KIND), true);
    const leaf = {
      id: 'leaf',
      size: { kind: 0, extent: 0 },
      leaf: {
        kind: 1, id: 'leaf', rank: 0, keep: false, overflow: 0, sigil: '',
      },
    };
    const root = {
      id: 'root',
      size: { kind: 1, extent: 0 },
      container: {
        axis: 1,
        inset: { left: 0, right: 0, top: 0, bottom: 0 },
        gap: 0,
        children: [leaf],
      },
    };
    const schema = { generation: 1, root };
    const state = {
      generation: 1,
      nodes: [
        { id: 'root', leaf: null },
        {
          id: 'leaf',
          leaf: { value: 'value', label: 'label', role: 0 },
        },
      ],
      focus_path: null,
    };
    const presence = {
      generation: 1,
      basis: 2,
      nodes: [
        { id: 'root', present: true },
        { id: 'leaf', present: true },
      ],
    };
    const frame = {
      version: { generation: 1, presence_basis: 2 },
      schema,
      state,
      presence,
    };
    assert.equal(generated.validateUiFrameWire(frame), true);
    assert.equal(generated.validateUiFrameWire({
      ...frame, version: { ...frame.version, generation: '1' },
    }), false);
    assert.equal(generated.validateUiFrameWire({
      ...frame,
      version: {
        generation: 18446744073709551615n,
        presence_basis: 9007199254740992n,
      },
    }), true);
    assert.equal(generated.validateUiFrameWire({
      ...frame,
      version: {
        generation: 18446744073709551616n,
        presence_basis: 2,
      },
    }), false);
    assert.equal(generated.validateUiFrameWire({
      ...frame, state: { ...state, nodes: [{ id: 'root', leaf: 7 }] },
    }), false);
    assert.equal(generated.validateUiSchemaWire({
      ...schema,
      root: { ...root, leaf: leaf.leaf },
    }), false);
    assert.equal(generated.validateUiSchemaWire({
      ...schema,
      root: { ...root, size: { kind: '0', extent: 0 } },
    }), false);
    assert.equal(generated.validateUiSchemaWire({
      ...schema,
      root: { ...root, size: { kind: true, extent: 0 } },
    }), false);
    assert.equal(generated.validateUiSchemaWire({
      ...schema,
      root: {
        ...root,
        container: { ...root.container, scroll: 'vertical' },
      },
    }), false);
    assert.equal(generated.validateUiSchemaWire({
      ...schema,
      root: {
        ...root,
        container: { ...root.container, scroll: 99 },
      },
    }), true);
    assert.equal(generated.validateUiFrameDeltaWire({
      base: frame.version,
      target: frame.version,
      kind: 'replacement',
      frame,
    }), true);
    assert.equal(generated.validateUiFrameDeltaWire({
      base: frame.version,
      target: frame.version,
      kind: 'changes',
      state,
    }), false);
    assert.equal(generated.validateUiFrameDeltaWire({
      base: frame.version,
      target: frame.version,
      kind: 'changes',
      state,
      presence,
    }), true);
    assert.equal(generated.validateUiStateSectionWire({
      ...state,
      nodes: [{ id: 'root', leaf: { value: 1, label: 'x', role: 0 } }],
    }), false);
    assert.equal(generated.validateUiPresenceSectionWire({
      ...presence,
      nodes: [{ id: 'root', present: 1 }],
    }), false);
    const stroke = generated.buildKeyStrokeWire(
      'KeyA', true, false, false, true);
    const keyInput = generated.buildClientInputKeyWire(stroke, 'a');
    assert.equal(generated.validateClientInputWire(keyInput), true);
    assert.equal(generated.validateClientInputWire(
      { ...keyInput, unexpected: 1n }), false);
    assert.equal(generated.validateClientInputWire(
      { ...keyInput, kind: '0' }), false);
    const documentInput = generated.buildClientInputDocumentWire(
      0, 1, 7, null, false, false, 0);
    assert.equal(generated.validateClientInputWire(documentInput), true);
    const externalInvocation = { file_id: 'file', action: 0n };
    assert.equal(
      generated.validateExternalActionInvocationWire(externalInvocation), true);
    assert.equal(generated.validateExternalActionInvocationWire(
      { ...externalInvocation, extra: true }), false);
    const resolved = generated.buildClientInputResolvedSelectionWire(
      9, 3, 8, [{ anchor: 2, active: 4 }]);
    assert.equal(generated.validateClientInputWire(resolved), true);
    assert.equal(generated.validateViewActionWire({
      kind: 0n, target: 0n, rows: -9223372036854775808n,
    }), true);
    assert.equal(generated.validateViewActionWire({
      kind: 0n, target: 0n, rows: 9223372036854775807n,
    }), true);
    assert.equal(generated.validateViewActionWire({
      kind: 0n, target: 0n, rows: -9223372036854775809n,
    }), false);
    assert.equal(generated.validateViewActionWire({
      kind: 2n, target: 0n,
      numerator: 4294967295n, denominator: 4294967295n,
    }), true);
    assert.equal(generated.validateViewActionWire({
      kind: 2n, target: 0n, numerator: 0n, denominator: 4294967296n,
    }), false);
    const command = {
      error: 0n, revision: 4n, message: '',
      routingChanged: 1n, geometryChanged: 0n,
    };
    assert.equal(generated.validateCommandResultWire(command), true);
    assert.equal(generated.validateCommandResultWire(
      { ...command, routingChanged: 2n }), false);
    assert.equal(generated.validateCommandResultWire(
      { ...command, routingChanged: 4294967295n }), false);
    assert.equal(generated.validateClientInputResultWire({
      outcome: 0n,
      client_owned: null,
      command,
      picker_activation: null,
    }), true);
    await fsp.appendFile(destinations.js, '// stale\n');
    await assert.rejects(
      checkOutputs(second, destinations), /output is stale/);
  });

  await check('invalid declarations reject before rendering', async () => {
    const duplicate = structuredClone(manifest);
    duplicate.messageKinds[1].ordinal = duplicate.messageKinds[0].ordinal;
    assert.throws(() => validateManifest(duplicate), /duplicate message ordinal/);

    const retiredSection = structuredClone(manifest);
    retiredSection.semanticSections[0].lifecycle = 'retired';
    assert.throws(
      () => validateManifest(retiredSection), /invalid section declaration/);

    const mismatchedCompatibility = structuredClone(manifest);
    mismatchedCompatibility.semanticSections[0].lifecycle = 'compatibility';
    assert.throws(
      () => validateManifest(mismatchedCompatibility),
      /invalid section declaration/);

    const invalidBytes = structuredClone(manifest);
    invalidBytes.wireTypes.find(
      (wireType) => wireType.symbol === 'UntitledDocumentId').schema.length = 0;
    assert.throws(
      () => validateManifest(invalidBytes), /invalid wire bytes length/);

    const invalidArrayLength = structuredClone(manifest);
    invalidArrayLength.wireTypes.find(
      (wireType) => wireType.symbol === 'ThemeSnapshot')
      .schema.fields[0].type.length = -1;
    assert.throws(
      () => validateManifest(invalidArrayLength), /invalid wire array length/);

    const invalidManifest = path.join(temporary, 'invalid-manifest.mjs');
    const invalidDestinations = path.join(temporary, 'invalid-output');
    await fsp.writeFile(invalidManifest, `
export default {
  messageKinds: [
    { symbol: 'Same', wireName: 'first', ordinal: 0, lifecycle: 'current' },
    { symbol: 'Same', wireName: 'second', ordinal: 1, lifecycle: 'current' },
  ],
  semanticSections: [],
  wireEnums: [],
  wireTypes: [],
};
`);
    await assert.rejects(run([
      '--write',
      '--manifest', invalidManifest,
      '--cpp', path.join(invalidDestinations, 'manifest.h'),
      '--ui-cpp', path.join(invalidDestinations, 'ui-schema.h'),
      '--js', path.join(invalidDestinations, 'manifest.mjs'),
    ]), /duplicate message symbol/);
    await assert.rejects(fsp.access(invalidDestinations), { code: 'ENOENT' });
  });

  await check('wire type declarations reject ambiguous recursion and drift', () => {
    assert.ok(Array.isArray(manifest.wireTypes));
    assert.ok(manifest.wireTypes.length > 0);

    const duplicateType = structuredClone(manifest);
    duplicateType.wireTypes.push(structuredClone(duplicateType.wireTypes[0]));
    assert.throws(
      () => validateManifest(duplicateType), /duplicate wire type symbol/);

    const unknownReference = structuredClone(manifest);
    unknownReference.wireTypes[0].schema = { kind: 'ref', type: 'Missing' };
    assert.throws(
      () => validateManifest(unknownReference), /invalid wire type reference/);

    const undeclaredRecursion = structuredClone(manifest);
    const recursive = undeclaredRecursion.wireTypes.find(
      (wireType) => wireType.symbol === 'UiNode');
    recursive.schema.variants[0].type.fields
      .find((field) => field.wireName === 'children')
      .type.items.recursive = false;
    assert.throws(
      () => validateManifest(undeclaredRecursion), /undeclared recursive wire type/);

    const multiTypeCycle = structuredClone(manifest);
    multiTypeCycle.wireTypes = [
      { symbol: 'First', schema: { kind: 'ref', type: 'Second' } },
      { symbol: 'Second', schema: { kind: 'ref', type: 'First' } },
    ];
    assert.throws(
      () => validateManifest(multiTypeCycle), /multi-type wire cycle/);

    const duplicateField = structuredClone(manifest);
    const duplicateNode = duplicateField.wireTypes.find(
      (wireType) => wireType.symbol === 'UiNode');
    duplicateNode.schema.fields.push(
      structuredClone(duplicateNode.schema.fields[0]));
    assert.throws(
      () => validateManifest(duplicateField), /duplicate wire type field/);

    const implicitPresence = structuredClone(manifest);
    const implicitNode = implicitPresence.wireTypes.find(
      (wireType) => wireType.symbol === 'UiNode');
    delete implicitNode.schema.fields[0].required;
    assert.throws(
      () => validateManifest(implicitPresence), /invalid wire type field/);

    const invalidUnknownPolicy = structuredClone(manifest);
    invalidUnknownPolicy.wireTypes.find(
      (wireType) => wireType.symbol === 'UiNode').schema.unknownFields = 'ignore';
    assert.throws(
      () => validateManifest(invalidUnknownPolicy), /invalid unknown-field policy/);

    const unknownPrimitiveProperty = structuredClone(manifest);
    unknownPrimitiveProperty.wireTypes.find(
      (wireType) => wireType.symbol === 'UiNode').schema.fields[1]
      .type.fields[0].type.extra = true;
    assert.throws(
      () => validateManifest(unknownPrimitiveProperty),
      /unknown wire primitive property/);

    const broadenedEnumFallback = structuredClone(manifest);
    broadenedEnumFallback.wireTypes.find(
      (wireType) => wireType.symbol === 'UiNode')
      .schema.fields[3].type.acceptUnknown = true;
    assert.throws(
      () => validateManifest(broadenedEnumFallback), /invalid wire type enum/);

    const invalidUnion = structuredClone(manifest);
    invalidUnion.wireTypes.find(
      (wireType) => wireType.symbol === 'UiNode').schema.variants.length = 1;
    assert.throws(
      () => validateManifest(invalidUnion), /invalid wire field union/);

    const optionalExactField = structuredClone(manifest);
    optionalExactField.wireTypes.find(
      (wireType) => wireType.symbol === 'ExternalActionInvocation')
      .schema.fields[0].required = false;
    assert.throws(
      () => validateManifest(optionalExactField),
      /optional field in exact wire record/);
  });

  await check('wire enum declarations reject drift and invalid fallbacks', () => {
    assert.ok(Array.isArray(manifest.wireEnums));
    assert.ok(manifest.wireEnums.length > 0);

    const duplicateOrdinal = structuredClone(manifest);
    duplicateOrdinal.wireEnums[0].values[1].ordinal =
      duplicateOrdinal.wireEnums[0].values[0].ordinal;
    assert.throws(
      () => validateManifest(duplicateOrdinal), /duplicate wire enum ordinal/);

    const invalidFallback = structuredClone(manifest);
    invalidFallback.wireEnums[0].unknown = {
      policy: 'map-to',
      fallback: 'NotAValue',
    };
    assert.throws(
      () => validateManifest(invalidFallback), /invalid wire enum fallback/);

    const broadenedFallback = structuredClone(manifest);
    broadenedFallback.wireEnums[0].unknown = {
      policy: 'map-to',
      fallback: broadenedFallback.wireEnums[0].values[0].symbol,
    };
    assert.throws(
      () => validateManifest(broadenedFallback), /invalid wire enum fallback/);

    const reservedExport = structuredClone(manifest);
    reservedExport.wireEnums[0].jsName = 'MESSAGE_KINDS';
    assert.throws(
      () => validateManifest(reservedExport),
      /reserved wire enum JavaScript name/);
  });

  await check('retired message reservations remain explicit and sparse', async () => {
    assert.deepEqual(
      manifest.messageKinds
        .filter((message) => message.lifecycle === 'retired')
        .map(({ symbol, wireName, ordinal }) => [symbol, wireName, ordinal]),
      [
        ['ClipboardRequest', 'clipboard_request', 3],
        ['ClipboardResponse', 'clipboard_response', 4],
        ['StatusActionInvocation', 'status_action_invocation', 5],
      ]);
  });

  await check('sparse enum ordinals and retired surfaces stay reserved', () => {
    const viewSurface = manifest.wireEnums.find(
      (wireEnum) => wireEnum.symbol === 'ViewSurface');
    assert.deepEqual(
      viewSurface.values.map(
        ({ symbol, wireName, ordinal, lifecycle: state }) =>
          [symbol, wireName, ordinal, state]),
      [
        ['TabBar', 'tabbar', 0, 'current'],
        ['FileTree', 'file_tree', 1, 'retired'],
        ['GitStatus', 'git_status', 2, 'retired'],
        ['FindResults', 'findresults', 3, 'current'],
        ['Symbols', 'symbols', 4, 'retired'],
        ['FooterPrompt', 'footer_prompt', 5, 'retired'],
        ['Notice', 'notice', 6, 'current'],
        ['ExternalModification', 'external_modification', 7, 'current'],
        ['Document', 'document', 8, 'current'],
        ['Tree', 'tree', 9, 'current'],
      ]);

    const diagnosticSeverity = manifest.wireEnums.find(
      (wireEnum) => wireEnum.symbol === 'LspDiagnosticSeverity');
    assert.deepEqual(
      diagnosticSeverity.values.map(({ symbol, ordinal }) => [symbol, ordinal]),
      [['Error', 1], ['Warning', 2], ['Information', 3], ['Hint', 4]]);
  });

  await check('every wire enum identity and ordinal has an independent oracle', () => {
    const actual = manifest.wireEnums.map((wireEnum) =>
      `${wireEnum.symbol}=${wireEnum.values.map((value) =>
        `${value.symbol}/${value.wireName}/${value.ordinal}/${value.lifecycle}`)
        .join(',')}`);
    assert.deepEqual(actual, [
      'CommandError=None/none/0/current,UnknownClient/unknown_client/1/current,UnknownCommand/unknown_command/2/current,StaleRevision/stale_revision/3/current,CapabilityDenied/capability_denied/4/current,HandlerFailed/handler_failed/5/current,RevisionExhausted/revision_exhausted/6/current',
      'DocumentMode=Edit/edit/0/current,ReadOnly/read_only/1/current,Diff/diff/2/current',
      'StatusPriority=Error/error/0/current,Warning/warning/1/current,Information/information/2/current,Progress/progress/3/current',
      'PromptKind=Path/path/0/current,Find/find/1/current,Replace/replace/2/current,Settings/settings/3/current,CommandArgument/command_argument/4/current,Palette/palette/5/current',
      'PromptControlKind=Input/input/0/current,Toggle/toggle/1/current,Count/count/2/current',
      'SearchMode=File/file/0/current,Line/line/1/current,Symbol/symbol/2/current,Text/text/3/current,Command/command/4/current',
      'FindReplaceError=None/none/0/current,InvalidPattern/invalid_pattern/1/current,InvalidUtf8/invalid_utf8/2/current,InvalidSelection/invalid_selection/3/current,BudgetExhausted/budget_exhausted/4/current,Cancelled/cancelled/5/current,NoMatch/no_match/6/current,StaleRevision/stale_revision/7/current,DocumentRejected/document_rejected/8/current,WorkspaceRejected/workspace_rejected/9/current,RecoveryRejected/recovery_rejected/10/current',
      'SettingScope=Defaults/defaults/0/current,User/user/1/current,Workspace/workspace/2/current,Language/language/3/current,Document/document/4/current',
      'SettingKey=IndentWidth/indent_width/0/current,IndentStyle/indent_style/1/current,IndentDetection/indent_detection/2/current,AutoIndent/auto_indent/3/current,LineEnding/line_ending/4/current,FinalNewline/final_newline/5/current,Encoding/encoding/6/current,WordWrap/word_wrap/7/current,Theme/theme/8/current,Keymap/keymap/9/current,SearchCaseSensitive/search_case_sensitive/10/current,SearchWholeWord/search_whole_word/11/current,SearchRegularExpression/search_regular_expression/12/current,UndoByteBudget/undo_byte_budget/13/current,RecoveryByteBudget/recovery_byte_budget/14/current,TypingCoalescingMs/typing_coalescing_ms/15/current,FileFinderRespectGitignore/file_finder_respect_gitignore/16/current,AutosaveDebounceMs/autosave_debounce_ms/17/current,LineNumbers/line_numbers/18/current',
      'TextEncoding=Utf8/utf8/0/current,Utf8Bom/utf8_bom/1/current,Utf16le/utf16le/2/current,Utf16be/utf16be/3/current,Windows1252/windows1252/4/current,Iso88591/iso88591/5/current',
      'IndentStyle=Spaces/spaces/0/current,Tabs/tabs/1/current',
      'LineEnding=Lf/lf/0/current,Crlf/crlf/1/current,Cr/cr/2/current,Mixed/mixed/3/current',
      'TabKind=Document/document/0/current,LiveDiff/live_diff/1/current,ReadOnlyOutput/read_only_output/2/current,SearchResults/search_results/3/current,TreeView/tree_view/4/current',
      'TabRecoveryBadge=None/none/0/current,Pending/pending/1/current,Durable/durable/2/current,Failed/failed/3/current',
      'JournalDocumentKeyKind=Saved/saved/0/current,Untitled/untitled/1/current',
      'DiffLineKind=Added/added/0/current,Removed/removed/1/current,Modified/modified/2/current',
      'DiffFileStatus=Added/added/0/current,Modified/modified/1/current,Deleted/deleted/2/current,Renamed/renamed/3/current',
      'ExternalAction=Reload/reload/0/current,KeepBuffer/keep_buffer/1/current,OpenDiff/open_diff/2/current',
      'ExternalDocumentStatus=ExternallyModified/externally_modified/0/current,ExternallyRemoved/externally_removed/1/current',
      'FollowMode=Following/following/0/current,Paused/paused/1/current',
      'TreeProviderKind=Filesystem/filesystem/0/current,Git/git/1/current,Symbols/symbols/2/current',
      'TreeNodeKind=Root/root/0/current,Directory/directory/1/current,File/file/2/current,Symlink/symlink/3/current,GitEntry/git_entry/4/current,Symbol/symbol/5/current',
      'GitTreeStatus=Added/added/0/current,Modified/modified/1/current,Deleted/deleted/2/current,Renamed/renamed/3/current,Untracked/untracked/4/current',
      'SyntaxScope=PlainText/plain_text/0/current,Comment/comment/1/current,Keyword/keyword/2/current,String/string/3/current,Number/number/4/current,Type/type/5/current,Function/function/6/current,Variable/variable/7/current,OperatorToken/operator/8/current,Punctuation/punctuation/9/current,Invalid/invalid/10/current',
      'BracketKind=Round/round/0/current,Square/square/1/current,Curly/curly/2/current',
      'BracketRole=Open/open/0/current,Close/close/1/current',
      'CommentKind=Line/line/0/current,Block/block/1/current',
      'CommentTokenRole=Line/line/0/current,BlockOpen/block_open/1/current,BlockClose/block_close/2/current',
      'LspDiagnosticSeverity=Error/error/1/current,Warning/warning/2/current,Information/information/3/current,Hint/hint/4/current',
      'ShellNodeKind=Header/header/0/current,HeaderField/header_field/1/current,Footer/footer/2/current,FooterField/footer_field/3/current,FooterAction/footer_action/4/current,TabBar/tab_bar/5/current,Tab/tab/6/current,Panel/panel/7/current,PanelProvider/panel_provider/8/current,Pane/pane/9/current,Scrollbar/scrollbar/10/current,PromptReservation/prompt_reservation/11/current,EmptyState/empty_state/12/current,NoticeBar/notice_bar/13/current,NoticeAction/notice_action/14/current,FooterHint/footer_hint/15/current,TabSeparator/tab_separator/16/current,ExternalModificationBar/external_modification_bar/17/current,ExternalModificationRow/external_modification_row/18/current,ExternalModificationAction/external_modification_action/19/current',
      'FocusTarget=Editor/editor/0/current,Panel/panel/1/current,Prompt/prompt/2/current,ExternalModification/external_modification/3/current',
      'SemanticRole=Text/text/0/current,Canvas/canvas/1/current,Caret/caret/2/current,Selection/selection/3/current,TreeBackground/tree_background/4/current,TreeFocus/tree_focus/5/current,TabActive/tab_active/6/current,TabInactive/tab_inactive/7/current,PanelActive/panel_active/8/current,PanelInactive/panel_inactive/9/current,Header/header/10/current,Footer/footer/11/current,StatusInfo/status_info/12/current,StatusWarning/status_warning/13/current,LineNumber/line_number/14/current,SearchMatch/search_match/15/current,Prompt/prompt/16/current,ScrollbarTrack/scrollbar_track/17/current,ScrollbarThumb/scrollbar_thumb/18/current,DiffAdded/diff_added/19/current,DiffRemoved/diff_removed/20/current,DiffModified/diff_modified/21/current,TabInactiveBackground/tab_inactive_background/22/current,HeaderBackground/header_background/23/current,FooterBackground/footer_background/24/current,CurrentLineNumber/current_line_number/25/current,CurrentLineNumberBackground/current_line_number_background/26/current,LineNumberBackground/line_number_background/27/current',
      'ClientInputKind=Key/key/0/current,Tab/tab/1/current,Tree/tree/2/current,Picker/picker/3/current,PromptControl/prompt_control/4/current,ExternalAction/external_action/5/current,StatusAction/status_action/6/current,PublishedUiAction/published_ui_action/7/current,NoticeAction/notice_action/8/current,Document/document/9/current,ScrollLines/scroll_lines/10/current,ScrollFraction/scroll_fraction/11/current,ViewNavigation/view_navigation/12/current,ResolvedPaneFocus/resolved_pane_focus/13/current,ResolvedSelection/resolved_selection/14/current',
      'InputPointerButton=Primary/primary/0/current,Auxiliary/auxiliary/1/current,Secondary/secondary/2/current',
      'InputPointerPhase=Press/press/0/current,Move/move/1/current,Release/release/2/current,Cancel/cancel/3/current',
      'DocumentPointerEdge=None/none/0/current,Before/before/1/current,After/after/2/current',
      'SemanticScrollTarget=Document/document/0/current,Tree/tree/1/current',
      'ClientOwnedInputKind=AppendText/append_text/0/current,DeleteGraphemeBackward/delete_grapheme_backward/1/current,DeleteWordBackward/delete_word_backward/2/current,SelectNext/select_next/3/current,SelectPrevious/select_previous/4/current,Submit/submit/5/current',
      'ClientInputOutcome=Unhandled/unhandled/0/current,ClientOwned/client_owned/1/current,Dispatched/dispatched/2/current,Rejected/rejected/3/current,ViewOwned/view_owned/4/current',
      'WidgetKind=Container/container/0/current,Label/label/1/current,Field/field/2/current,Checkbox/checkbox/3/current,TextInput/text_input/4/current,Spacer/spacer/5/current,View/view/6/current,StatusActions/status_actions/7/current',
      'ViewSurface=TabBar/tabbar/0/current,FileTree/file_tree/1/retired,GitStatus/git_status/2/retired,FindResults/findresults/3/current,Symbols/symbols/4/retired,FooterPrompt/footer_prompt/5/retired,Notice/notice/6/current,ExternalModification/external_modification/7/current,Document/document/8/current,Tree/tree/9/current',
      'Axis=Row/row/0/current,Column/column/1/current',
      'ScrollAxis=None/none/0/current,Vertical/vertical/1/current',
      'SizeKind=Exact/exact/0/current,Flex/flex/1/current,Auto/auto/2/current,Responsive/responsive/3/current',
      'Overflow=None/none/0/current,Truncate/truncate/1/current,ScrollTail/scroll_tail/2/current',
      'ViewActionKind=ScrollLines/scroll_lines/0/current,ScrollPages/scroll_pages/1/current,ScrollFraction/scroll_fraction/2/current,MoveVisualSelection/move_visual_selection/3/current,RevealSelection/reveal_selection/4/current,CenterSelection/center_selection/5/current,SplitPane/split_pane/6/current,ClosePane/close_pane/7/current,CyclePane/cycle_pane/8/current,FocusPane/focus_pane/9/current,ContinuePointerEdge/continue_pointer_edge/10/current',
      'ViewScrollTarget=Document/document/0/current,Tree/tree/1/current',
      'VisualSelectionDirection=LineUp/line_up/0/current,LineDown/line_down/1/current,PageUp/page_up/2/current,PageDown/page_down/3/current',
      'SplitAxis=Horizontal/horizontal/0/current,Vertical/vertical/1/current',
      'PaneCycleDirection=Next/next/0/current,Previous/previous/1/current',
      'PaneDirection=Left/left/0/current,Right/right/1/current,Up/up/2/current,Down/down/3/current',
      'PointerEdgeDirection=Before/before/0/current,After/after/1/current',
      'PalettePresenceOpKind=Show/show/0/current,Hide/hide/1/current',
      'SettingValueKind=Boolean/boolean/0/current,Uint32/uint32/1/current,Uint64/uint64/2/current,IndentStyle/indent_style/3/current,LineEnding/line_ending/4/current,TextEncoding/text_encoding/5/current,Text/text/6/current',
    ]);
  });

  await check('lifecycle metadata stays outside runtime consumers', async () => {
    const root = path.resolve(
      path.dirname(fileURLToPath(import.meta.url)), '../..');
    const protocol = fs.readFileSync(path.join(root, 'src/Protocol.cpp'), 'utf8');
    const reconcile =
      fs.readFileSync(path.join(root, 'apps/web/reconcile.mjs'), 'utf8');
    assert.doesNotMatch(protocol, /ManifestLifecycle|\\.lifecycle/);
    assert.doesNotMatch(reconcile, /ManifestLifecycle|\\.lifecycle/);
  });

  await check('production consumers contain no handwritten enum mirrors', () => {
    const root = path.resolve(
      path.dirname(fileURLToPath(import.meta.url)), '../..');
    const protocol = fs.readFileSync(path.join(root, 'src/Protocol.cpp'), 'utf8');
    const uiTree =
      fs.readFileSync(path.join(root, 'src/UiTreeProtocol.cpp'), 'utf8');
    const reconcile =
      fs.readFileSync(path.join(root, 'apps/web/reconcile.mjs'), 'utf8');
    const client = fs.readFileSync(path.join(root, 'apps/web/client.mjs'), 'utf8');
    assert.doesNotMatch(
      uiTree, /kAll(?:Axes|ScrollAxes|SizeKinds|Overflows)/);
    assert.doesNotMatch(
      reconcile,
      /export const (?:WIDGET|AXIS|SIZE|SCROLL|SURFACE|PICKER_MODE|CLIENT_OWNED_INPUT|PALETTE_PRESENCE_OP)\s*=/);
    assert.doesNotMatch(
      client,
      /const (?:ROLE|FOCUS_EDITOR|VIEW_ACTION|VIEW_SCROLL_TARGET)\s*=/);
    assert.doesNotMatch(reconcile, /\bkind:\s*[0-9]+n?\b/);
    assert.doesNotMatch(
      reconcile, /\bkind:\s*BigInt\(CLIENT_INPUT_KIND\./);
    assert.match(reconcile, /buildClientInputDocumentWire/);
    assert.match(client, /validateClientInputResultWire\(payload\)/);
    assert.match(client, /validateCommandResultWire\(payload\)/);
    assert.match(client, /validateSessionSnapshotWire\(payload\)/);
    assert.match(client, /validateSessionDeltaWire\(payload\)/);
    assert.match(
      protocol, /validateSessionSnapshotWire\(payload\)/);
    assert.match(protocol, /validateSessionDeltaWire\(payload\)/);
    assert.match(protocol, /ShellNodeKind::Header/);
    assert.match(protocol, /FocusTarget::Editor/);
    assert.doesNotMatch(protocol, /\bhasExactly\b|\bviewEnumField\b/);
    for (const symbol of [
      'StatusActionInvocation',
      'ResolvedSelectionRange',
      'ExternalActionInvocation',
    ]) {
      const start = protocol.indexOf(
        `std::optional<${symbol}>& out)`);
      const end = protocol.indexOf('\n}', start);
      assert.ok(start >= 0 && end > start);
      assert.doesNotMatch(
        protocol.slice(start, end), /object->size\(\)\s*[!=]=?\s*\d/);
    }
  });
} finally {
  await fsp.rm(temporary, { recursive: true, force: true });
}

process.stdout.write(`semantic wire generator: ${checks} checks passed\n`);
