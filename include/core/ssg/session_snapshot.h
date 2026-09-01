#pragma once

#include <ssg/ClipboardRegister.h>
#include <ssg/DiffModel.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/FindReplace.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/DocumentHistory.h>
#include <ssg/Keymap.h>
#include <ssg/LspFeatureController.h>
#include <ssg/lsp_sync_client.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/PromptSurface.h>
#include <ssg/Search.h>
#include <ssg/Selection.h>
#include <ssg/EditorClient.h>
#include <ssg/Settings.h>
#include <ssg/snapshot.h>
#include <ssg/StatusQueue.h>
#include <ssg/SyntaxModel.h>
#include <ssg/TabManager.h>
#include <ssg/TextCodec.h>
#include <ssg/Theme.h>
#include <ssg/TreeModel.h>
#include <ssg/UiFrame.h>

#include <optional>
#include <string>
#include <vector>

namespace ssg {

class SessionSnapshotCodec;

// One action in the draft-conflict notice: a label bound to the already-registered
// library `command` it dispatches. Geometry-free: grid presentation combines it
// with the solved notice surface.
struct NoticeAction {
    std::string id;
    std::string label;
    std::string command;
    friend bool operator==(const NoticeAction&, const NoticeAction&) = default;
};

// The geometry-free semantic projection of the active document's draft-conflict
// notice (M15): its message and ordered actions. Absent unless the active document
// has an unresolved draft conflict. A native client renders and drives the notice
// from this; the grid client combines it with the solved notice node.
struct NoticeView {
    std::string text;
    std::vector<NoticeAction> actions;
    friend bool operator==(const NoticeView&, const NoticeView&) = default;
};

struct SessionSnapshotSections {
    DocumentViewState document;
    SelectionSet selection;
    HistoryViewState history;
    ClipboardViewState clipboard;
    PromptStatusViewState promptStatus;
    SearchViewState search;
    FindReplaceViewState findReplace;
    SettingsViewState settings;
    KeymapViewState keymap;
    TextEncodingViewState textEncoding;
    TabViewState tabs;
    DiffViewState diff;
    ExternalModificationViewState externalModification;
    FollowEditsViewState followEdits;
    TreeViewState tree;
    SyntaxViewState syntax;
    LspSyncViewState lspSync;
    LspFeatureViewState lspFeatures;
    ThemeSnapshot theme;
    PaletteViewState palette;
    // One validated, atomically published UI-VM frame: schema, resolved values,
    // presence basis, and authoritative focus path cannot diverge.
    UiFrame uiFrame;
    // The geometry-free semantic projection of the active document's draft-conflict
    // notice (M15). Absent unless the active document has an unresolved conflict. A
    // native and grid clients render the notice bar from this; grid placement
    // comes from the solved notice node.
    std::optional<NoticeView> noticeView;
    // Decision-13 durable capability fact: whether the session is watching the
    // workspace for external modification. False for the whole session when the
    // platform cannot provide a filesystem watcher, so a client renders "external
    // changes are not being watched" from library truth rather than inventing it.
    // Additive on the wire; an absent field decodes to available (true).
    bool watcherAvailable = true;
};

[[nodiscard]] bool operator==(SessionSnapshotSections const& left,
                              SessionSnapshotSections const& right);

struct ClientSnapshotState {
    ClientId clientId;
    ViewId viewId;
    std::vector<CapabilityId> capabilities;

    bool operator==(ClientSnapshotState const&) const = default;
};

// CONTRACT
// SessionSnapshot: while one instance is alive, any number of threads may read
//   it concurrently through const access, but moving or destroying it requires
//   external exclusion, and handing it to a thread that may outlive the owner
//   requires an explicit copy or serialization. It carries no internal
//   synchronization for its own move or destruction. The same governs
//   SessionDelta.
class SessionSnapshot {
public:
    SessionSnapshot(Revision revision, SessionTopology topology,
                    ClientSnapshotState client,
                    SessionSnapshotSections sections);

    SessionSnapshot(SessionSnapshot const&) = delete;
    SessionSnapshot& operator=(SessionSnapshot const&) = delete;
    SessionSnapshot(SessionSnapshot&&) noexcept = default;
    SessionSnapshot& operator=(SessionSnapshot&&) noexcept = default;

    [[nodiscard]] Revision revision() const noexcept { return revision_; }
    [[nodiscard]] SessionTopology const& topology() const noexcept {
        return topology_;
    }
    [[nodiscard]] ClientSnapshotState const& client() const noexcept {
        return client_;
    }
    [[nodiscard]] SessionSnapshotSections const& sections() const noexcept {
        return sections_;
    }
    bool operator==(SessionSnapshot const&) const;

  private:
    Revision revision_;
    SessionTopology topology_;
    ClientSnapshotState client_;
    SessionSnapshotSections sections_;
};

struct SettingsSectionDelta {
    std::vector<SettingsDelta> changes;
};

struct ThemeSectionDelta {
    std::optional<ThemeSnapshot> replacement;
};

// Whole-value delta of the palette section: the candidate universe and matcher
// parameters change together (a picker opens/closes, a mode switches, the command
// catalog changes), so a change replaces the section wholesale; nullopt means
// unchanged. Without this a delta-replaying client keeps a stale candidate universe.
struct PaletteSectionDelta {
    std::optional<PaletteViewState> replacement;
};

// Delta of the optional semantic NoticeView section. `changed` is explicit because
// the section is itself optional: {true, nullopt} means the notice cleared, {true,
// value} a new/changed notice, {false, _} unchanged. Read `changed` first.
struct NoticeViewSectionDelta {
    bool changed = false;
    std::optional<NoticeView> replacement;
};

struct SessionReplayResult;

class SessionDelta {
public:
    SessionDelta(SessionDelta const&) = delete;
    SessionDelta& operator=(SessionDelta const&) = delete;
    SessionDelta(SessionDelta&&) noexcept = default;
    SessionDelta& operator=(SessionDelta&&) noexcept = default;

    [[nodiscard]] Revision baseRevision() const noexcept {
        return baseRevision_;
    }
    [[nodiscard]] Revision revision() const noexcept { return revision_; }
    [[nodiscard]] ClientId clientId() const noexcept { return clientId_; }
    [[nodiscard]] ViewId viewId() const noexcept { return viewId_; }
    [[nodiscard]] std::vector<CapabilityId> const& capabilities() const noexcept {
        return capabilities_;
    }
    [[nodiscard]] std::optional<SessionTopology> const& topology() const noexcept {
        return topology_;
    }
    [[nodiscard]] std::optional<DocumentDelta> const& document() const noexcept {
        return document_;
    }
    [[nodiscard]] std::optional<ByteOffset> const& documentCaret()
        const noexcept {
        return documentCaret_;
    }
    [[nodiscard]] SelectionSetDelta const& selection() const noexcept {
        return selection_;
    }
    [[nodiscard]] HistoryDelta const& history() const noexcept {
        return history_;
    }
    [[nodiscard]] ClipboardDelta const& clipboard() const noexcept {
        return clipboard_;
    }
    [[nodiscard]] PromptStatusDelta const& promptStatus() const noexcept {
        return promptStatus_;
    }
    [[nodiscard]] std::optional<SearchViewState> const& search() const noexcept {
        return search_;
    }
    [[nodiscard]] std::optional<FindReplaceViewState> const& findReplace()
        const noexcept {
        return findReplace_;
    }
    [[nodiscard]] SettingsSectionDelta const& settings() const noexcept {
        return settings_;
    }
    [[nodiscard]] KeymapDelta const& keymap() const noexcept { return keymap_; }
    [[nodiscard]] std::optional<TextEncodingViewState> const& textEncoding()
        const noexcept {
        return textEncoding_;
    }
    [[nodiscard]] std::optional<TabViewState> const& tabs() const noexcept {
        return tabs_;
    }
    [[nodiscard]] DiffDelta const& diff() const noexcept { return diff_; }
    [[nodiscard]] ExternalModificationDelta const& externalModification()
        const noexcept {
        return externalModification_;
    }
    [[nodiscard]] std::optional<FollowEditsViewState> const& followEdits()
        const noexcept {
        return followEdits_;
    }
    [[nodiscard]] TreeDelta const& tree() const noexcept { return tree_; }
    [[nodiscard]] std::optional<SyntaxViewState> const& syntax() const noexcept {
        return syntax_;
    }
    [[nodiscard]] std::optional<LspSyncViewState> const& lspSync() const noexcept {
        return lspSync_;
    }
    [[nodiscard]] std::optional<LspFeatureViewState> const& lspFeatures()
        const noexcept {
        return lspFeatures_;
    }
    [[nodiscard]] ThemeSectionDelta const& theme() const noexcept {
        return theme_;
    }
    [[nodiscard]] UiFrameDelta const& uiFrameDelta() const noexcept {
        return uiFrameDelta_;
    }
    [[nodiscard]] PaletteSectionDelta const& palette() const noexcept {
        return palette_;
    }
    [[nodiscard]] NoticeViewSectionDelta const& noticeView() const noexcept {
        return noticeView_;
    }
    [[nodiscard]] std::optional<bool> const& watcherAvailable() const noexcept {
        return watcherAvailable_;
    }

private:
    friend class SessionSnapshotCodec;
    // The protocol codec reconstructs a SessionDelta from decoded wire
    // fields; this factory is the only non-derivation construction path so
    // normal in-process construction remains through derive_session_delta.

    SessionDelta(
        Revision baseRevision, Revision revision, ClientId clientId,
        ViewId viewId, std::vector<CapabilityId> capabilities,
        std::optional<SessionTopology> topology,
        std::optional<DocumentDelta> document,
        std::optional<ByteOffset> documentCaret,
        SelectionSetDelta selection, HistoryDelta history,
        ClipboardDelta clipboard, PromptStatusDelta promptStatus,
        std::optional<SearchViewState> search,
        std::optional<FindReplaceViewState> findReplace,
        SettingsSectionDelta settings, KeymapDelta keymap,
        std::optional<TextEncodingViewState> textEncoding,
        std::optional<TabViewState> tabs,
        DiffDelta diff, ExternalModificationDelta externalModification,
        std::optional<FollowEditsViewState> followEdits, TreeDelta tree,
        std::optional<SyntaxViewState> syntax,
        std::optional<LspSyncViewState> lspSync,
        std::optional<LspFeatureViewState> lspFeatures,
        ThemeSectionDelta theme, UiFrameDelta uiFrameDelta,
        PaletteSectionDelta palette = {},
        NoticeViewSectionDelta noticeView = {},
        std::optional<bool> watcherAvailable = {});

    Revision baseRevision_;
    Revision revision_;
    ClientId clientId_;
    ViewId viewId_;
    std::vector<CapabilityId> capabilities_;
    std::optional<SessionTopology> topology_;
    std::optional<DocumentDelta> document_;
    std::optional<ByteOffset> documentCaret_;
    SelectionSetDelta selection_;
    HistoryDelta history_;
    ClipboardDelta clipboard_;
    PromptStatusDelta promptStatus_;
    std::optional<SearchViewState> search_;
    std::optional<FindReplaceViewState> findReplace_;
    SettingsSectionDelta settings_;
    KeymapDelta keymap_;
    std::optional<TextEncodingViewState> textEncoding_;
    std::optional<TabViewState> tabs_;
    DiffDelta diff_;
    ExternalModificationDelta externalModification_;
    std::optional<FollowEditsViewState> followEdits_;
    TreeDelta tree_;
    std::optional<SyntaxViewState> syntax_;
    std::optional<LspSyncViewState> lspSync_;
    std::optional<LspFeatureViewState> lspFeatures_;
    ThemeSectionDelta theme_;
    UiFrameDelta uiFrameDelta_;
    PaletteSectionDelta palette_;
    NoticeViewSectionDelta noticeView_;
    std::optional<bool> watcherAvailable_;
};

struct SessionReplayResult {
    std::optional<SessionSnapshot> snapshot;
    std::string error;

    [[nodiscard]] bool accepted() const noexcept {
        return snapshot.has_value();
    }
};

class SessionSnapshotCodec {
public:
    [[nodiscard]] SessionDelta deriveDelta(SessionSnapshot const& before,
                                           SessionSnapshot const& after) const;
    // CONTRACT: Replay validates the retained revision and attachment, and
    // publishes no candidate section before the complete delta accepts.
    [[nodiscard]] SessionReplayResult replay(SessionSnapshot const& base,
                                             SessionDelta const& delta) const;

    // Reconstructs a SessionDelta from already-validated wire fields (protocol
    // codec use only). Ordinary code derives deltas through deriveDelta.
    [[nodiscard]] SessionDelta decodeWire(
        Revision baseRevision, Revision revision, ClientId clientId,
        ViewId viewId, std::vector<CapabilityId> capabilities,
        std::optional<SessionTopology> topology,
        std::optional<DocumentDelta> document,
        std::optional<ByteOffset> documentCaret,
        SelectionSetDelta selection, HistoryDelta history,
        ClipboardDelta clipboard, PromptStatusDelta promptStatus,
        std::optional<SearchViewState> search,
        std::optional<FindReplaceViewState> findReplace,
        SettingsSectionDelta settings, KeymapDelta keymap,
        std::optional<TextEncodingViewState> textEncoding,
        std::optional<TabViewState> tabs,
        DiffDelta diff, ExternalModificationDelta externalModification,
        std::optional<FollowEditsViewState> followEdits, TreeDelta tree,
        std::optional<SyntaxViewState> syntax,
        std::optional<LspSyncViewState> lspSync,
        std::optional<LspFeatureViewState> lspFeatures,
        ThemeSectionDelta theme, UiFrameDelta uiFrameDelta,
        PaletteSectionDelta palette = {},
        NoticeViewSectionDelta noticeView = {},
        std::optional<bool> watcherAvailable = {}) const;
};

}  // namespace ssg
