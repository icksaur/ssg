#pragma once

// Shared core enum inventories.  These macros keep closed enum declarations and
// their exhaustive inventories synchronized without coupling core to a transport.

#define SSG_COMMAND_ERROR_ENUMERATORS(X) \
    X(None, 0) X(UnknownClient, 1) X(UnknownCommand, 2) X(StaleRevision, 3) \
    X(CapabilityDenied, 4) X(HandlerFailed, 5) X(RevisionExhausted, 6)
#define SSG_DOCUMENT_MODE_ENUMERATORS(X) X(Edit, 0) X(ReadOnly, 1) X(Diff, 2)
#define SSG_STATUS_PRIORITY_ENUMERATORS(X) \
    X(Error, 0) X(Warning, 1) X(Information, 2) X(Progress, 3)
#define SSG_PROMPT_KIND_ENUMERATORS(X) \
    X(Path, 0) X(Find, 1) X(Replace, 2) X(Settings, 3) X(CommandArgument, 4) X(Palette, 5)
#define SSG_PROMPT_CONTROL_KIND_ENUMERATORS(X) X(Input, 0) X(Toggle, 1) X(Count, 2)
#define SSG_SEARCH_MODE_ENUMERATORS(X) X(File, 0) X(Line, 1) X(Symbol, 2) X(Text, 3) X(Command, 4)
#define SSG_FIND_REPLACE_ERROR_ENUMERATORS(X) \
    X(None, 0) X(InvalidPattern, 1) X(InvalidUtf8, 2) X(InvalidSelection, 3) \
    X(BudgetExhausted, 4) X(Cancelled, 5) X(NoMatch, 6) X(StaleRevision, 7) \
    X(DocumentRejected, 8) X(WorkspaceRejected, 9) X(RecoveryRejected, 10)
#define SSG_SETTING_SCOPE_ENUMERATORS(X) \
    X(Defaults, 0) X(User, 1) X(Workspace, 2) X(Language, 3) X(Document, 4)
#define SSG_SETTING_KEY_ENUMERATORS(X) \
    X(IndentWidth, 0) X(IndentStyle, 1) X(IndentDetection, 2) X(AutoIndent, 3) \
    X(LineEnding, 4) X(FinalNewline, 5) X(Encoding, 6) X(WordWrap, 7) \
    X(Theme, 8) X(Keymap, 9) X(SearchCaseSensitive, 10) X(SearchWholeWord, 11) \
    X(SearchRegularExpression, 12) X(UndoByteBudget, 13) X(RecoveryByteBudget, 14) \
    X(TypingCoalescingMs, 15) X(FileFinderRespectGitignore, 16) \
    X(AutosaveDebounceMs, 17) X(LineNumbers, 18)
#define SSG_TEXT_ENCODING_ENUMERATORS(X) \
    X(Utf8, 0) X(Utf8Bom, 1) X(Utf16le, 2) X(Utf16be, 3) X(Windows1252, 4) X(Iso88591, 5)
#define SSG_INDENT_STYLE_ENUMERATORS(X) X(Spaces, 0) X(Tabs, 1)
#define SSG_LINE_ENDING_ENUMERATORS(X) X(Lf, 0) X(Crlf, 1) X(Cr, 2) X(Mixed, 3)
#define SSG_TAB_KIND_ENUMERATORS(X) \
    X(Document, 0) X(LiveDiff, 1) X(ReadOnlyOutput, 2) X(SearchResults, 3) X(TreeView, 4)
#define SSG_TAB_RECOVERY_BADGE_ENUMERATORS(X) X(None, 0) X(Pending, 1) X(Durable, 2) X(Failed, 3)
#define SSG_JOURNAL_DOCUMENT_KEY_KIND_ENUMERATORS(X) X(Saved, 0) X(Untitled, 1)
#define SSG_DIFF_LINE_KIND_ENUMERATORS(X) X(Added, 0) X(Removed, 1) X(Modified, 2)
#define SSG_DIFF_FILE_STATUS_ENUMERATORS(X) X(Added, 0) X(Modified, 1) X(Deleted, 2) X(Renamed, 3)
#define SSG_EXTERNAL_ACTION_ENUMERATORS(X) X(Reload, 0) X(KeepBuffer, 1) X(OpenDiff, 2)
#define SSG_EXTERNAL_DOCUMENT_STATUS_ENUMERATORS(X) X(ExternallyModified, 0) X(ExternallyRemoved, 1)
#define SSG_FOLLOW_MODE_ENUMERATORS(X) X(Following, 0) X(Paused, 1)
#define SSG_TREE_PROVIDER_KIND_ENUMERATORS(X) X(Filesystem, 0) X(Git, 1) X(Symbols, 2)
#define SSG_TREE_NODE_KIND_ENUMERATORS(X) \
    X(Root, 0) X(Directory, 1) X(File, 2) X(Symlink, 3) X(GitEntry, 4) X(Symbol, 5)
#define SSG_GIT_TREE_STATUS_ENUMERATORS(X) \
    X(Added, 0) X(Modified, 1) X(Deleted, 2) X(Renamed, 3) X(Untracked, 4)
#define SSG_SYNTAX_SCOPE_ENUMERATORS(X) \
    X(PlainText, 0) X(Comment, 1) X(Keyword, 2) X(String, 3) X(Number, 4) \
    X(Type, 5) X(Function, 6) X(Variable, 7) X(OperatorToken, 8) X(Punctuation, 9) X(Invalid, 10)
#define SSG_BRACKET_KIND_ENUMERATORS(X) X(Round, 0) X(Square, 1) X(Curly, 2)
#define SSG_BRACKET_ROLE_ENUMERATORS(X) X(Open, 0) X(Close, 1)
#define SSG_COMMENT_KIND_ENUMERATORS(X) X(Line, 0) X(Block, 1)
#define SSG_COMMENT_TOKEN_ROLE_ENUMERATORS(X) X(Line, 0) X(BlockOpen, 1) X(BlockClose, 2)
#define SSG_LSP_DIAGNOSTIC_SEVERITY_ENUMERATORS(X) X(Error, 1) X(Warning, 2) X(Information, 3) X(Hint, 4)
#define SSG_SHELL_NODE_KIND_ENUMERATORS(X) \
    X(Header, 0) X(HeaderField, 1) X(Footer, 2) X(FooterField, 3) \
    X(FooterAction, 4) X(TabBar, 5) X(Tab, 6) X(Panel, 7) \
    X(PanelProvider, 8) X(Pane, 9) X(Scrollbar, 10) \
    X(PromptReservation, 11) X(EmptyState, 12) X(NoticeBar, 13) \
    X(NoticeAction, 14) X(FooterHint, 15) X(TabSeparator, 16) \
    X(ExternalModificationBar, 17) X(ExternalModificationRow, 18) \
    X(ExternalModificationAction, 19)
#define SSG_FOCUS_TARGET_ENUMERATORS(X) X(Editor, 0) X(Panel, 1) X(Prompt, 2) X(ExternalModification, 3)
#define SSG_SEMANTIC_ROLE_ENUMERATORS(X) \
    X(Text, 0) X(Canvas, 1) X(Caret, 2) X(Selection, 3) X(TreeBackground, 4) X(TreeFocus, 5) \
    X(TabActive, 6) X(TabInactive, 7) X(PanelActive, 8) X(PanelInactive, 9) X(Header, 10) \
    X(Footer, 11) X(StatusInfo, 12) X(StatusWarning, 13) X(LineNumber, 14) X(SearchMatch, 15) \
    X(Prompt, 16) X(ScrollbarTrack, 17) X(ScrollbarThumb, 18) X(DiffAdded, 19) \
    X(DiffRemoved, 20) X(DiffModified, 21) X(TabInactiveBackground, 22) \
    X(HeaderBackground, 23) X(FooterBackground, 24) X(CurrentLineNumber, 25) \
    X(CurrentLineNumberBackground, 26) X(LineNumberBackground, 27)
#define SSG_CLIENT_INPUT_KIND_ENUMERATORS(X) \
    X(Key, 0) X(Tab, 1) X(Tree, 2) X(Picker, 3) X(ExternalAction, 5) \
    X(NoticeAction, 8) X(Document, 9) X(ScrollLines, 10) X(ScrollFraction, 11) \
    X(ViewNavigation, 12) X(ResolvedPaneFocus, 13) X(ResolvedSelection, 14)
#define SSG_INPUT_POINTER_BUTTON_ENUMERATORS(X) X(Primary, 0) X(Auxiliary, 1) X(Secondary, 2)
#define SSG_INPUT_POINTER_PHASE_ENUMERATORS(X) X(Press, 0) X(Move, 1) X(Release, 2) X(Cancel, 3)
#define SSG_DOCUMENT_POINTER_EDGE_ENUMERATORS(X) X(None, 0) X(Before, 1) X(After, 2)
#define SSG_SEMANTIC_SCROLL_TARGET_ENUMERATORS(X) X(Document, 0) X(Tree, 1)
#define SSG_CLIENT_OWNED_INPUT_KIND_ENUMERATORS(X) \
    X(AppendText, 0) X(DeleteGraphemeBackward, 1) X(DeleteWordBackward, 2) \
    X(SelectNext, 3) X(SelectPrevious, 4) X(Submit, 5)
#define SSG_CLIENT_INPUT_OUTCOME_ENUMERATORS(X) \
    X(Unhandled, 0) X(ClientOwned, 1) X(Dispatched, 2) X(Rejected, 3) X(ViewOwned, 4)
#define SSG_WIDGET_KIND_ENUMERATORS(X) \
    X(Container, 0) X(Label, 1) X(Field, 2) X(Checkbox, 3) X(TextInput, 4) \
    X(Spacer, 5) X(View, 6) X(StatusActions, 7)
#define SSG_VIEW_SURFACE_ENUMERATORS(X) \
    X(TabBar, 0) X(FindResults, 3) X(Notice, 6) X(ExternalModification, 7) \
    X(Document, 8) X(Tree, 9)
#define SSG_OVERFLOW_ENUMERATORS(X) X(None, 0) X(Truncate, 1) X(ScrollTail, 2)
#define SSG_VIEW_ACTION_KIND_ENUMERATORS(X) \
    X(ScrollLines, 0) X(ScrollPages, 1) X(ScrollFraction, 2) X(MoveVisualSelection, 3) \
    X(RevealSelection, 4) X(CenterSelection, 5) X(SplitPane, 6) X(ClosePane, 7) \
    X(CyclePane, 8) X(FocusPane, 9) X(ContinuePointerEdge, 10)
#define SSG_VIEW_SCROLL_TARGET_ENUMERATORS(X) X(Document, 0) X(Tree, 1)
#define SSG_VISUAL_SELECTION_DIRECTION_ENUMERATORS(X) X(LineUp, 0) X(LineDown, 1) X(PageUp, 2) X(PageDown, 3)
#define SSG_SPLIT_AXIS_ENUMERATORS(X) X(Horizontal, 0) X(Vertical, 1)
#define SSG_PANE_CYCLE_DIRECTION_ENUMERATORS(X) X(Next, 0) X(Previous, 1)
#define SSG_PANE_DIRECTION_ENUMERATORS(X) X(Left, 0) X(Right, 1) X(Up, 2) X(Down, 3)
#define SSG_POINTER_EDGE_DIRECTION_ENUMERATORS(X) X(Before, 0) X(After, 1)
#define SSG_PALETTE_PRESENCE_OP_KIND_ENUMERATORS(X) X(Show, 0) X(Hide, 1)
