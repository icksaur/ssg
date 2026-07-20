#pragma once

#include <ssg/clipboard.h>
#include <ssg/diff.h>
#include <ssg/external_modification_flow.h>
#include <ssg/find_replace.h>
#include <ssg/follow_edits.h>
#include <ssg/history.h>
#include <ssg/keymap.h>
#include <ssg/lsp_features.h>
#include <ssg/lsp_sync.h>
#include <ssg/palette_searcher.h>
#include <ssg/search.h>
#include <ssg/selection.h>
#include <ssg/session.h>
#include <ssg/settings.h>
#include <ssg/snapshot.h>
#include <ssg/status.h>
#include <ssg/syntax.h>
#include <ssg/tabs.h>
#include <ssg/text_encoding.h>
#include <ssg/theme.h>
#include <ssg/tree.h>
#include <ssg/ui_layout.h>
#include <ssg/viewport.h>

#include <optional>
#include <string>
#include <vector>

namespace ssg {

struct SessionSnapshotSections {
    DocumentViewState document;
    SelectionViewState selection;
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
    ShellViewState shell;
    PaletteViewState palette;
};

[[nodiscard]] bool operator==(SessionSnapshotSections const& left,
                              SessionSnapshotSections const& right);

struct ClientSnapshotState {
    ClientId clientId;
    ViewId viewId;
    std::vector<CapabilityId> capabilities;
    ViewportViewState viewport;

    bool operator==(ClientSnapshotState const&) const = default;
};

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

struct ShellSectionDelta {
    std::optional<ShellViewState> replacement;
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
    [[nodiscard]] SelectionViewDelta const& selection() const noexcept {
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
    [[nodiscard]] SearchDelta const& search() const noexcept { return search_; }
    [[nodiscard]] FindReplaceDelta const& findReplace() const noexcept {
        return findReplace_;
    }
    [[nodiscard]] SettingsSectionDelta const& settings() const noexcept {
        return settings_;
    }
    [[nodiscard]] KeymapDelta const& keymap() const noexcept { return keymap_; }
    [[nodiscard]] std::optional<TextEncodingDelta> const& textEncoding()
        const noexcept {
        return textEncoding_;
    }
    [[nodiscard]] TabDelta const& tabs() const noexcept { return tabs_; }
    [[nodiscard]] DiffDelta const& diff() const noexcept { return diff_; }
    [[nodiscard]] ExternalModificationDelta const& externalModification()
        const noexcept {
        return externalModification_;
    }
    [[nodiscard]] FollowEditsDelta const& followEdits() const noexcept {
        return followEdits_;
    }
    [[nodiscard]] TreeDelta const& tree() const noexcept { return tree_; }
    [[nodiscard]] SyntaxDelta const& syntax() const noexcept { return syntax_; }
    [[nodiscard]] LspSyncDelta const& lspSync() const noexcept {
        return lspSync_;
    }
    [[nodiscard]] LspFeatureDelta const& lspFeatures() const noexcept {
        return lspFeatures_;
    }
    [[nodiscard]] ThemeSectionDelta const& theme() const noexcept {
        return theme_;
    }
    [[nodiscard]] ShellSectionDelta const& shell() const noexcept {
        return shell_;
    }
    [[nodiscard]] ViewportDelta const& viewport() const noexcept {
        return viewport_;
    }

private:
    friend SessionDelta deriveSessionDelta(SessionSnapshot const&,
                                             SessionSnapshot const&);
    friend SessionReplayResult replaySessionDelta(SessionSnapshot const&,
                                                     SessionDelta const&);
    // The protocol codec reconstructs a SessionDelta from decoded wire
    // fields; this factory is the only non-derivation construction path so
    // normal in-process construction remains through derive_session_delta.
    friend SessionDelta decodeWireSessionDelta(
        Revision baseRevision, Revision revision, ClientId clientId,
        ViewId viewId, std::vector<CapabilityId> capabilities,
        std::optional<SessionTopology> topology,
        std::optional<DocumentDelta> document,
        std::optional<ByteOffset> documentCaret,
        SelectionViewDelta selection, HistoryDelta history,
        ClipboardDelta clipboard, PromptStatusDelta promptStatus,
        SearchDelta search, FindReplaceDelta findReplace,
        SettingsSectionDelta settings, KeymapDelta keymap,
        std::optional<TextEncodingDelta> textEncoding, TabDelta tabs,
        DiffDelta diff, ExternalModificationDelta externalModification,
        FollowEditsDelta followEdits, TreeDelta tree, SyntaxDelta syntax,
        LspSyncDelta lspSync, LspFeatureDelta lspFeatures,
        ThemeSectionDelta theme, ShellSectionDelta shell,
        ViewportDelta viewport);

    SessionDelta(
        Revision baseRevision, Revision revision, ClientId clientId,
        ViewId viewId, std::vector<CapabilityId> capabilities,
        std::optional<SessionTopology> topology,
        std::optional<DocumentDelta> document,
        std::optional<ByteOffset> documentCaret,
        SelectionViewDelta selection, HistoryDelta history,
        ClipboardDelta clipboard, PromptStatusDelta promptStatus,
        SearchDelta search, FindReplaceDelta findReplace,
        SettingsSectionDelta settings, KeymapDelta keymap,
        std::optional<TextEncodingDelta> textEncoding, TabDelta tabs,
        DiffDelta diff, ExternalModificationDelta externalModification,
        FollowEditsDelta followEdits, TreeDelta tree, SyntaxDelta syntax,
        LspSyncDelta lspSync, LspFeatureDelta lspFeatures,
        ThemeSectionDelta theme, ShellSectionDelta shell,
        ViewportDelta viewport);

    Revision baseRevision_;
    Revision revision_;
    ClientId clientId_;
    ViewId viewId_;
    std::vector<CapabilityId> capabilities_;
    std::optional<SessionTopology> topology_;
    std::optional<DocumentDelta> document_;
    std::optional<ByteOffset> documentCaret_;
    SelectionViewDelta selection_;
    HistoryDelta history_;
    ClipboardDelta clipboard_;
    PromptStatusDelta promptStatus_;
    SearchDelta search_;
    FindReplaceDelta findReplace_;
    SettingsSectionDelta settings_;
    KeymapDelta keymap_;
    std::optional<TextEncodingDelta> textEncoding_;
    TabDelta tabs_;
    DiffDelta diff_;
    ExternalModificationDelta externalModification_;
    FollowEditsDelta followEdits_;
    TreeDelta tree_;
    SyntaxDelta syntax_;
    LspSyncDelta lspSync_;
    LspFeatureDelta lspFeatures_;
    ThemeSectionDelta theme_;
    ShellSectionDelta shell_;
    ViewportDelta viewport_;
};

struct SessionReplayResult {
    std::optional<SessionSnapshot> snapshot;
    std::string error;

    [[nodiscard]] bool accepted() const noexcept {
        return snapshot.has_value();
    }
};

[[nodiscard]] SessionSnapshot assembleSessionSnapshot(
    Revision revision, SessionTopology topology,
    InvocationPrincipal const& principal, ViewId viewId,
    ViewportViewState viewport, SessionSnapshotSections sections);
[[nodiscard]] SessionDelta deriveSessionDelta(SessionSnapshot const& before,
                                                SessionSnapshot const& after);
[[nodiscard]] SessionReplayResult replaySessionDelta(
    SessionSnapshot const& base, SessionDelta const& delta);

// Reconstructs a SessionDelta from already-validated wire fields (protocol
// codec use only; see the friend declaration above). Ordinary code derives
// deltas through derive_session_delta instead.
[[nodiscard]] SessionDelta decodeWireSessionDelta(
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
    ThemeSectionDelta theme, ShellSectionDelta shell, ViewportDelta viewport);

}  // namespace ssg
