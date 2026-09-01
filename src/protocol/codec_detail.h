#pragma once

#include <ssg/Protocol.h>

#include <ssg/CommandCatalog.h>
#include <ssg/CommandSpecBuilder.h>
#include <ssg/FileCommands.h>
#include <ssg/FindReplace.h>
#include <ssg/Keymap.h>
#include <ssg/PaletteProtocol.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/PresenceProtocol.h>
#include <ssg/Selection.h>
#include <ssg/Settings.h>
#include <ssg/TextCodec.h>
#include <ssg/TextInputCommands.h>
#include <ssg/UiStateProtocol.h>
#include <ssg/UiTreeProtocol.h>
#include <ssg/detail/generated/wire_schema.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ssg::protocol_detail {

struct DecodedMessage {
    ProtocolError error;
    std::optional<ProtocolValue> payload;
    std::string message;
};

[[nodiscard]] std::string encodeMessage(
    ProtocolMessageKind kind, ProtocolValue const& payload);
[[nodiscard]] DecodedMessage decodeMessage(
    std::string_view bytes, ProtocolMessageKind expectedKind,
    ProtocolLimits const& limits);

//
// Every leaf/composite type reachable from SessionSnapshotSections and
// SessionDelta has a
// toValue()/decodePresent() pair, plumbed through one generic fromValue<T>
// entry point defined once below.
//
// Rationale for the decodePresent() split: several domain types (DiffFileId,
// TreeNodeId, TreeProviderId, LanguageId, TreeRevision, ViewportDimensions,
// ScrollFractionArguments, SelectionSet and therefore SelectionViewState,
// JournalDocumentKey, and every aggregate embedding one of these) have no
// default constructor, so a generic "T out{}; decode into out" pattern does
// not compile. Every decodePresent() overload instead receives a
// std::optional<T>& and constructs the result in place via out.emplace(...)
// from already-decoded parts -- never a bare default-constructed T. The
// single generic fromValue<T>() wrapper interprets a wire null as "field is
// legitimately absent" (out.reset(); return true) before delegating to
// decodePresent() for the non-null case. This one signature serves both:
//   - decoding a genuinely domain-optional field (std::optional<T> in the
//     C++ struct): the caller keeps the resulting std::optional<T> as-is.
//   - decoding a domain-required field (plain T in the C++ struct): the
//     caller uses require_field<T>() (below), which treats an empty result
//     -- whether from a malformed payload or an unexpected wire null in a
//     required-field position -- as a decode failure.
//
// Ordering constraint: dependent calls in the generic templates only see the
// overload set declared before their definitions. Keep every typed conversion
// declaration above those templates as codecs move between translation units.
ProtocolValue toValue(bool value);
bool decodePresent(ProtocolValue const& value, std::optional<bool>& out);
ProtocolValue toValue(std::uint8_t value);
bool decodePresent(ProtocolValue const& value, std::optional<std::uint8_t>& out);
ProtocolValue toValue(std::uint32_t value);
bool decodePresent(ProtocolValue const& value, std::optional<std::uint32_t>& out);
ProtocolValue toValue(std::uint64_t value);
bool decodePresent(ProtocolValue const& value, std::optional<std::uint64_t>& out);
ProtocolValue toValue(std::int64_t value);
bool decodePresent(ProtocolValue const& value, std::optional<std::int64_t>& out);
ProtocolValue toValue(int value);
bool decodePresent(ProtocolValue const& value, std::optional<int>& out);
ProtocolValue toValue(std::string const& value);
bool decodePresent(ProtocolValue const& value, std::optional<std::string>& out);
ProtocolValue toValue(std::vector<std::uint8_t> const& value);
bool decodePresent(ProtocolValue const& value,
                    std::optional<std::vector<std::uint8_t>>& out);
ProtocolValue toValue(std::filesystem::path const& value);
bool decodePresent(ProtocolValue const& value,
                    std::optional<std::filesystem::path>& out);

template <typename Enum, typename = std::enable_if_t<std::is_enum_v<Enum>>>
ProtocolValue toValue(Enum value) {
    return ProtocolValue::makeUint(static_cast<std::uint64_t>(
        static_cast<std::underlying_type_t<Enum>>(value)));
}

// (toValue(Enum) is served generically above; only decodePresent needs a
// forward declaration per enum, each implemented via decode_enum().)
bool decodePresent(ProtocolValue const& value, std::optional<DocumentMode>& out);
bool decodePresent(ProtocolValue const& value, std::optional<StatusPriority>& out);
bool decodePresent(ProtocolValue const& value, std::optional<PromptKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<PromptControlKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<SearchMode>& out);
bool decodePresent(ProtocolValue const& value, std::optional<FindReplaceError>& out);
bool decodePresent(ProtocolValue const& value, std::optional<SettingScope>& out);
bool decodePresent(ProtocolValue const& value, std::optional<SettingKey>& out);
bool decodePresent(ProtocolValue const& value, std::optional<TextEncoding>& out);
bool decodePresent(ProtocolValue const& value, std::optional<IndentStyle>& out);
bool decodePresent(ProtocolValue const& value, std::optional<LineEnding>& out);
bool decodePresent(ProtocolValue const& value, std::optional<TabKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<TabRecoveryBadge>& out);
bool decodePresent(ProtocolValue const& value, std::optional<JournalDocumentKeyKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<DiffLineKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<DiffFileStatus>& out);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalAction>& out);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalDocumentStatus>& out);
bool decodePresent(ProtocolValue const& value, std::optional<FollowMode>& out);
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<GitTreeStatus>& out);
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxScope>& out);
bool decodePresent(ProtocolValue const& value, std::optional<BracketKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<BracketRole>& out);
bool decodePresent(ProtocolValue const& value, std::optional<CommentKind>& out);
bool decodePresent(ProtocolValue const& value, std::optional<CommentTokenRole>& out);
bool decodePresent(ProtocolValue const& value, std::optional<LspDiagnosticSeverity>& out);
bool decodePresent(ProtocolValue const& value, std::optional<FocusTarget>& out);
bool decodePresent(ProtocolValue const& value, std::optional<SemanticRole>& out);
bool decodePresent(ProtocolValue const& value, std::optional<ClientInputKind>& out);
bool decodePresent(ProtocolValue const& value,
                   std::optional<InputPointerButton>& out);
bool decodePresent(ProtocolValue const& value,
                   std::optional<InputPointerPhase>& out);
bool decodePresent(ProtocolValue const& value,
                   std::optional<DocumentPointerEdge>& out);
bool decodePresent(ProtocolValue const& value,
                   std::optional<SemanticScrollTarget>& out);

ProtocolValue toValue(Revision const& value);
bool decodePresent(ProtocolValue const& value, std::optional<Revision>& out);
ProtocolValue toValue(ByteOffset const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ByteOffset>& out);
ProtocolValue toValue(LineIndex const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LineIndex>& out);
ProtocolValue toValue(CellIndex const& value);
bool decodePresent(ProtocolValue const& value, std::optional<CellIndex>& out);
ProtocolValue toValue(ClientId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ClientId>& out);
ProtocolValue toValue(ViewId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ViewId>& out);
ProtocolValue toValue(WorkspaceId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceId>& out);
ProtocolValue toValue(CapabilityId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<CapabilityId>& out);
ProtocolValue toValue(StatusId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<StatusId>& out);
ProtocolValue toValue(TabId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TabId>& out);
ProtocolValue toValue(FileDocumentId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FileDocumentId>& out);
ProtocolValue toValue(DiffFileId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffFileId>& out);
ProtocolValue toValue(PaneId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PaneId>& out);
ProtocolValue toValue(TreeProviderId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderId>& out);
ProtocolValue toValue(TreeProviderBinding const& value);
bool decodePresent(ProtocolValue const& value,
                   std::optional<TreeProviderBinding>& out);
ProtocolValue toValue(TreeNodeId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeId>& out);
ProtocolValue toValue(TreeRevision const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeRevision>& out);
ProtocolValue toValue(LanguageId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LanguageId>& out);
ProtocolValue toValue(UntitledDocumentId const& value);
bool decodePresent(ProtocolValue const& value, std::optional<UntitledDocumentId>& out);
ProtocolValue toValue(JournalDocumentKey const& value);
bool decodePresent(ProtocolValue const& value, std::optional<JournalDocumentKey>& out);

ProtocolValue toValue(DocumentPosition const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DocumentPosition>& out);
ProtocolValue toValue(DocumentViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DocumentViewState>& out);
ProtocolValue toValue(DocumentDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DocumentDelta>& out);
ProtocolValue toValue(Selection const& value);
bool decodePresent(ProtocolValue const& value, std::optional<Selection>& out);
ProtocolValue toValue(SelectionSet const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionSet>& out);
ProtocolValue toValue(SelectionViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionViewState>& out);
ProtocolValue toValue(SelectionViewDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionViewDelta>& out);
ProtocolValue toValue(SelectionNavigation const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionNavigation>& out);
ProtocolValue toValue(SelectionNavigationDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionNavigationDelta>& out);
ProtocolValue toValue(SelectionSetDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionSetDelta>& out);
ProtocolValue toValue(NoticeViewSectionDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<NoticeViewSectionDelta>& out);
ProtocolValue toValue(HistoryViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<HistoryViewState>& out);
ProtocolValue toValue(HistoryDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<HistoryDelta>& out);
ProtocolValue toValue(ClipboardWrite const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ClipboardWrite>& out);
ProtocolValue toValue(ClipboardViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ClipboardViewState>& out);
ProtocolValue toValue(ClipboardDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ClipboardDelta>& out);
ProtocolValue toValue(StatusAction const& value);
bool decodePresent(ProtocolValue const& value, std::optional<StatusAction>& out);
ProtocolValue toValue(StatusItemView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<StatusItemView>& out);
ProtocolValue toValue(StatusViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<StatusViewState>& out);
ProtocolValue toValue(Rect const& value);
bool decodePresent(ProtocolValue const& value, std::optional<Rect>& out);
ProtocolValue toValue(PromptControl const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptControl>& out);
ProtocolValue toValue(PromptView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptView>& out);
ProtocolValue toValue(NoticeAction const& value);
bool decodePresent(ProtocolValue const& value, std::optional<NoticeAction>& out);
ProtocolValue toValue(NoticeView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<NoticeView>& out);
ProtocolValue toValue(PromptStatusViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptStatusViewState>& out);
ProtocolValue toValue(PromptStatusDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptStatusDelta>& out);
ProtocolValue toValue(SearchResult const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SearchResult>& out);
ProtocolValue toValue(SearchViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SearchViewState>& out);
ProtocolValue toValue(ByteRange const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ByteRange>& out);
ProtocolValue toValue(FindOptions const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FindOptions>& out);
ProtocolValue toValue(FindRequest const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FindRequest>& out);
ProtocolValue toValue(FindMatch const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FindMatch>& out);
ProtocolValue toValue(WorkspaceFileReplacement const& value);
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceFileReplacement>& out);
ProtocolValue toValue(WorkspaceReplacePreview const& value);
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceReplacePreview>& out);
ProtocolValue toValue(FindReplaceViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FindReplaceViewState>& out);
ProtocolValue toValue(SettingValue const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingValue>& out);
ProtocolValue toValue(EffectiveSetting const& value);
bool decodePresent(ProtocolValue const& value, std::optional<EffectiveSetting>& out);
ProtocolValue toValue(SettingViewEntry const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingViewEntry>& out);
ProtocolValue toValue(SettingsViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingsViewState>& out);
ProtocolValue toValue(SettingsDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingsDelta>& out);
ProtocolValue toValue(SettingsSectionDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingsSectionDelta>& out);
ProtocolValue toValue(KeyStroke const& value);
bool decodePresent(ProtocolValue const& value, std::optional<KeyStroke>& out);
ProtocolValue toValue(KeyBinding const& value);
bool decodePresent(ProtocolValue const& value, std::optional<KeyBinding>& out);
ProtocolValue toValue(KeymapViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<KeymapViewState>& out);
ProtocolValue toValue(KeymapDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<KeymapDelta>& out);
ProtocolValue toValue(TextEncodingStatus const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TextEncodingStatus>& out);
ProtocolValue toValue(TextEncodingViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TextEncodingViewState>& out);
ProtocolValue toValue(TabState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TabState>& out);
ProtocolValue toValue(TabViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TabViewState>& out);
ProtocolValue toValue(DiffHunk const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffHunk>& out);
ProtocolValue toValue(DiffWordRange const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffWordRange>& out);
ProtocolValue toValue(DiffLineChange const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffLineChange>& out);
ProtocolValue toValue(DiffFileView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffFileView>& out);
ProtocolValue toValue(DiffViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffViewState>& out);
ProtocolValue toValue(DiffDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DiffDelta>& out);
ProtocolValue toValue(ExternalDocumentView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalDocumentView>& out);
ProtocolValue toValue(ExternalActionAffordance const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalActionAffordance>& out);
ProtocolValue toValue(ExternalModificationViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalModificationViewState>& out);
ProtocolValue toValue(ExternalModificationDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalModificationDelta>& out);
ProtocolValue toValue(ViewportDimensions const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ViewportDimensions>& out);
ProtocolValue toValue(FollowScrollOffset const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FollowScrollOffset>& out);
ProtocolValue toValue(FollowTarget const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FollowTarget>& out);
ProtocolValue toValue(FollowClientView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FollowClientView>& out);
ProtocolValue toValue(FollowEditsViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FollowEditsViewState>& out);
ProtocolValue toValue(ResolvedSelectionRange const& value);
bool decodePresent(ProtocolValue const& value,
                   std::optional<ResolvedSelectionRange>& out);
ProtocolValue toValue(TreeNodeCommand const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeCommand>& out);
ProtocolValue toValue(GitTreeAffordance const& value);
bool decodePresent(ProtocolValue const& value, std::optional<GitTreeAffordance>& out);
ProtocolValue toValue(TreeNode const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeNode>& out);
ProtocolValue toValue(TreeNodeView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeNodeView>& out);
ProtocolValue toValue(TreeProviderView const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderView>& out);
ProtocolValue toValue(TreeWindow const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeWindow>& out);
ProtocolValue toValue(TreeViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeViewState>& out);
ProtocolValue toValue(TreeProviderDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeProviderDelta>& out);
ProtocolValue toValue(TreeDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeDelta>& out);
ProtocolValue toValue(SyntaxSpan const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxSpan>& out);
ProtocolValue toValue(SyntaxBracketPair const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxBracketPair>& out);
ProtocolValue toValue(UnmatchedBracket const& value);
bool decodePresent(ProtocolValue const& value, std::optional<UnmatchedBracket>& out);
ProtocolValue toValue(SyntaxRange const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxRange>& out);
ProtocolValue toValue(CommentToken const& value);
bool decodePresent(ProtocolValue const& value, std::optional<CommentToken>& out);
ProtocolValue toValue(CommentRange const& value);
bool decodePresent(ProtocolValue const& value, std::optional<CommentRange>& out);
ProtocolValue toValue(LineIndentation const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LineIndentation>& out);
ProtocolValue toValue(SyntaxViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SyntaxViewState>& out);
ProtocolValue toValue(LspPosition const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspPosition>& out);
ProtocolValue toValue(LspRange const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspRange>& out);
ProtocolValue toValue(LspDiagnostic const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspDiagnostic>& out);
ProtocolValue toValue(LspDocumentDiagnostics const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspDocumentDiagnostics>& out);
ProtocolValue toValue(LspSyncViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspSyncViewState>& out);
ProtocolValue toValue(LspCompletionItem const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspCompletionItem>& out);
ProtocolValue toValue(LspCompletionViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspCompletionViewState>& out);
ProtocolValue toValue(LspHover const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspHover>& out);
ProtocolValue toValue(LspNavigationTarget const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspNavigationTarget>& out);
ProtocolValue toValue(LspNavigationViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspNavigationViewState>& out);
ProtocolValue toValue(LspFeatureViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<LspFeatureViewState>& out);
ProtocolValue toValue(SrgbColor const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SrgbColor>& out);
ProtocolValue toValue(ThemeSnapshot const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ThemeSnapshot>& out);
ProtocolValue toValue(Style const& value);
bool decodePresent(ProtocolValue const& value, std::optional<Style>& out);
ProtocolValue toValue(ThemeSectionDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ThemeSectionDelta>& out);
ProtocolValue toValue(GridSize const& value);
bool decodePresent(ProtocolValue const& value, std::optional<GridSize>& out);
ProtocolValue toValue(VisualRow const& value);
bool decodePresent(ProtocolValue const& value, std::optional<VisualRow>& out);
ProtocolValue toValue(ProjectedRow const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ProjectedRow>& out);
ProtocolValue toValue(CellHitTarget const& value);
bool decodePresent(ProtocolValue const& value, std::optional<CellHitTarget>& out);
ProtocolValue toValue(ScrollbarMetrics const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ScrollbarMetrics>& out);
ProtocolValue toValue(ViewportViewState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ViewportViewState>& out);
ProtocolValue toValue(ViewportDelta const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ViewportDelta>& out);
ProtocolValue toValue(SessionTopology const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SessionTopology>& out);
ProtocolValue toValue(ClientSnapshotState const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ClientSnapshotState>& out);
ProtocolValue toValue(SessionSnapshotSections const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SessionSnapshotSections>& out);

ProtocolValue toValue(TextInputArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TextInputArguments>& out);
ProtocolValue toValue(PaletteExecuteArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PaletteExecuteArguments>& out);
ProtocolValue toValue(PickerSubmitArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PickerSubmitArguments>& out);
ProtocolValue toValue(TreeSelectArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<TreeSelectArguments>& out);
ProtocolValue toValue(ExternalActionInvocation const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ExternalActionInvocation>& out);
ProtocolValue toValue(FindQueryArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<FindQueryArguments>& out);
ProtocolValue toValue(PromptValueArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptValueArguments>& out);
ProtocolValue toValue(PromptFocusArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<PromptFocusArguments>& out);
ProtocolValue toValue(UiNodeActivationArguments const& value);
bool decodePresent(ProtocolValue const& value,
                   std::optional<UiNodeActivationArguments>& out);
ProtocolValue toValue(SelectionCommandArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SelectionCommandArguments>& out);
ProtocolValue toValue(ScrollLinesArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ScrollLinesArguments>& out);
ProtocolValue toValue(ScrollPagesArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ScrollPagesArguments>& out);
ProtocolValue toValue(ScrollFractionArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ScrollFractionArguments>& out);
ProtocolValue toValue(DroppedContentArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<DroppedContentArguments>& out);
ProtocolValue toValue(ReopenWithEncodingArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<ReopenWithEncodingArguments>& out);
ProtocolValue toValue(SetEncodingArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SetEncodingArguments>& out);
ProtocolValue toValue(SetLineEndingArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SetLineEndingArguments>& out);
ProtocolValue toValue(SetFinalNewlineArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SetFinalNewlineArguments>& out);
ProtocolValue toValue(SettingSetArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingSetArguments>& out);
ProtocolValue toValue(SettingResetArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingResetArguments>& out);
ProtocolValue toValue(SettingResetScopeArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<SettingResetScopeArguments>& out);
ProtocolValue toValue(WorkspaceReplaceArguments const& value);
bool decodePresent(ProtocolValue const& value, std::optional<WorkspaceReplaceArguments>& out);

template <typename T>
ProtocolValue toValue(std::optional<T> const& value);
template <typename T>
ProtocolValue toValue(std::vector<T> const& value);
template <typename T, std::size_t N>
ProtocolValue toValue(std::array<T, N> const& value);

template <typename T>
[[nodiscard]] bool fromValue(ProtocolValue const& value, std::optional<T>& out);
template <typename T>
bool decodePresent(ProtocolValue const& value, std::optional<std::vector<T>>& out);
template <typename T, std::size_t N>
bool decodePresent(ProtocolValue const& value, std::optional<std::array<T, N>>& out);

template <typename T>
ProtocolValue toValue(std::optional<T> const& value) {
    if (!value) {
        return ProtocolValue::makeNull();
    }
    return toValue(*value);
}

template <typename T>
ProtocolValue toValue(std::vector<T> const& value) {
    std::vector<ProtocolValue> items;
    items.reserve(value.size());
    for (auto const& item : value) {
        items.push_back(toValue(item));
    }
    return ProtocolValue::makeArray(std::move(items));
}

template <typename T, std::size_t N>
ProtocolValue toValue(std::array<T, N> const& value) {
    std::vector<ProtocolValue> items;
    items.reserve(N);
    for (auto const& item : value) {
        items.push_back(toValue(item));
    }
    return ProtocolValue::makeArray(std::move(items));
}

template <typename T>
[[nodiscard]] bool fromValue(ProtocolValue const& value, std::optional<T>& out) {
    if (value.kind() == ProtocolValue::Kind::NullValue) {
        out.reset();
        return true;
    }
    try {
        return decodePresent(value, out);
    } catch (std::invalid_argument const&) {
        // Domain constructors enforce invariants for trusted in-process
        // callers. Invalid wire values are ordinary decode failures, not
        // exceptions escaping into the transport.
        out.reset();
        return false;
    }
}

template <typename T>
bool decodePresent(ProtocolValue const& value, std::optional<std::vector<T>>& out) {
    auto const* items = value.asArray();
    if (items == nullptr) {
        return false;
    }
    std::vector<T> result;
    result.reserve(items->size());
    for (auto const& item : *items) {
        std::optional<T> decoded;
        if (!fromValue(item, decoded) || !decoded.has_value()) {
            return false;
        }
        result.push_back(std::move(*decoded));
    }
    out.emplace(std::move(result));
    return true;
}

template <typename T, std::size_t N>
bool decodePresent(ProtocolValue const& value, std::optional<std::array<T, N>>& out) {
    auto const* items = value.asArray();
    if (items == nullptr || items->size() != N) {
        return false;
    }
    std::array<T, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        std::optional<T> decoded;
        if (!fromValue((*items)[i], decoded) || !decoded.has_value()) {
            return false;
        }
        result[i] = std::move(*decoded);
    }
    out.emplace(std::move(result));
    return true;
}

// Unwraps a required (non-optional) field: a null field pointer (absent from
// the wire object), a decode failure, and an explicit wire null all converge
// to std::nullopt, so composite decoders can uniformly reject missing or
// malformed required data with a single `if (!field) return false;`.
template <typename T>
[[nodiscard]] std::optional<T> requireField(ProtocolValue const* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    std::optional<T> decoded;
    if (!fromValue(*value, decoded)) {
        return std::nullopt;
    }
    return decoded;
}

// Decodes a domain-optional field: a null field pointer (absent from the
// wire object) is a legitimate absence (out.reset(), success); a present
// field delegates to fromValue(), which itself treats an explicit wire null
// as absence and rejects malformed data. Unlike require_field(), a
// genuinely-absent field is not an error here.
template <typename T>
[[nodiscard]] bool decodeOptionalField(ProtocolValue const* fieldValue,
                                         std::optional<T>& out) {
    if (fieldValue == nullptr) {
        out.reset();
        return true;
    }
    return fromValue(*fieldValue, out);
}

template <typename Enum, std::size_t N>
[[nodiscard]] bool decodeEnum(ProtocolValue const& value, std::optional<Enum>& out,
                               std::array<Enum, N> const& validValues) {
    auto raw = value.asUint();
    if (!raw) {
        return false;
    }
    for (Enum candidate : validValues) {
        if (static_cast<std::uint64_t>(
                static_cast<std::underlying_type_t<Enum>>(candidate)) ==
            *raw) {
            out.emplace(candidate);
            return true;
        }
    }
    return false;
}

template <typename Enum, std::size_t N>
[[nodiscard]] bool decodeEnum(
    ProtocolValue const& value, std::optional<Enum>& out,
    std::array<detail::generated::WireEnumValueFact, N> const& validValues) {
    auto raw = value.asUint();
    if (!raw) return false;
    for (auto const& candidate : validValues) {
        if (candidate.ordinal == *raw) {
            out.emplace(static_cast<Enum>(*raw));
            return true;
        }
    }
    return false;
}

[[nodiscard]] ProtocolValue encodeUiFrame(UiFrame const& value);
[[nodiscard]] std::optional<UiFrame> decodeUiFrame(
    ProtocolValue const& value);
[[nodiscard]] ProtocolValue encodeUiFrameDelta(UiFrameDelta const& value);
[[nodiscard]] std::optional<UiFrameDelta> decodeUiFrameDelta(
    ProtocolValue const& value);

}  // namespace ssg::protocol_detail
