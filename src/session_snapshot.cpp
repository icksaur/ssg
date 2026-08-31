#include <ssg/session_snapshot.h>

#include <ssg/PresenceProtocol.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ssg {
namespace {

bool shellEqual(ShellViewState const& left, ShellViewState const& right) {
    return left.viewport == right.viewport && left.header == right.header &&
           left.footer == right.footer && left.tabBar == right.tabBar &&
           left.panel == right.panel &&
           left.panelScrollbar == right.panelScrollbar &&
           left.prompt == right.prompt &&
           left.panes == right.panes &&
           left.tabHits == right.tabHits &&
           left.accessibilityNodes == right.accessibilityNodes &&
           left.palette == right.palette &&
           left.externalActions == right.externalActions;
}

SelectionSetDelta selectionDelta(SelectionSet const& before,
                                 SelectionSet const& after) {
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
           left.promptStatus == right.promptStatus &&
           left.search == right.search &&
           left.findReplace == right.findReplace &&
           left.settings == right.settings && left.keymap == right.keymap &&
           left.textEncoding == right.textEncoding &&
           left.tabs == right.tabs && left.diff == right.diff &&
           left.externalModification == right.externalModification &&
           left.followEdits == right.followEdits &&
           left.tree == right.tree && left.syntax == right.syntax &&
           left.lspSync == right.lspSync &&
           left.lspFeatures == right.lspFeatures &&
           left.theme == right.theme &&
           left.focus == right.focus &&
           left.palette == right.palette &&
           left.uiFrame == right.uiFrame &&
           left.promptView == right.promptView &&
           left.noticeView == right.noticeView &&
           left.watcherAvailable == right.watcherAvailable &&
           left.externalFocusHeld == right.externalFocusHeld;
}

bool PresentationSnapshot::operator==(PresentationSnapshot const& other) const {
    return viewport == other.viewport && style == other.style &&
           prompt == other.prompt && shellEqual(shell, other.shell) &&
           selectionNav == other.selectionNav &&
           treeWindows == other.treeWindows;
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
    std::optional<ByteOffset> documentCaret, SelectionSetDelta selection,
    HistoryDelta history, ClipboardDelta clipboard,
    PromptStatusDelta promptStatus, SearchDelta search,
    FindReplaceDelta findReplace, SettingsSectionDelta settings,
    KeymapDelta keymap, std::optional<TextEncodingDelta> textEncoding,
    TabDelta tabs, DiffDelta diff,
    ExternalModificationDelta externalModification,
    FollowEditsDelta followEdits, TreeDelta tree, SyntaxDelta syntax,
    LspSyncDelta lspSync, LspFeatureDelta lspFeatures,
    ThemeSectionDelta theme, StyleSectionDelta style,
    ShellSectionDelta shell, ViewportDelta viewport,
    UiFrameDelta uiFrameDelta,
    std::optional<FocusTarget> focus, SelectionNavigationDelta selectionNav,
    PromptProjectionDelta promptProjection, TreeWindowsDelta treeWindows,
    PaletteSectionDelta palette,
    PromptViewSectionDelta promptView, NoticeViewSectionDelta noticeView,
    std::optional<bool> watcherAvailable, std::optional<bool> externalFocusHeld)
    : baseRevision_{baseRevision},
      revision_{revision},
      clientId_{clientId},
      viewId_{viewId},
      capabilities_{std::move(capabilities)},
      topology_{std::move(topology)},
      document_{std::move(document)},
      documentCaret_{documentCaret},
      selection_{std::move(selection)},
      history_{std::move(history)},
      clipboard_{std::move(clipboard)},
      promptStatus_{std::move(promptStatus)},
      search_{std::move(search)},
      findReplace_{std::move(findReplace)},
      settings_{std::move(settings)},
      keymap_{std::move(keymap)},
      textEncoding_{std::move(textEncoding)},
      tabs_{std::move(tabs)},
      diff_{std::move(diff)},
      externalModification_{std::move(externalModification)},
      followEdits_{std::move(followEdits)},
      tree_{std::move(tree)},
      syntax_{std::move(syntax)},
      lspSync_{std::move(lspSync)},
      lspFeatures_{std::move(lspFeatures)},
      theme_{std::move(theme)},
      style_{std::move(style)},
      shell_{std::move(shell)},
      viewport_{std::move(viewport)},
      uiFrameDelta_{std::move(uiFrameDelta)},
      focus_{focus},
      selectionNav_{std::move(selectionNav)},
      promptProjection_{std::move(promptProjection)},
      treeWindows_{std::move(treeWindows)},
      palette_{std::move(palette)},
      promptView_{std::move(promptView)},
      noticeView_{std::move(noticeView)},
      watcherAvailable_{watcherAvailable},
      externalFocusHeld_{externalFocusHeld} {}

LegacyPresentationSnapshot SessionSnapshotCodec::assemble(
    Revision revision, SessionTopology topology,
    InvocationPrincipal const& principal, ViewId viewId,
    ViewportViewState viewport, SessionSnapshotSections sections,
    Style style, std::optional<PromptViewState> prompt,
    ShellViewState shell, SelectionNavigation selectionNav,
    std::vector<TreeWindow> treeWindows) const {
    return {
        SessionSnapshot{
            revision, std::move(topology),
            {principal.clientId(), viewId, principal.capabilities()},
            std::move(sections)},
        PresentationSnapshot{std::move(viewport), std::move(style),
                             std::move(prompt), std::move(shell),
                             std::move(selectionNav),
                             std::move(treeWindows)}};
}

SessionDelta SessionSnapshotCodec::deriveDelta(SessionSnapshot const& before,
                                               SessionSnapshot const& after)
    const {
    if (before.client().clientId != after.client().clientId ||
        before.client().viewId != after.client().viewId ||
        before.client().capabilities != after.client().capabilities) {
        throw std::invalid_argument{
            "session deltas require one immutable client attachment"};
    }
    if (after.revision() < before.revision() ||
        before.revision() == after.revision()) {
        throw std::invalid_argument{"session delta revisions must advance"};
    }
    auto document =
        DocumentSnapshotCodec{}.deriveDelta(before.sections().document,
                                           after.sections().document);
    auto const& old = before.sections();
    auto const& next = after.sections();
    if (!document &&
        (old.document.revision != next.document.revision ||
         old.document.text != next.document.text ||
         old.document.diffFileIdentity != next.document.diffFileIdentity)) {
        throw std::invalid_argument{
            "document state changed without a document delta"};
    }
    return {
        before.revision(),
        after.revision(),
        before.client().clientId,
        before.client().viewId,
        before.client().capabilities,
        before.topology() == after.topology()
            ? std::nullopt
            : std::optional{after.topology()},
        std::move(document),
        old.document.caret == next.document.caret
            ? std::nullopt
            : std::optional{next.document.caret},
        selectionDelta(old.selection, next.selection),
        HistoryDeltaCodec{}.derive(old.history, next.history),
        ClipboardDeltaCodec{}.derive(old.clipboard, next.clipboard),
        PromptStatusDeltaCodec{}.derive(old.promptStatus, next.promptStatus),
        SearchDeltaCodec{}.derive(old.search, next.search),
        FindReplaceDeltaCodec{}.derive(old.findReplace, next.findReplace),
        settingsDelta(old.settings, next.settings),
        KeymapMatcher::deriveDelta(old.keymap, next.keymap),
        TextCodec{}.deriveDelta(old.textEncoding, next.textEncoding),
        TabDeltaCodec{}.derive(old.tabs, next.tabs),
        DiffDeltaCodec{}.derive(old.diff, next.diff),
        ExternalModificationDeltaCodec{}.derive(old.externalModification,
                                                    next.externalModification),
        FollowEditsDeltaCodec{}.derive(old.followEdits, next.followEdits),
        TreeDeltaCodec{}.derive(old.tree, next.tree, 4096),
        SyntaxDeltaCodec{}.derive(old.syntax, next.syntax),
        LspSyncDeltaCodec{}.derive(old.lspSync, next.lspSync),
        LspFeatureDeltaCodec{}.derive(old.lspFeatures, next.lspFeatures),
        {old.theme == next.theme ? std::nullopt
                                 : std::optional{next.theme}},
        {},
        {},
        {},
        UiFrameDeltaCodec{}.derive(old.uiFrame, next.uiFrame),
        old.focus == next.focus ? std::nullopt
                                : std::optional{next.focus},
        {},
        {},
        {},
        PaletteSectionDelta{old.palette == next.palette
                                ? std::nullopt
                                : std::optional{next.palette}},
        PromptViewSectionDelta{old.promptView != next.promptView,
                               next.promptView},
        NoticeViewSectionDelta{old.noticeView != next.noticeView,
                               next.noticeView},
        old.watcherAvailable == next.watcherAvailable
            ? std::nullopt
            : std::optional{next.watcherAvailable},
        old.externalFocusHeld == next.externalFocusHeld
            ? std::nullopt
            : std::optional{next.externalFocusHeld},
    };
}

SessionReplayResult SessionSnapshotCodec::replay(SessionSnapshot const& base,
                                                 SessionDelta const& delta)
    const {
    if (base.revision() != delta.baseRevision_ ||
        delta.revision_ <= delta.baseRevision_ ||
        base.client().clientId != delta.clientId_ ||
        base.client().viewId != delta.viewId_ ||
        base.client().capabilities != delta.capabilities_) {
        return {std::nullopt, "session delta base revision mismatch"};
    }

    auto document = std::optional<DocumentViewState>{base.sections().document};
    if (delta.document_) {
        document = DocumentSnapshotCodec{}.replay(
            base.sections().document, *delta.document_,
            delta.documentCaret_.value_or(base.sections().document.caret));
    } else if (delta.documentCaret_) {
        if (delta.documentCaret_->value() > document->text.size()) {
            return {std::nullopt, "document caret is out of bounds"};
        }
        document->caret = *delta.documentCaret_;
    }
    auto selection = replayReplacement(base.sections().selection,
                                        delta.selection_);
    auto history = replayReplacement(base.sections().history, delta.history_);
    auto clipboard =
        replayReplacement(base.sections().clipboard, delta.clipboard_);
    auto promptStatus = replayReplacement(base.sections().promptStatus,
                                            delta.promptStatus_);
    auto search = SearchDeltaCodec{}.replay(base.sections().search, delta.search_);
    auto findReplace = FindReplaceDeltaCodec{}.replay(base.sections().findReplace,
                                                  delta.findReplace_);
    auto settings = replaySettings(base.sections().settings, delta.settings_);
    auto keymap = replayReplacement(base.sections().keymap, delta.keymap_);
    auto tabs = TabDeltaCodec{}.replay(base.sections().tabs, delta.tabs_);
    auto diff = DiffDeltaCodec{}.replay(base.sections().diff, delta.diff_);
    auto external = ExternalModificationDeltaCodec{}.replay(
        base.sections().externalModification,
        delta.externalModification_);
    auto tree = TreeDeltaCodec{}.replay(base.sections().tree, delta.tree_);
    auto syntax = SyntaxDeltaCodec{}.replay(base.sections().syntax, delta.syntax_);
    auto lspSync =
        LspSyncDeltaCodec{}.replay(base.sections().lspSync, delta.lspSync_);
    auto lspFeatures = LspFeatureDeltaCodec{}.replay(
        base.sections().lspFeatures, delta.lspFeatures_);

    if (!document || !selection || !history || !clipboard || !promptStatus ||
        !search.accepted() ||
        findReplace.error != FindReplaceReplayError::None || !settings ||
        !keymap || !tabs.accepted() || !diff.accepted() ||
        !external.accepted() || !tree.accepted() || !syntax.accepted() ||
        !lspSync.accepted() || !lspFeatures.accepted()) {
        return {std::nullopt, "malformed feature delta"};
    }

    auto textEncoding = base.sections().textEncoding;
    if (delta.textEncoding_) {
        if (delta.textEncoding_->before != textEncoding) {
            return {std::nullopt, "text encoding delta base mismatch"};
        }
        textEncoding = delta.textEncoding_->after;
    }

    auto followEdits = base.sections().followEdits;
    if (delta.followEdits_.baseGeneration != followEdits.generation ||
        (delta.followEdits_.replacement &&
         (delta.followEdits_.replacement->generation !=
              delta.followEdits_.generation ||
          delta.followEdits_.generation <
              delta.followEdits_.baseGeneration)) ||
        (!delta.followEdits_.replacement &&
         delta.followEdits_.generation !=
             delta.followEdits_.baseGeneration)) {
        return {std::nullopt, "follow-edits delta base mismatch"};
    }
    if (delta.followEdits_.replacement) {
        followEdits = *delta.followEdits_.replacement;
    }

    auto theme = delta.theme_.replacement.value_or(base.sections().theme);
    auto focus = delta.focus_.value_or(base.sections().focus);
    // The palette section (candidate universe + matcher parameters) changes as
    // pickers open/close and the command catalog changes; the delta carries a whole-
    // value replacement when it does, else the base value is preserved.
    auto palette = delta.palette_.replacement.value_or(base.sections().palette);
    auto uiFrame =
        UiFrameDeltaCodec{}.replay(base.sections().uiFrame, delta.uiFrameDelta_);
    // The semantic footer-prompt section is optional; the delta's `changed`
    // distinguishes "closed" (replacement nullopt) from "unchanged".
    auto promptView = delta.promptView_.changed
                          ? delta.promptView_.replacement
                          : base.sections().promptView;
    // The semantic draft-conflict notice section is optional; the delta's `changed`
    // distinguishes "cleared" (replacement nullopt) from "unchanged".
    auto noticeView = delta.noticeView_.changed
                          ? delta.noticeView_.replacement
                          : base.sections().noticeView;
    // Decision-13 durable capability fact: the delta carries it only when it flips
    // (nullopt otherwise), so an unchanged availability preserves the base value.
    auto watcherAvailable =
        delta.watcherAvailable_.value_or(base.sections().watcherAvailable);
    // Additive: the delta carries the external-focus flag only when it flips
    // (nullopt otherwise), so an unchanged state preserves the base value.
    auto externalFocusHeld =
        delta.externalFocusHeld_.value_or(base.sections().externalFocusHeld);
    if (!uiFrame.accepted()) {
        return {std::nullopt, "UI frame delta is inconsistent"};
    }

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
        focus,
        std::move(palette),
        std::move(*uiFrame.frame),
        std::move(promptView),
        std::move(noticeView),
        watcherAvailable,
        externalFocusHeld,
    };
    ClientSnapshotState client = base.client();
    return {SessionSnapshot{
                delta.revision_,
                delta.topology_.value_or(base.topology()),
                std::move(client),
                std::move(sections)},
            {}};
}

SessionDelta SessionSnapshotCodec::decodeWire(
    Revision baseRevision, Revision revision, ClientId clientId,
    ViewId viewId, std::vector<CapabilityId> capabilities,
    std::optional<SessionTopology> topology,
    std::optional<DocumentDelta> document,
    std::optional<ByteOffset> documentCaret, SelectionSetDelta selection,
    HistoryDelta history, ClipboardDelta clipboard,
    PromptStatusDelta promptStatus, SearchDelta search,
    FindReplaceDelta findReplace, SettingsSectionDelta settings,
    KeymapDelta keymap, std::optional<TextEncodingDelta> textEncoding,
    TabDelta tabs, DiffDelta diff,
    ExternalModificationDelta externalModification,
    FollowEditsDelta followEdits, TreeDelta tree, SyntaxDelta syntax,
    LspSyncDelta lspSync, LspFeatureDelta lspFeatures,
    ThemeSectionDelta theme, StyleSectionDelta style,
    ShellSectionDelta shell, ViewportDelta viewport,
    UiFrameDelta uiFrameDelta,
    std::optional<FocusTarget> focus,
    SelectionNavigationDelta selectionNav,
    PromptProjectionDelta promptProjection,
    TreeWindowsDelta treeWindows, PaletteSectionDelta palette,
    PromptViewSectionDelta promptView,
    NoticeViewSectionDelta noticeView, std::optional<bool> watcherAvailable,
    std::optional<bool> externalFocusHeld)
    const {
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
                        std::move(style),
                        std::move(shell),
                        std::move(viewport),
                        std::move(uiFrameDelta),
                        focus,
                        std::move(selectionNav),
                        std::move(promptProjection),
                        std::move(treeWindows),
                        std::move(palette),
                        std::move(promptView),
                        std::move(noticeView),
                        watcherAvailable,
                        externalFocusHeld};
}

}  // namespace ssg
