#include <ssg/session_snapshot.h>

#include <ssg/detail/generated/ordinary_replay.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ssg {
namespace {

SelectionSetDelta deriveSelectionSetDelta(SelectionSet const& before,
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
           left.palette == right.palette &&
           left.uiFrame == right.uiFrame &&
           left.noticeView == right.noticeView &&
           left.watcherAvailable == right.watcherAvailable;
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
    PromptStatusDelta promptStatus, std::optional<SearchViewState> search,
    std::optional<FindReplaceViewState> findReplace,
    SettingsSectionDelta settings, KeymapDelta keymap,
    std::optional<TextEncodingViewState> textEncoding,
    std::optional<TabViewState> tabs, DiffDelta diff,
    ExternalModificationDelta externalModification,
    std::optional<FollowEditsViewState> followEdits, TreeDelta tree,
    std::optional<SyntaxViewState> syntax,
    std::optional<LspSyncViewState> lspSync,
    std::optional<LspFeatureViewState> lspFeatures,
    ThemeSectionDelta theme, UiFrameDelta uiFrameDelta,
    PaletteSectionDelta palette, NoticeViewSectionDelta noticeView,
    std::optional<bool> watcherAvailable)
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
      uiFrameDelta_{std::move(uiFrameDelta)},
      palette_{std::move(palette)},
      noticeView_{std::move(noticeView)},
      watcherAvailable_{watcherAvailable} {}

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
        deriveSelectionSetDelta(old.selection, next.selection),
        HistoryDeltaCodec{}.derive(old.history, next.history),
        ClipboardDeltaCodec{}.derive(old.clipboard, next.clipboard),
        PromptStatusDeltaCodec{}.derive(old.promptStatus, next.promptStatus),
        old.search == next.search ? std::nullopt : std::optional{next.search},
        old.findReplace == next.findReplace
            ? std::nullopt
            : std::optional{next.findReplace},
        settingsDelta(old.settings, next.settings),
        KeymapMatcher::deriveDelta(old.keymap, next.keymap),
        old.textEncoding == next.textEncoding
            ? std::nullopt
            : std::optional{next.textEncoding},
        old.tabs == next.tabs ? std::nullopt : std::optional{next.tabs},
        DiffDeltaCodec{}.derive(old.diff, next.diff),
        ExternalModificationDeltaCodec{}.derive(old.externalModification,
                                                    next.externalModification),
        old.followEdits == next.followEdits
            ? std::nullopt
            : std::optional{next.followEdits},
        TreeDeltaCodec{}.derive(old.tree, next.tree, 4096),
        old.syntax == next.syntax ? std::nullopt : std::optional{next.syntax},
        old.lspSync == next.lspSync
            ? std::nullopt
            : std::optional{next.lspSync},
        old.lspFeatures == next.lspFeatures
            ? std::nullopt
            : std::optional{next.lspFeatures},
        {old.theme == next.theme ? std::nullopt
                                 : std::optional{next.theme}},
        UiFrameDeltaCodec{}.derive(old.uiFrame, next.uiFrame),
        PaletteSectionDelta{old.palette == next.palette
                                ? std::nullopt
                                : std::optional{next.palette}},
        NoticeViewSectionDelta{old.noticeView != next.noticeView,
                               next.noticeView},
        old.watcherAvailable == next.watcherAvailable
            ? std::nullopt
            : std::optional{next.watcherAvailable},
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
    auto ordinary = detail::generated::replayOrdinarySessionSections(
        base.sections(), delta);
    if (!ordinary) {
        return {std::nullopt, "malformed ordinary section delta"};
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
    auto settings = replaySettings(base.sections().settings, delta.settings_);
    auto diff = DiffDeltaCodec{}.replay(base.sections().diff, delta.diff_);
    auto external = ExternalModificationDeltaCodec{}.replay(
        base.sections().externalModification,
        delta.externalModification_);
    auto tree = TreeDeltaCodec{}.replay(base.sections().tree, delta.tree_);

    if (!document || !settings || !diff.accepted() ||
        !external.accepted() || !tree.accepted()) {
        return {std::nullopt, "malformed feature delta"};
    }

    auto uiFrame =
        UiFrameDeltaCodec{}.replay(base.sections().uiFrame, delta.uiFrameDelta_);
    if (!uiFrame.accepted()) {
        return {std::nullopt, "UI frame delta is inconsistent"};
    }
    SessionSnapshotSections sections{
        std::move(*document),
        std::move(ordinary->selection),
        std::move(ordinary->history),
        std::move(ordinary->clipboard),
        std::move(ordinary->promptStatus),
        std::move(ordinary->search),
        std::move(ordinary->findReplace),
        std::move(*settings),
        std::move(ordinary->keymap),
        std::move(ordinary->textEncoding),
        std::move(ordinary->tabs),
        std::move(*diff.state),
        std::move(*external.state),
        std::move(ordinary->followEdits),
        std::move(*tree.state),
        std::move(ordinary->syntax),
        std::move(ordinary->lspSync),
        std::move(ordinary->lspFeatures),
        std::move(ordinary->theme),
        std::move(ordinary->palette),
        std::move(*uiFrame.frame),
        std::move(ordinary->noticeView),
        ordinary->watcherAvailable,
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
    PromptStatusDelta promptStatus, std::optional<SearchViewState> search,
    std::optional<FindReplaceViewState> findReplace,
    SettingsSectionDelta settings, KeymapDelta keymap,
    std::optional<TextEncodingViewState> textEncoding,
    std::optional<TabViewState> tabs, DiffDelta diff,
    ExternalModificationDelta externalModification,
    std::optional<FollowEditsViewState> followEdits, TreeDelta tree,
    std::optional<SyntaxViewState> syntax,
    std::optional<LspSyncViewState> lspSync,
    std::optional<LspFeatureViewState> lspFeatures,
    ThemeSectionDelta theme, UiFrameDelta uiFrameDelta,
    PaletteSectionDelta palette, NoticeViewSectionDelta noticeView,
    std::optional<bool> watcherAvailable)
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
                        std::move(uiFrameDelta),
                        std::move(palette),
                        std::move(noticeView),
                        watcherAvailable};
}

}  // namespace ssg
