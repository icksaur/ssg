export const lifecycle = Object.freeze({
  CURRENT: 'current',
  COMPATIBILITY: 'compatibility',
  RETIRED: 'retired',
});

export const replayPolicy = Object.freeze({
  REPLACEMENT: 'replacement',
  CHANGED_REPLACEMENT: 'changed-replacement',
  SPECIALIZED: 'specialized',
  COMPATIBILITY: 'compatibility',
});

export const messageKinds = Object.freeze([
  { symbol: 'CommandRequest', wireName: 'command_request', ordinal: 0,
    lifecycle: lifecycle.CURRENT },
  { symbol: 'SessionSnapshot', wireName: 'session_snapshot', ordinal: 1,
    lifecycle: lifecycle.CURRENT },
  { symbol: 'SessionDelta', wireName: 'session_delta', ordinal: 2,
    lifecycle: lifecycle.CURRENT },
  { symbol: 'ClipboardRequest', wireName: 'clipboard_request', ordinal: 3,
    lifecycle: lifecycle.RETIRED },
  { symbol: 'ClipboardResponse', wireName: 'clipboard_response', ordinal: 4,
    lifecycle: lifecycle.RETIRED },
  { symbol: 'StatusActionInvocation', wireName: 'status_action_invocation',
    ordinal: 5, lifecycle: lifecycle.RETIRED },
  { symbol: 'CommandResult', wireName: 'command_result', ordinal: 6,
    lifecycle: lifecycle.CURRENT },
  { symbol: 'ClientInput', wireName: 'client_input', ordinal: 7,
    lifecycle: lifecycle.CURRENT },
  { symbol: 'ClientInputResult', wireName: 'client_input_result', ordinal: 8,
    lifecycle: lifecycle.CURRENT },
]);

export const semanticSections = Object.freeze([
  { symbol: 'Document', snapshot: 'document',
    delta: ['document', 'document_caret'], lifecycle: lifecycle.CURRENT,
    replay: replayPolicy.SPECIALIZED },
  { symbol: 'Selection', snapshot: 'selection', delta: ['selection'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.CHANGED_REPLACEMENT },
  { symbol: 'History', snapshot: 'history', delta: ['history'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.CHANGED_REPLACEMENT },
  { symbol: 'Clipboard', snapshot: 'clipboard', delta: ['clipboard'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.CHANGED_REPLACEMENT },
  { symbol: 'PromptStatus', snapshot: 'prompt_status',
    delta: ['prompt_status'], lifecycle: lifecycle.CURRENT,
    replay: replayPolicy.CHANGED_REPLACEMENT },
  { symbol: 'Search', snapshot: 'search', delta: ['search'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.SPECIALIZED },
  { symbol: 'FindReplace', snapshot: 'find_replace', delta: ['find_replace'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.SPECIALIZED },
  { symbol: 'Settings', snapshot: 'settings', delta: ['settings'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.SPECIALIZED },
  { symbol: 'Keymap', snapshot: 'keymap', delta: ['keymap'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.CHANGED_REPLACEMENT },
  { symbol: 'TextEncoding', snapshot: 'text_encoding',
    delta: ['text_encoding'], lifecycle: lifecycle.CURRENT,
    replay: replayPolicy.SPECIALIZED },
  { symbol: 'Tabs', snapshot: 'tabs', delta: ['tabs'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.SPECIALIZED },
  { symbol: 'Diff', snapshot: 'diff', delta: ['diff'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.SPECIALIZED },
  { symbol: 'ExternalModification', snapshot: 'external_modification',
    delta: ['external_modification'], lifecycle: lifecycle.CURRENT,
    replay: replayPolicy.SPECIALIZED },
  { symbol: 'FollowEdits', snapshot: 'follow_edits', delta: ['follow_edits'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.SPECIALIZED },
  { symbol: 'Tree', snapshot: 'tree', delta: ['tree'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.SPECIALIZED },
  { symbol: 'Syntax', snapshot: 'syntax', delta: ['syntax'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.SPECIALIZED },
  { symbol: 'LspSync', snapshot: 'lsp_sync', delta: ['lsp_sync'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.SPECIALIZED },
  { symbol: 'LspFeatures', snapshot: 'lsp_features',
    delta: ['lsp_features'], lifecycle: lifecycle.CURRENT,
    replay: replayPolicy.SPECIALIZED },
  { symbol: 'Theme', snapshot: 'theme', delta: ['theme'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.REPLACEMENT },
  { symbol: 'Focus', snapshot: 'focus', delta: ['focus'],
    lifecycle: lifecycle.COMPATIBILITY, replay: replayPolicy.COMPATIBILITY },
  { symbol: 'Palette', snapshot: 'palette', delta: ['palette'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.REPLACEMENT },
  { symbol: 'UiFrame', snapshot: 'ui_frame', delta: ['ui_frame_delta'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.SPECIALIZED },
  { symbol: 'PromptView', snapshot: 'prompt_view', delta: ['prompt_view'],
    lifecycle: lifecycle.COMPATIBILITY, replay: replayPolicy.COMPATIBILITY },
  { symbol: 'NoticeView', snapshot: 'notice_view', delta: ['notice_view'],
    lifecycle: lifecycle.CURRENT, replay: replayPolicy.CHANGED_REPLACEMENT },
  { symbol: 'WatcherAvailable', snapshot: 'watcher_available',
    delta: ['watcher_available'], lifecycle: lifecycle.CURRENT,
    replay: replayPolicy.REPLACEMENT },
  { symbol: 'ExternalFocusHeld', snapshot: 'external_focus_held',
    delta: ['external_focus_held'], lifecycle: lifecycle.COMPATIBILITY,
    replay: replayPolicy.COMPATIBILITY },
]);

export const wireEnums = Object.freeze([
  {
    symbol: 'CommandError', jsName: 'COMMAND_ERROR',
    values: [
      { symbol: 'None', wireName: 'none', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'UnknownClient', wireName: 'unknown_client', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'UnknownCommand', wireName: 'unknown_command', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'StaleRevision', wireName: 'stale_revision', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'CapabilityDenied', wireName: 'capability_denied', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'HandlerFailed', wireName: 'handler_failed', ordinal: 5, lifecycle: lifecycle.CURRENT },
      { symbol: 'RevisionExhausted', wireName: 'revision_exhausted', ordinal: 6, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'DocumentMode', jsName: 'DOCUMENT_MODE',
    values: [
      { symbol: 'Edit', wireName: 'edit', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'ReadOnly', wireName: 'read_only', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Diff', wireName: 'diff', ordinal: 2, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'StatusPriority', jsName: 'STATUS_PRIORITY',
    values: [
      { symbol: 'Error', wireName: 'error', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Warning', wireName: 'warning', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Information', wireName: 'information', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Progress', wireName: 'progress', ordinal: 3, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'PromptKind', jsName: 'PROMPT_KIND',
    values: [
      { symbol: 'Path', wireName: 'path', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Find', wireName: 'find', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Replace', wireName: 'replace', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Settings', wireName: 'settings', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'CommandArgument', wireName: 'command_argument', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'Palette', wireName: 'palette', ordinal: 5, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'PromptControlKind', jsName: 'PROMPT_CONTROL_KIND',
    values: [
      { symbol: 'Input', wireName: 'input', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Toggle', wireName: 'toggle', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Count', wireName: 'count', ordinal: 2, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'SearchMode', jsName: 'SEARCH_MODE',
    values: [
      { symbol: 'File', wireName: 'file', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Line', wireName: 'line', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Symbol', wireName: 'symbol', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Text', wireName: 'text', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'Command', wireName: 'command', ordinal: 4, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'FindReplaceError', jsName: 'FIND_REPLACE_ERROR',
    values: [
      { symbol: 'None', wireName: 'none', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'InvalidPattern', wireName: 'invalid_pattern', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'InvalidUtf8', wireName: 'invalid_utf8', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'InvalidSelection', wireName: 'invalid_selection', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'BudgetExhausted', wireName: 'budget_exhausted', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'Cancelled', wireName: 'cancelled', ordinal: 5, lifecycle: lifecycle.CURRENT },
      { symbol: 'NoMatch', wireName: 'no_match', ordinal: 6, lifecycle: lifecycle.CURRENT },
      { symbol: 'StaleRevision', wireName: 'stale_revision', ordinal: 7, lifecycle: lifecycle.CURRENT },
      { symbol: 'DocumentRejected', wireName: 'document_rejected', ordinal: 8, lifecycle: lifecycle.CURRENT },
      { symbol: 'WorkspaceRejected', wireName: 'workspace_rejected', ordinal: 9, lifecycle: lifecycle.CURRENT },
      { symbol: 'RecoveryRejected', wireName: 'recovery_rejected', ordinal: 10, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'SettingScope', jsName: 'SETTING_SCOPE',
    values: [
      { symbol: 'Defaults', wireName: 'defaults', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'User', wireName: 'user', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Workspace', wireName: 'workspace', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Language', wireName: 'language', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'Document', wireName: 'document', ordinal: 4, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'SettingKey', jsName: 'SETTING_KEY',
    values: [
      { symbol: 'IndentWidth', wireName: 'indent_width', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'IndentStyle', wireName: 'indent_style', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'IndentDetection', wireName: 'indent_detection', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'AutoIndent', wireName: 'auto_indent', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'LineEnding', wireName: 'line_ending', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'FinalNewline', wireName: 'final_newline', ordinal: 5, lifecycle: lifecycle.CURRENT },
      { symbol: 'Encoding', wireName: 'encoding', ordinal: 6, lifecycle: lifecycle.CURRENT },
      { symbol: 'WordWrap', wireName: 'word_wrap', ordinal: 7, lifecycle: lifecycle.CURRENT },
      { symbol: 'Theme', wireName: 'theme', ordinal: 8, lifecycle: lifecycle.CURRENT },
      { symbol: 'Keymap', wireName: 'keymap', ordinal: 9, lifecycle: lifecycle.CURRENT },
      { symbol: 'SearchCaseSensitive', wireName: 'search_case_sensitive', ordinal: 10, lifecycle: lifecycle.CURRENT },
      { symbol: 'SearchWholeWord', wireName: 'search_whole_word', ordinal: 11, lifecycle: lifecycle.CURRENT },
      { symbol: 'SearchRegularExpression', wireName: 'search_regular_expression', ordinal: 12, lifecycle: lifecycle.CURRENT },
      { symbol: 'UndoByteBudget', wireName: 'undo_byte_budget', ordinal: 13, lifecycle: lifecycle.CURRENT },
      { symbol: 'RecoveryByteBudget', wireName: 'recovery_byte_budget', ordinal: 14, lifecycle: lifecycle.CURRENT },
      { symbol: 'TypingCoalescingMs', wireName: 'typing_coalescing_ms', ordinal: 15, lifecycle: lifecycle.CURRENT },
      { symbol: 'FileFinderRespectGitignore', wireName: 'file_finder_respect_gitignore', ordinal: 16, lifecycle: lifecycle.CURRENT },
      { symbol: 'AutosaveDebounceMs', wireName: 'autosave_debounce_ms', ordinal: 17, lifecycle: lifecycle.CURRENT },
      { symbol: 'LineNumbers', wireName: 'line_numbers', ordinal: 18, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'TextEncoding', jsName: 'TEXT_ENCODING',
    values: [
      { symbol: 'Utf8', wireName: 'utf8', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Utf8Bom', wireName: 'utf8_bom', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Utf16le', wireName: 'utf16le', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Utf16be', wireName: 'utf16be', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'Windows1252', wireName: 'windows1252', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'Iso88591', wireName: 'iso88591', ordinal: 5, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'IndentStyle', jsName: 'INDENT_STYLE',
    values: [
      { symbol: 'Spaces', wireName: 'spaces', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Tabs', wireName: 'tabs', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'LineEnding', jsName: 'LINE_ENDING',
    values: [
      { symbol: 'Lf', wireName: 'lf', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Crlf', wireName: 'crlf', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Cr', wireName: 'cr', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Mixed', wireName: 'mixed', ordinal: 3, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'TabKind', jsName: 'TAB_KIND',
    values: [
      { symbol: 'Document', wireName: 'document', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'LiveDiff', wireName: 'live_diff', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'ReadOnlyOutput', wireName: 'read_only_output', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'SearchResults', wireName: 'search_results', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'TreeView', wireName: 'tree_view', ordinal: 4, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'TabRecoveryBadge', jsName: 'TAB_RECOVERY_BADGE',
    values: [
      { symbol: 'None', wireName: 'none', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Pending', wireName: 'pending', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Durable', wireName: 'durable', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Failed', wireName: 'failed', ordinal: 3, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'JournalDocumentKeyKind', jsName: 'JOURNAL_DOCUMENT_KEY_KIND',
    values: [
      { symbol: 'Saved', wireName: 'saved', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Untitled', wireName: 'untitled', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'DiffLineKind', jsName: 'DIFF_LINE_KIND',
    values: [
      { symbol: 'Added', wireName: 'added', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Removed', wireName: 'removed', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Modified', wireName: 'modified', ordinal: 2, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'DiffFileStatus', jsName: 'DIFF_FILE_STATUS',
    values: [
      { symbol: 'Added', wireName: 'added', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Modified', wireName: 'modified', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Deleted', wireName: 'deleted', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Renamed', wireName: 'renamed', ordinal: 3, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'ExternalAction', jsName: 'EXTERNAL_ACTION',
    values: [
      { symbol: 'Reload', wireName: 'reload', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'KeepBuffer', wireName: 'keep_buffer', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'OpenDiff', wireName: 'open_diff', ordinal: 2, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'ExternalDocumentStatus', jsName: 'EXTERNAL_DOCUMENT_STATUS',
    values: [
      { symbol: 'ExternallyModified', wireName: 'externally_modified', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'ExternallyRemoved', wireName: 'externally_removed', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'FollowMode', jsName: 'FOLLOW_MODE',
    values: [
      { symbol: 'Following', wireName: 'following', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Paused', wireName: 'paused', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'TreeProviderKind', jsName: 'TREE_PROVIDER_KIND',
    values: [
      { symbol: 'Filesystem', wireName: 'filesystem', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Git', wireName: 'git', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Symbols', wireName: 'symbols', ordinal: 2, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'TreeNodeKind', jsName: 'TREE_NODE_KIND',
    values: [
      { symbol: 'Root', wireName: 'root', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Directory', wireName: 'directory', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'File', wireName: 'file', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Symlink', wireName: 'symlink', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'GitEntry', wireName: 'git_entry', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'Symbol', wireName: 'symbol', ordinal: 5, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'GitTreeStatus', jsName: 'GIT_TREE_STATUS',
    values: [
      { symbol: 'Added', wireName: 'added', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Modified', wireName: 'modified', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Deleted', wireName: 'deleted', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Renamed', wireName: 'renamed', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'Untracked', wireName: 'untracked', ordinal: 4, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'SyntaxScope', jsName: 'SYNTAX_SCOPE',
    values: [
      { symbol: 'PlainText', wireName: 'plain_text', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Comment', wireName: 'comment', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Keyword', wireName: 'keyword', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'String', wireName: 'string', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'Number', wireName: 'number', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'Type', wireName: 'type', ordinal: 5, lifecycle: lifecycle.CURRENT },
      { symbol: 'Function', wireName: 'function', ordinal: 6, lifecycle: lifecycle.CURRENT },
      { symbol: 'Variable', wireName: 'variable', ordinal: 7, lifecycle: lifecycle.CURRENT },
      { symbol: 'OperatorToken', wireName: 'operator', ordinal: 8, lifecycle: lifecycle.CURRENT },
      { symbol: 'Punctuation', wireName: 'punctuation', ordinal: 9, lifecycle: lifecycle.CURRENT },
      { symbol: 'Invalid', wireName: 'invalid', ordinal: 10, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'BracketKind', jsName: 'BRACKET_KIND',
    values: [
      { symbol: 'Round', wireName: 'round', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Square', wireName: 'square', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Curly', wireName: 'curly', ordinal: 2, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'BracketRole', jsName: 'BRACKET_ROLE',
    values: [
      { symbol: 'Open', wireName: 'open', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Close', wireName: 'close', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'CommentKind', jsName: 'COMMENT_KIND',
    values: [
      { symbol: 'Line', wireName: 'line', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Block', wireName: 'block', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'CommentTokenRole', jsName: 'COMMENT_TOKEN_ROLE',
    values: [
      { symbol: 'Line', wireName: 'line', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'BlockOpen', wireName: 'block_open', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'BlockClose', wireName: 'block_close', ordinal: 2, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'LspDiagnosticSeverity', jsName: 'LSP_DIAGNOSTIC_SEVERITY',
    values: [
      { symbol: 'Error', wireName: 'error', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Warning', wireName: 'warning', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Information', wireName: 'information', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'Hint', wireName: 'hint', ordinal: 4, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'ShellNodeKind', jsName: 'SHELL_NODE_KIND',
    values: [
      { symbol: 'Header', wireName: 'header', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'HeaderField', wireName: 'header_field', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Footer', wireName: 'footer', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'FooterField', wireName: 'footer_field', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'FooterAction', wireName: 'footer_action', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'TabBar', wireName: 'tab_bar', ordinal: 5, lifecycle: lifecycle.CURRENT },
      { symbol: 'Tab', wireName: 'tab', ordinal: 6, lifecycle: lifecycle.CURRENT },
      { symbol: 'Panel', wireName: 'panel', ordinal: 7, lifecycle: lifecycle.CURRENT },
      { symbol: 'PanelProvider', wireName: 'panel_provider', ordinal: 8, lifecycle: lifecycle.CURRENT },
      { symbol: 'Pane', wireName: 'pane', ordinal: 9, lifecycle: lifecycle.CURRENT },
      { symbol: 'Scrollbar', wireName: 'scrollbar', ordinal: 10, lifecycle: lifecycle.CURRENT },
      { symbol: 'PromptReservation', wireName: 'prompt_reservation', ordinal: 11, lifecycle: lifecycle.CURRENT },
      { symbol: 'EmptyState', wireName: 'empty_state', ordinal: 12, lifecycle: lifecycle.CURRENT },
      { symbol: 'NoticeBar', wireName: 'notice_bar', ordinal: 13, lifecycle: lifecycle.CURRENT },
      { symbol: 'NoticeAction', wireName: 'notice_action', ordinal: 14, lifecycle: lifecycle.CURRENT },
      { symbol: 'FooterHint', wireName: 'footer_hint', ordinal: 15, lifecycle: lifecycle.CURRENT },
      { symbol: 'TabSeparator', wireName: 'tab_separator', ordinal: 16, lifecycle: lifecycle.CURRENT },
      { symbol: 'ExternalModificationBar', wireName: 'external_modification_bar', ordinal: 17, lifecycle: lifecycle.CURRENT },
      { symbol: 'ExternalModificationRow', wireName: 'external_modification_row', ordinal: 18, lifecycle: lifecycle.CURRENT },
      { symbol: 'ExternalModificationAction', wireName: 'external_modification_action', ordinal: 19, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'FocusTarget', jsName: 'FOCUS_TARGET',
    values: [
      { symbol: 'Editor', wireName: 'editor', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Panel', wireName: 'panel', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Prompt', wireName: 'prompt', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'ExternalModification', wireName: 'external_modification', ordinal: 3, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'SemanticRole', jsName: 'SEMANTIC_ROLE',
    values: [
      { symbol: 'Text', wireName: 'text', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Canvas', wireName: 'canvas', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Caret', wireName: 'caret', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Selection', wireName: 'selection', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'TreeBackground', wireName: 'tree_background', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'TreeFocus', wireName: 'tree_focus', ordinal: 5, lifecycle: lifecycle.CURRENT },
      { symbol: 'TabActive', wireName: 'tab_active', ordinal: 6, lifecycle: lifecycle.CURRENT },
      { symbol: 'TabInactive', wireName: 'tab_inactive', ordinal: 7, lifecycle: lifecycle.CURRENT },
      { symbol: 'PanelActive', wireName: 'panel_active', ordinal: 8, lifecycle: lifecycle.CURRENT },
      { symbol: 'PanelInactive', wireName: 'panel_inactive', ordinal: 9, lifecycle: lifecycle.CURRENT },
      { symbol: 'Header', wireName: 'header', ordinal: 10, lifecycle: lifecycle.CURRENT },
      { symbol: 'Footer', wireName: 'footer', ordinal: 11, lifecycle: lifecycle.CURRENT },
      { symbol: 'StatusInfo', wireName: 'status_info', ordinal: 12, lifecycle: lifecycle.CURRENT },
      { symbol: 'StatusWarning', wireName: 'status_warning', ordinal: 13, lifecycle: lifecycle.CURRENT },
      { symbol: 'LineNumber', wireName: 'line_number', ordinal: 14, lifecycle: lifecycle.CURRENT },
      { symbol: 'SearchMatch', wireName: 'search_match', ordinal: 15, lifecycle: lifecycle.CURRENT },
      { symbol: 'Prompt', wireName: 'prompt', ordinal: 16, lifecycle: lifecycle.CURRENT },
      { symbol: 'ScrollbarTrack', wireName: 'scrollbar_track', ordinal: 17, lifecycle: lifecycle.CURRENT },
      { symbol: 'ScrollbarThumb', wireName: 'scrollbar_thumb', ordinal: 18, lifecycle: lifecycle.CURRENT },
      { symbol: 'DiffAdded', wireName: 'diff_added', ordinal: 19, lifecycle: lifecycle.CURRENT },
      { symbol: 'DiffRemoved', wireName: 'diff_removed', ordinal: 20, lifecycle: lifecycle.CURRENT },
      { symbol: 'DiffModified', wireName: 'diff_modified', ordinal: 21, lifecycle: lifecycle.CURRENT },
      { symbol: 'TabInactiveBackground', wireName: 'tab_inactive_background', ordinal: 22, lifecycle: lifecycle.CURRENT },
      { symbol: 'HeaderBackground', wireName: 'header_background', ordinal: 23, lifecycle: lifecycle.CURRENT },
      { symbol: 'FooterBackground', wireName: 'footer_background', ordinal: 24, lifecycle: lifecycle.CURRENT },
      { symbol: 'CurrentLineNumber', wireName: 'current_line_number', ordinal: 25, lifecycle: lifecycle.CURRENT },
      { symbol: 'CurrentLineNumberBackground', wireName: 'current_line_number_background', ordinal: 26, lifecycle: lifecycle.CURRENT },
      { symbol: 'LineNumberBackground', wireName: 'line_number_background', ordinal: 27, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'ClientInputKind', jsName: 'CLIENT_INPUT_KIND',
    values: [
      { symbol: 'Key', wireName: 'key', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Tab', wireName: 'tab', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Tree', wireName: 'tree', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Picker', wireName: 'picker', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'PromptControl', wireName: 'prompt_control', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'ExternalAction', wireName: 'external_action', ordinal: 5, lifecycle: lifecycle.CURRENT },
      { symbol: 'StatusAction', wireName: 'status_action', ordinal: 6, lifecycle: lifecycle.CURRENT },
      { symbol: 'PublishedUiAction', wireName: 'published_ui_action', ordinal: 7, lifecycle: lifecycle.CURRENT },
      { symbol: 'NoticeAction', wireName: 'notice_action', ordinal: 8, lifecycle: lifecycle.CURRENT },
      { symbol: 'Document', wireName: 'document', ordinal: 9, lifecycle: lifecycle.CURRENT },
      { symbol: 'ScrollLines', wireName: 'scroll_lines', ordinal: 10, lifecycle: lifecycle.CURRENT },
      { symbol: 'ScrollFraction', wireName: 'scroll_fraction', ordinal: 11, lifecycle: lifecycle.CURRENT },
      { symbol: 'ViewNavigation', wireName: 'view_navigation', ordinal: 12, lifecycle: lifecycle.CURRENT },
      { symbol: 'ResolvedPaneFocus', wireName: 'resolved_pane_focus', ordinal: 13, lifecycle: lifecycle.CURRENT },
      { symbol: 'ResolvedSelection', wireName: 'resolved_selection', ordinal: 14, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'InputPointerButton', jsName: 'INPUT_POINTER_BUTTON',
    values: [
      { symbol: 'Primary', wireName: 'primary', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Auxiliary', wireName: 'auxiliary', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Secondary', wireName: 'secondary', ordinal: 2, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'InputPointerPhase', jsName: 'INPUT_POINTER_PHASE',
    values: [
      { symbol: 'Press', wireName: 'press', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Move', wireName: 'move', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Release', wireName: 'release', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Cancel', wireName: 'cancel', ordinal: 3, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'DocumentPointerEdge', jsName: 'DOCUMENT_POINTER_EDGE',
    values: [
      { symbol: 'None', wireName: 'none', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Before', wireName: 'before', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'After', wireName: 'after', ordinal: 2, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'SemanticScrollTarget', jsName: 'SEMANTIC_SCROLL_TARGET',
    values: [
      { symbol: 'Document', wireName: 'document', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Tree', wireName: 'tree', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'ClientOwnedInputKind', jsName: 'CLIENT_OWNED_INPUT_KIND',
    values: [
      { symbol: 'AppendText', wireName: 'append_text', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'DeleteGraphemeBackward', wireName: 'delete_grapheme_backward', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'DeleteWordBackward', wireName: 'delete_word_backward', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'SelectNext', wireName: 'select_next', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'SelectPrevious', wireName: 'select_previous', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'Submit', wireName: 'submit', ordinal: 5, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'ClientInputOutcome', jsName: 'CLIENT_INPUT_OUTCOME',
    values: [
      { symbol: 'Unhandled', wireName: 'unhandled', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'ClientOwned', wireName: 'client_owned', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Dispatched', wireName: 'dispatched', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Rejected', wireName: 'rejected', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'ViewOwned', wireName: 'view_owned', ordinal: 4, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'WidgetKind', jsName: 'WIDGET_KIND',
    values: [
      { symbol: 'Container', wireName: 'container', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Label', wireName: 'label', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Field', wireName: 'field', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Checkbox', wireName: 'checkbox', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'TextInput', wireName: 'text_input', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'Spacer', wireName: 'spacer', ordinal: 5, lifecycle: lifecycle.CURRENT },
      { symbol: 'View', wireName: 'view', ordinal: 6, lifecycle: lifecycle.CURRENT },
      { symbol: 'StatusActions', wireName: 'status_actions', ordinal: 7, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'ViewSurface', jsName: 'VIEW_SURFACE',
    values: [
      { symbol: 'TabBar', wireName: 'tabbar', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'FileTree', wireName: 'file_tree', ordinal: 1, lifecycle: lifecycle.RETIRED },
      { symbol: 'GitStatus', wireName: 'git_status', ordinal: 2, lifecycle: lifecycle.RETIRED },
      { symbol: 'FindResults', wireName: 'findresults', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'Symbols', wireName: 'symbols', ordinal: 4, lifecycle: lifecycle.RETIRED },
      { symbol: 'FooterPrompt', wireName: 'footer_prompt', ordinal: 5, lifecycle: lifecycle.RETIRED },
      { symbol: 'Notice', wireName: 'notice', ordinal: 6, lifecycle: lifecycle.CURRENT },
      { symbol: 'ExternalModification', wireName: 'external_modification', ordinal: 7, lifecycle: lifecycle.CURRENT },
      { symbol: 'Document', wireName: 'document', ordinal: 8, lifecycle: lifecycle.CURRENT },
      { symbol: 'Tree', wireName: 'tree', ordinal: 9, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'Axis', jsName: 'AXIS',
    values: [
      { symbol: 'Row', wireName: 'row', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Column', wireName: 'column', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'ScrollAxis', jsName: 'SCROLL_AXIS',
    values: [
      { symbol: 'None', wireName: 'none', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Vertical', wireName: 'vertical', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'map-to', fallback: 'None' },
  },
  {
    symbol: 'SizeKind', jsName: 'SIZE_KIND',
    values: [
      { symbol: 'Exact', wireName: 'exact', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Flex', wireName: 'flex', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Auto', wireName: 'auto', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Responsive', wireName: 'responsive', ordinal: 3, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'Overflow', jsName: 'OVERFLOW',
    values: [
      { symbol: 'None', wireName: 'none', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Truncate', wireName: 'truncate', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'ScrollTail', wireName: 'scroll_tail', ordinal: 2, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'ViewActionKind', jsName: 'VIEW_ACTION_KIND',
    values: [
      { symbol: 'ScrollLines', wireName: 'scroll_lines', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'ScrollPages', wireName: 'scroll_pages', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'ScrollFraction', wireName: 'scroll_fraction', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'MoveVisualSelection', wireName: 'move_visual_selection', ordinal: 3, lifecycle: lifecycle.CURRENT },
      { symbol: 'RevealSelection', wireName: 'reveal_selection', ordinal: 4, lifecycle: lifecycle.CURRENT },
      { symbol: 'CenterSelection', wireName: 'center_selection', ordinal: 5, lifecycle: lifecycle.CURRENT },
      { symbol: 'SplitPane', wireName: 'split_pane', ordinal: 6, lifecycle: lifecycle.CURRENT },
      { symbol: 'ClosePane', wireName: 'close_pane', ordinal: 7, lifecycle: lifecycle.CURRENT },
      { symbol: 'CyclePane', wireName: 'cycle_pane', ordinal: 8, lifecycle: lifecycle.CURRENT },
      { symbol: 'FocusPane', wireName: 'focus_pane', ordinal: 9, lifecycle: lifecycle.CURRENT },
      { symbol: 'ContinuePointerEdge', wireName: 'continue_pointer_edge', ordinal: 10, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'ViewScrollTarget', jsName: 'VIEW_SCROLL_TARGET',
    values: [
      { symbol: 'Document', wireName: 'document', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Tree', wireName: 'tree', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'VisualSelectionDirection', jsName: 'VISUAL_SELECTION_DIRECTION',
    values: [
      { symbol: 'LineUp', wireName: 'line_up', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'LineDown', wireName: 'line_down', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'PageUp', wireName: 'page_up', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'PageDown', wireName: 'page_down', ordinal: 3, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'SplitAxis', jsName: 'SPLIT_AXIS',
    values: [
      { symbol: 'Horizontal', wireName: 'horizontal', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Vertical', wireName: 'vertical', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'PaneCycleDirection', jsName: 'PANE_CYCLE_DIRECTION',
    values: [
      { symbol: 'Next', wireName: 'next', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Previous', wireName: 'previous', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'PaneDirection', jsName: 'PANE_DIRECTION',
    values: [
      { symbol: 'Left', wireName: 'left', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Right', wireName: 'right', ordinal: 1, lifecycle: lifecycle.CURRENT },
      { symbol: 'Up', wireName: 'up', ordinal: 2, lifecycle: lifecycle.CURRENT },
      { symbol: 'Down', wireName: 'down', ordinal: 3, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'PointerEdgeDirection', jsName: 'POINTER_EDGE_DIRECTION',
    values: [
      { symbol: 'Before', wireName: 'before', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'After', wireName: 'after', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
  {
    symbol: 'PalettePresenceOpKind', jsName: 'PALETTE_PRESENCE_OP_KIND',
    values: [
      { symbol: 'Show', wireName: 'show', ordinal: 0, lifecycle: lifecycle.CURRENT },
      { symbol: 'Hide', wireName: 'hide', ordinal: 1, lifecycle: lifecycle.CURRENT },
    ],
    unknown: { policy: 'reject' },
  },
]);

const optional = (wireName, type) =>
  Object.freeze({ wireName, type, required: false });
const required = (wireName, type) =>
  Object.freeze({ wireName, type, required: true });
const nullable = (value) => Object.freeze({ kind: 'nullable', value });
const enumType = (name, options = {}) =>
  Object.freeze({ kind: 'enum', enum: name, ...options });
const record = (fields) =>
  Object.freeze({ kind: 'record', fields, unknownFields: 'allow' });
const exactRecord = (fields) =>
  Object.freeze({ kind: 'record', fields, unknownFields: 'reject' });
const arrayOf = (items, options = {}) =>
  Object.freeze({ kind: 'array', items, ...options });
const ref = (type, recursive = false) =>
  Object.freeze({ kind: 'ref', type, ...(recursive ? { recursive: true } : {}) });
const boundedUint = Object.freeze({ kind: 'uint', maxHostInt: true });
const boundedInt = Object.freeze({ kind: 'int', hostInt: true });
const nonEmptyText = Object.freeze({ kind: 'text', nonEmpty: true });

const insetSchema = record([
  required('left', boundedUint),
  required('right', boundedUint),
  required('top', boundedUint),
  required('bottom', boundedUint),
]);
const valueSourceSchema = record([
  required('is_provider', 'bool'),
  required('literal', 'text'),
  required('provider', 'text'),
]);
const nodeStyleSchema = record([
  optional('foreground', enumType('SemanticRole')),
  optional('background', enumType('SemanticRole')),
]);
const sizeSchema = Object.freeze({
  kind: 'discriminated-record',
  unknownFields: 'allow',
  discriminator: { wireName: 'kind', enum: 'SizeKind' },
  fields: [required('extent', boundedUint)],
  variants: [
    { value: 'Flex', fields: [] },
    { value: 'Auto', fields: [] },
    { value: 'Exact', fields: [] },
    {
      value: 'Responsive',
      fields: [
        required('minimum', boundedUint),
        required('growth', boundedUint),
        required('optional', 'bool'),
      ],
    },
  ],
});
const widgetSchema = record([
  required('kind', enumType('WidgetKind')),
  required('id', 'text'),
  optional('value', nullable(valueSourceSchema)),
  optional('checked', nullable(valueSourceSchema)),
  optional('width', nullable(boundedInt)),
  optional('role', nullable('text')),
  optional('command', nullable('text')),
  optional('surface', nullable(enumType('ViewSurface', { values: 'reservations' }))),
  required('rank', boundedInt),
  required('keep', 'bool'),
  required('overflow', enumType('Overflow')),
  required('sigil', 'text'),
]);
const uiNodeSchema = Object.freeze({
  kind: 'field-union',
  unknownFields: 'allow',
  fields: [
    required('id', 'text'),
    required('size', sizeSchema),
    optional('style', nodeStyleSchema),
    optional('focus_context', enumType('FocusTarget')),
    optional('accessible_label', 'text'),
  ],
  variants: [
    {
      wireName: 'container',
      type: record([
        required('axis', enumType('Axis')),
        required('inset', insetSchema),
        required('gap', boundedUint),
        required('children', arrayOf(ref('UiNode', true))),
        optional('scroll', enumType('ScrollAxis', { acceptUnknown: true })),
      ]),
    },
    { wireName: 'leaf', type: widgetSchema },
  ],
});
const uiLeafStateSchema = record([
  required('value', 'text'),
  required('label', 'text'),
  optional('command', nullable('text')),
  optional('checked', nullable('bool')),
  required('role', enumType('SemanticRole')),
  optional('active', nullable('bool')),
]);
const uiNodeStateSchema = record([
  required('id', nonEmptyText),
  optional('leaf', nullable(uiLeafStateSchema)),
]);
const uiPresenceRecordSchema = record([
  required('id', nonEmptyText),
  required('present', 'bool'),
]);
const uint32 = Object.freeze({ kind: 'uint', maxUint32: true });
const encodedBool = Object.freeze({
  kind: 'uint',
  maxHostInt: true,
  maxUint32: true,
  allowedValues: [0, 1],
});
const keyStrokeSchema = record([
  required('code', 'text'),
  required('control', 'bool'),
  required('alt', 'bool'),
  required('meta', 'bool'),
  required('shift', 'bool'),
]);
const externalActionInvocationSchema = exactRecord([
  required('file_id', 'text'),
  required('action', enumType('ExternalAction')),
]);
const statusActionInvocationSchema = exactRecord([
  required('status_id', 'uint'),
  required('action_id', 'text'),
  required('generation', 'uint'),
]);
const resolvedSelectionRangeSchema = exactRecord([
  required('anchor', 'uint'),
  required('active', 'uint'),
]);
const viewActionSchema = Object.freeze({
  kind: 'discriminated-record',
  unknownFields: 'allow',
  discriminator: { wireName: 'kind', enum: 'ViewActionKind' },
  fields: [],
  variants: [
    {
      value: 'ScrollLines',
      fields: [
        required('target', enumType('ViewScrollTarget')),
        required('rows', 'int'),
      ],
    },
    { value: 'ScrollPages', fields: [required('pages', 'int')] },
    {
      value: 'ScrollFraction',
      fields: [
        required('target', enumType('ViewScrollTarget')),
        required('numerator', uint32),
        required('denominator', uint32),
      ],
    },
    {
      value: 'MoveVisualSelection',
      fields: [
        required('direction', enumType('VisualSelectionDirection')),
        required('extend', 'bool'),
      ],
    },
    { value: 'RevealSelection', fields: [] },
    { value: 'CenterSelection', fields: [] },
    {
      value: 'SplitPane',
      fields: [required('axis', enumType('SplitAxis'))],
    },
    { value: 'ClosePane', fields: [] },
    {
      value: 'CyclePane',
      fields: [required('direction', enumType('PaneCycleDirection'))],
    },
    {
      value: 'FocusPane',
      fields: [required('direction', enumType('PaneDirection'))],
    },
    {
      value: 'ContinuePointerEdge',
      fields: [required('direction', enumType('PointerEdgeDirection'))],
    },
  ],
});
const viewActionRequestSchema = record([
  required('view_id', 'uint'),
  required('semantic_revision', 'uint'),
  required('action', ref('ViewAction')),
]);
const commandResultSchema = record([
  required('error', enumType('CommandError')),
  required('revision', 'uint'),
  required('message', 'text'),
  optional('routingChanged', encodedBool),
  optional('geometryChanged', encodedBool),
  optional('view_action', ref('ViewActionRequest')),
]);
const pointerFields = () => [
  required('button', enumType('InputPointerButton')),
  required('phase', enumType('InputPointerPhase')),
];
const basedPointerFields = () => [
  ...pointerFields(),
  required('basis_revision', 'uint'),
];
const clientInputSchema = Object.freeze({
  kind: 'discriminated-record',
  unknownFields: 'reject',
  discriminator: { wireName: 'kind', enum: 'ClientInputKind' },
  fields: [],
  variants: [
    {
      value: 'Key',
      fields: [
        required('stroke', nullable(ref('KeyStroke'))),
        required('committed_text', 'text'),
      ],
    },
    {
      value: 'Tab',
      fields: [...basedPointerFields(), required('tab_id', 'uint')],
    },
    {
      value: 'Tree',
      fields: [...basedPointerFields(), required('node_id', 'text')],
    },
    {
      value: 'Picker',
      fields: [
        ...pointerFields(),
        required('picker_mode', enumType('SearchMode')),
        required('activation_id', 'uint'),
        required('candidate_id', 'text'),
      ],
    },
    {
      value: 'PromptControl',
      fields: [...basedPointerFields(), required('control_id', 'text')],
    },
    {
      value: 'ExternalAction',
      fields: [
        ...basedPointerFields(),
        required('invocation', ref('ExternalActionInvocation')),
      ],
    },
    {
      value: 'StatusAction',
      fields: [
        ...basedPointerFields(),
        required('invocation', ref('StatusActionInvocation')),
      ],
    },
    {
      value: 'PublishedUiAction',
      fields: [
        ...basedPointerFields(),
        required('schema_generation', 'uint'),
        required('node_id', 'text'),
      ],
    },
    {
      value: 'NoticeAction',
      fields: [...basedPointerFields(), required('action_id', 'text')],
    },
    {
      value: 'Document',
      fields: [
        ...basedPointerFields(),
        required('position', nullable('uint')),
        required('additive', 'bool'),
        required('select_word', 'bool'),
        required('edge', enumType('DocumentPointerEdge')),
      ],
    },
    {
      value: 'ScrollLines',
      fields: [
        required('basis_revision', 'uint'),
        required('target', enumType('SemanticScrollTarget')),
        required('rows', 'int'),
      ],
    },
    {
      value: 'ScrollFraction',
      fields: [
        required('basis_revision', 'uint'),
        required('target', enumType('SemanticScrollTarget')),
        required('numerator', uint32),
        required('denominator', uint32),
      ],
    },
    {
      value: 'ViewNavigation',
      fields: [required('basis_revision', 'uint')],
    },
    {
      value: 'ResolvedPaneFocus',
      fields: [required('basis_revision', 'uint')],
    },
    {
      value: 'ResolvedSelection',
      fields: [
        required('basis_revision', 'uint'),
        required('active_tab', 'uint'),
        required('document_revision', 'uint'),
        required('selections', arrayOf(ref('ResolvedSelectionRange'))),
      ],
    },
  ],
});
const clientOwnedInputSchema = record([
  required('kind', enumType('ClientOwnedInputKind')),
  required('text', 'text'),
]);
const pickerActivationSchema = record([
  required('mode', enumType('SearchMode')),
  required('activation_id', 'uint'),
]);
const clientInputResultSchema = record([
  required('outcome', enumType('ClientInputOutcome')),
  required('client_owned', nullable(ref('ClientOwnedInput'))),
  required('command', nullable(ref('CommandResult'))),
  required('picker_activation', nullable(ref('PickerActivation'))),
]);

export const wireTypes = Object.freeze([
  { symbol: 'KeyStroke', schema: keyStrokeSchema, jsBuilder: true },
  {
    symbol: 'ExternalActionInvocation',
    schema: externalActionInvocationSchema,
  },
  {
    symbol: 'StatusActionInvocation',
    schema: statusActionInvocationSchema,
  },
  {
    symbol: 'ResolvedSelectionRange',
    schema: resolvedSelectionRangeSchema,
  },
  { symbol: 'ViewAction', schema: viewActionSchema },
  { symbol: 'ViewActionRequest', schema: viewActionRequestSchema },
  { symbol: 'CommandResult', schema: commandResultSchema },
  { symbol: 'ClientInput', schema: clientInputSchema, jsBuilder: true },
  { symbol: 'ClientOwnedInput', schema: clientOwnedInputSchema },
  { symbol: 'PickerActivation', schema: pickerActivationSchema },
  { symbol: 'ClientInputResult', schema: clientInputResultSchema },
  { symbol: 'UiNodeStyle', schema: nodeStyleSchema },
  { symbol: 'UiNode', schema: uiNodeSchema },
  {
    symbol: 'UiSchema',
    schema: record([
      required('generation', 'uint'),
      required('root', ref('UiNode')),
    ]),
  },
  {
    symbol: 'UiStateSection',
    schema: record([
      required('generation', 'uint'),
      required('nodes', arrayOf(uiNodeStateSchema)),
      optional('focus_path',
        nullable(arrayOf(nonEmptyText, { nonEmpty: true }))),
    ]),
  },
  {
    symbol: 'UiPresenceSection',
    schema: record([
      required('generation', 'uint'),
      required('basis', 'uint'),
      required('nodes', arrayOf(uiPresenceRecordSchema)),
    ]),
  },
  {
    symbol: 'PalettePresenceOverlay',
    schema: record([
      required('generation', 'uint'),
      required('ops', arrayOf(record([
        required('kind', enumType('PalettePresenceOpKind')),
        required('target', nonEmptyText),
      ]))),
    ]),
  },
  {
    symbol: 'UiFrameVersion',
    schema: record([
      required('generation', 'uint'),
      required('presence_basis', 'uint'),
    ]),
  },
  {
    symbol: 'UiFrame',
    schema: record([
      required('version', ref('UiFrameVersion')),
      required('schema', ref('UiSchema')),
      required('state', ref('UiStateSection')),
      required('presence', ref('UiPresenceSection')),
    ]),
  },
  {
    symbol: 'UiFrameDelta',
    // Predecessor split UI fields normalize before this modern envelope exists.
    schema: {
      kind: 'text-discriminated-record',
      unknownFields: 'allow',
      discriminator: { wireName: 'kind' },
      fields: [
        required('base', ref('UiFrameVersion')),
        required('target', ref('UiFrameVersion')),
      ],
      variants: [
        {
          value: 'replacement',
          fields: [required('frame', ref('UiFrame'))],
        },
        {
          value: 'changes',
          fields: [
            required('state', ref('UiStateSection')),
            required('presence', ref('UiPresenceSection')),
          ],
        },
      ],
    },
  },
]);

export default Object.freeze({
  lifecycle,
  replayPolicy,
  messageKinds,
  semanticSections,
  wireEnums,
  wireTypes,
});
