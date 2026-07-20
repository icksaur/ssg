#include <ssg/session_snapshot.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

bool shellEqual(ShellViewState const& left, ShellViewState const& right) {
    return left.viewport == right.viewport && left.header == right.header &&
           left.footer == right.footer && left.tab_bar == right.tab_bar &&
           left.panel == right.panel &&
           left.panel_scrollbar == right.panel_scrollbar &&
           left.prompt == right.prompt &&
           left.panes == right.panes &&
           left.tab_hits == right.tab_hits &&
           left.accessibility_nodes == right.accessibility_nodes;
}

SelectionViewDelta selectionDelta(SelectionViewState const& before,
                                   SelectionViewState const& after) {
    bool const changed = before != after;
    return {changed, changed ? std::optional{after} : std::nullopt};
}

SettingsSectionDelta settingsDelta(SettingsViewState const& before,
                                    SettingsViewState const& after) {
    SettingsSectionDelta result;
    for (std::size_t index = 0; index < before.entries.size(); ++index) {
        auto const& oldEntry = before.entries[index];
        auto const& newEntry = after.entries[index];
        if (oldEntry != newEntry) {
            if (oldEntry.key != newEntry.key) {
                throw std::invalid_argument{
                    "settings snapshots have incompatible key order"};
            }
            result.changes.push_back(
                {oldEntry.key, oldEntry.effective, newEntry.effective});
        }
    }
    return result;
}

std::optional<SettingsViewState> replaySettings(
    SettingsViewState state, SettingsSectionDelta const& delta) {
    for (auto const& change : delta.changes) {
        auto found = std::find_if(
            state.entries.begin(), state.entries.end(),
            [&](SettingViewEntry const& entry) {
                return entry.key == change.key;
            });
        if (found == state.entries.end() || found->effective != change.before) {
            return std::nullopt;
        }
        found->effective = change.after;
    }
    return state;
}

template <typename State, typename Delta>
std::optional<State> replayReplacement(State const& state,
                                        Delta const& delta) {
    if (delta.changed != delta.replacement.has_value()) {
        return std::nullopt;
    }
    return delta.replacement ? delta.replacement : std::optional<State>{state};
}

}  // namespace

bool operator==(SessionSnapshotSections const& left,
                SessionSnapshotSections const& right) {
    return left.document == right.document &&
           left.selection == right.selection && left.history == right.history &&
           left.clipboard == right.clipboard &&
           left.prompt_status == right.prompt_status &&
           left.search == right.search &&
           left.find_replace == right.find_replace &&
           left.settings == right.settings && left.keymap == right.keymap &&
           left.text_encoding == right.text_encoding &&
           left.tabs == right.tabs && left.diff == right.diff &&
           left.external_modification == right.external_modification &&
           left.follow_edits == right.follow_edits &&
           left.tree == right.tree && left.syntax == right.syntax &&
           left.lsp_sync == right.lsp_sync &&
           left.lsp_features == right.lsp_features &&
           left.theme == right.theme && shellEqual(left.shell, right.shell) &&
           left.palette == right.palette;
}

SessionSnapshot::SessionSnapshot(Revision revision, SessionTopology topology,
                                 ClientSnapshotState client,
                                 SessionSnapshotSections sections)
    : revision_{revision},
      topology_{std::move(topology)},
      client_{std::move(client)},
      sections_{std::move(sections)} {}

bool SessionSnapshot::operator==(SessionSnapshot const& other) const {
    return revision_ == other.revision_ && topology_ == other.topology_ &&
           client_ == other.client_ && sections_ == other.sections_;
}

SessionDelta::SessionDelta(
    Revision baseRevision, Revision revision, ClientId clientId,
    ViewId viewId, std::vector<CapabilityId> capabilities,
    std::optional<SessionTopology> topology,
    std::optional<DocumentDelta> document,
    std::optional<ByteOffset> documentCaret, SelectionViewDelta selection,
    HistoryDelta history, ClipboardDelta clipboard,
    PromptStatusDelta promptStatus, SearchDelta search,
    FindReplaceDelta findReplace, SettingsSectionDelta settings,
    KeymapDelta keymap, std::optional<TextEncodingDelta> textEncoding,
    TabDelta tabs, DiffDelta diff,
    ExternalModificationDelta externalModification,
    FollowEditsDelta followEdits, TreeDelta tree, SyntaxDelta syntax,
    LspSyncDelta lspSync, LspFeatureDelta lspFeatures,
    ThemeSectionDelta theme, ShellSectionDelta shell, ViewportDelta viewport)
    : base_revision_{baseRevision},
      revision_{revision},
      client_id_{clientId},
      view_id_{viewId},
      capabilities_{std::move(capabilities)},
      topology_{std::move(topology)},
      document_{std::move(document)},
      document_caret_{documentCaret},
      selection_{std::move(selection)},
      history_{std::move(history)},
      clipboard_{std::move(clipboard)},
      prompt_status_{std::move(promptStatus)},
      search_{std::move(search)},
      find_replace_{std::move(findReplace)},
      settings_{std::move(settings)},
      keymap_{std::move(keymap)},
      text_encoding_{std::move(textEncoding)},
      tabs_{std::move(tabs)},
      diff_{std::move(diff)},
      external_modification_{std::move(externalModification)},
      follow_edits_{std::move(followEdits)},
      tree_{std::move(tree)},
      syntax_{std::move(syntax)},
      lsp_sync_{std::move(lspSync)},
      lsp_features_{std::move(lspFeatures)},
      theme_{std::move(theme)},
      shell_{std::move(shell)},
      viewport_{std::move(viewport)} {}

SessionSnapshot assembleSessionSnapshot(
    Revision revision, SessionTopology topology,
    InvocationPrincipal const& principal, ViewId viewId,
    ViewportViewState viewport, SessionSnapshotSections sections) {
    return {revision,
            std::move(topology),
            {principal.clientId(), viewId, principal.capabilities(),
             std::move(viewport)},
            std::move(sections)};
}

SessionDelta deriveSessionDelta(SessionSnapshot const& before,
                                  SessionSnapshot const& after) {
    if (before.client().client_id != after.client().client_id ||
        before.client().view_id != after.client().view_id ||
        before.client().capabilities != after.client().capabilities) {
        throw std::invalid_argument{
            "session deltas require one immutable client attachment"};
    }
    if (after.revision() < before.revision() ||
        before.revision() == after.revision()) {
        throw std::invalid_argument{"session delta revisions must advance"};
    }
    auto document =
        deriveDocumentDelta(before.sections().document,
                              after.sections().document);
    auto const& old = before.sections();
    auto const& next = after.sections();
    if (!document &&
        (old.document.revision != next.document.revision ||
         old.document.text != next.document.text)) {
        throw std::invalid_argument{
            "document content changed without a new document revision"};
    }
    return {
        before.revision(),
        after.revision(),
        before.client().client_id,
        before.client().view_id,
        before.client().capabilities,
        before.topology() == after.topology()
            ? std::nullopt
            : std::optional{after.topology()},
        std::move(document),
        old.document.caret == next.document.caret
            ? std::nullopt
            : std::optional{next.document.caret},
        selectionDelta(old.selection, next.selection),
        deriveHistoryDelta(old.history, next.history),
        deriveClipboardDelta(old.clipboard, next.clipboard),
        derivePromptStatusDelta(old.prompt_status, next.prompt_status),
        deriveSearchDelta(old.search, next.search),
        deriveFindReplaceDelta(old.find_replace, next.find_replace),
        settingsDelta(old.settings, next.settings),
        deriveKeymapDelta(old.keymap, next.keymap),
        deriveTextEncodingDelta(old.text_encoding, next.text_encoding),
        deriveTabDelta(old.tabs, next.tabs),
        deriveDiffDelta(old.diff, next.diff),
        deriveExternalModificationDelta(old.external_modification,
                                            next.external_modification),
        deriveFollowEditsDelta(old.follow_edits, next.follow_edits),
        deriveTreeDelta(old.tree, next.tree, 4096),
        deriveSyntaxDelta(old.syntax, next.syntax),
        deriveLspSyncDelta(old.lsp_sync, next.lsp_sync),
        deriveLspFeatureDelta(old.lsp_features, next.lsp_features),
        {old.theme == next.theme ? std::nullopt
                                 : std::optional{next.theme}},
        {shellEqual(old.shell, next.shell)
             ? std::nullopt
             : std::optional{next.shell}},
        deriveViewportDelta(before.client().viewport,
                              after.client().viewport),
    };
}

SessionReplayResult replaySessionDelta(SessionSnapshot const& base,
                                         SessionDelta const& delta) {
    if (base.revision() != delta.base_revision_ ||
        delta.revision_ <= delta.base_revision_ ||
        base.client().client_id != delta.client_id_ ||
        base.client().view_id != delta.view_id_ ||
        base.client().capabilities != delta.capabilities_) {
        return {std::nullopt, "session delta base revision mismatch"};
    }

    auto document = std::optional<DocumentViewState>{base.sections().document};
    if (delta.document_) {
        document = replayDocumentDelta(
            base.sections().document, *delta.document_,
            delta.document_caret_.value_or(base.sections().document.caret));
    } else if (delta.document_caret_) {
        if (delta.document_caret_->value() > document->text.size()) {
            return {std::nullopt, "document caret is out of bounds"};
        }
        document->caret = *delta.document_caret_;
    }
    auto selection = replayReplacement(base.sections().selection,
                                        delta.selection_);
    auto history = replayReplacement(base.sections().history, delta.history_);
    auto clipboard =
        replayReplacement(base.sections().clipboard, delta.clipboard_);
    auto promptStatus = replayReplacement(base.sections().prompt_status,
                                            delta.prompt_status_);
    auto search = replaySearchDelta(base.sections().search, delta.search_);
    auto findReplace = replayFindReplaceDelta(base.sections().find_replace,
                                                  delta.find_replace_);
    auto settings = replaySettings(base.sections().settings, delta.settings_);
    auto keymap = replayReplacement(base.sections().keymap, delta.keymap_);
    auto tabs = replayTabDelta(base.sections().tabs, delta.tabs_);
    auto diff = replayDiffDelta(base.sections().diff, delta.diff_);
    auto external = replayExternalModificationDelta(
        base.sections().external_modification,
        delta.external_modification_);
    auto tree = replayTreeDelta(base.sections().tree, delta.tree_);
    auto syntax = replaySyntaxDelta(base.sections().syntax, delta.syntax_);
    auto lspSync =
        replayLspSyncDelta(base.sections().lsp_sync, delta.lsp_sync_);
    auto lspFeatures = replayLspFeatureDelta(
        base.sections().lsp_features, delta.lsp_features_);

    if (!document || !selection || !history || !clipboard || !promptStatus ||
        !search.accepted() ||
        findReplace.error != FindReplaceReplayError::None || !settings ||
        !keymap || !tabs.accepted() || !diff.accepted() ||
        !external.accepted() || !tree.accepted() || !syntax.accepted() ||
        !lspSync.accepted() || !lspFeatures.accepted()) {
        return {std::nullopt, "malformed feature delta"};
    }

    auto textEncoding = base.sections().text_encoding;
    if (delta.text_encoding_) {
        if (delta.text_encoding_->before != textEncoding) {
            return {std::nullopt, "text encoding delta base mismatch"};
        }
        textEncoding = delta.text_encoding_->after;
    }

    auto followEdits = base.sections().follow_edits;
    if (delta.follow_edits_.base_generation != followEdits.generation ||
        (delta.follow_edits_.replacement &&
         (delta.follow_edits_.replacement->generation !=
              delta.follow_edits_.generation ||
          delta.follow_edits_.generation <
              delta.follow_edits_.base_generation)) ||
        (!delta.follow_edits_.replacement &&
         delta.follow_edits_.generation !=
             delta.follow_edits_.base_generation)) {
        return {std::nullopt, "follow-edits delta base mismatch"};
    }
    if (delta.follow_edits_.replacement) {
        followEdits = *delta.follow_edits_.replacement;
    }

    auto theme = delta.theme_.replacement.value_or(base.sections().theme);
    auto shell = delta.shell_.replacement.value_or(base.sections().shell);
    auto viewport = delta.viewport_.replacement.value_or(
        base.client().viewport);
    // The published palette candidate list is authoritative server state that the
    // delta does not carry (the P0 command catalog is static within a session), so
    // preserve it from the base rather than dropping it -- otherwise a
    // delta-replaying client loses its command catalog and diverges from a fresh
    // snapshot.
    auto palette = base.sections().palette;

    SessionSnapshotSections sections{
        std::move(*document),
        std::move(*selection),
        std::move(*history),
        std::move(*clipboard),
        std::move(*promptStatus),
        std::move(*search.state),
        std::move(findReplace.state),
        std::move(*settings),
        std::move(*keymap),
        std::move(textEncoding),
        std::move(*tabs.state),
        std::move(*diff.state),
        std::move(*external.state),
        std::move(followEdits),
        std::move(*tree.state),
        std::move(*syntax.state),
        std::move(*lspSync.state),
        std::move(*lspFeatures.state),
        std::move(theme),
        std::move(shell),
        std::move(palette),
    };
    ClientSnapshotState client = base.client();
    client.viewport = std::move(viewport);
    return {SessionSnapshot{
                delta.revision_,
                delta.topology_.value_or(base.topology()),
                std::move(client),
                std::move(sections)},
            {}};
}

SessionDelta decodeWireSessionDelta(
    Revision baseRevision, Revision revision, ClientId clientId,
    ViewId viewId, std::vector<CapabilityId> capabilities,
    std::optional<SessionTopology> topology,
    std::optional<DocumentDelta> document,
    std::optional<ByteOffset> documentCaret, SelectionViewDelta selection,
    HistoryDelta history, ClipboardDelta clipboard,
    PromptStatusDelta promptStatus, SearchDelta search,
    FindReplaceDelta findReplace, SettingsSectionDelta settings,
    KeymapDelta keymap, std::optional<TextEncodingDelta> textEncoding,
    TabDelta tabs, DiffDelta diff,
    ExternalModificationDelta externalModification,
    FollowEditsDelta followEdits, TreeDelta tree, SyntaxDelta syntax,
    LspSyncDelta lspSync, LspFeatureDelta lspFeatures,
    ThemeSectionDelta theme, ShellSectionDelta shell, ViewportDelta viewport) {
    return SessionDelta{baseRevision,
                        revision,
                        clientId,
                        viewId,
                        std::move(capabilities),
                        std::move(topology),
                        std::move(document),
                        documentCaret,
                        std::move(selection),
                        std::move(history),
                        std::move(clipboard),
                        std::move(promptStatus),
                        std::move(search),
                        std::move(findReplace),
                        std::move(settings),
                        std::move(keymap),
                        std::move(textEncoding),
                        std::move(tabs),
                        std::move(diff),
                        std::move(externalModification),
                        std::move(followEdits),
                        std::move(tree),
                        std::move(syntax),
                        std::move(lspSync),
                        std::move(lspFeatures),
                        std::move(theme),
                        std::move(shell),
                        std::move(viewport)};
}

}  // namespace ssg
