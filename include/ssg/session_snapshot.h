#pragma once

#include <ssg/clipboard.h>
#include <ssg/diff.h>
#include <ssg/external_modification.h>
#include <ssg/find_replace.h>
#include <ssg/follow_edits.h>
#include <ssg/history.h>
#include <ssg/input.h>
#include <ssg/lsp_features.h>
#include <ssg/lsp_sync.h>
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
    PromptStatusViewState prompt_status;
    SearchViewState search;
    FindReplaceViewState find_replace;
    SettingsViewState settings;
    KeymapViewState keymap;
    TextEncodingViewState text_encoding;
    TabViewState tabs;
    DiffViewState diff;
    ExternalModificationViewState external_modification;
    FollowEditsViewState follow_edits;
    TreeViewState tree;
    SyntaxViewState syntax;
    LspSyncViewState lsp_sync;
    LspFeatureViewState lsp_features;
    ThemeSnapshot theme;
    ShellViewState shell;
};

[[nodiscard]] bool operator==(SessionSnapshotSections const& left,
                              SessionSnapshotSections const& right);

struct ClientSnapshotState {
    ClientId client_id;
    ViewId view_id;
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

    [[nodiscard]] Revision base_revision() const noexcept {
        return base_revision_;
    }
    [[nodiscard]] Revision revision() const noexcept { return revision_; }
    [[nodiscard]] ClientId client_id() const noexcept { return client_id_; }
    [[nodiscard]] ViewId view_id() const noexcept { return view_id_; }
    [[nodiscard]] std::vector<CapabilityId> const& capabilities() const noexcept {
        return capabilities_;
    }
    [[nodiscard]] std::optional<SessionTopology> const& topology() const noexcept {
        return topology_;
    }
    [[nodiscard]] std::optional<DocumentDelta> const& document() const noexcept {
        return document_;
    }
    [[nodiscard]] std::optional<ByteOffset> const& document_caret()
        const noexcept {
        return document_caret_;
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
    [[nodiscard]] PromptStatusDelta const& prompt_status() const noexcept {
        return prompt_status_;
    }
    [[nodiscard]] SearchDelta const& search() const noexcept { return search_; }
    [[nodiscard]] FindReplaceDelta const& find_replace() const noexcept {
        return find_replace_;
    }
    [[nodiscard]] SettingsSectionDelta const& settings() const noexcept {
        return settings_;
    }
    [[nodiscard]] KeymapDelta const& keymap() const noexcept { return keymap_; }
    [[nodiscard]] std::optional<TextEncodingDelta> const& text_encoding()
        const noexcept {
        return text_encoding_;
    }
    [[nodiscard]] TabDelta const& tabs() const noexcept { return tabs_; }
    [[nodiscard]] DiffDelta const& diff() const noexcept { return diff_; }
    [[nodiscard]] ExternalModificationDelta const& external_modification()
        const noexcept {
        return external_modification_;
    }
    [[nodiscard]] FollowEditsDelta const& follow_edits() const noexcept {
        return follow_edits_;
    }
    [[nodiscard]] TreeDelta const& tree() const noexcept { return tree_; }
    [[nodiscard]] SyntaxDelta const& syntax() const noexcept { return syntax_; }
    [[nodiscard]] LspSyncDelta const& lsp_sync() const noexcept {
        return lsp_sync_;
    }
    [[nodiscard]] LspFeatureDelta const& lsp_features() const noexcept {
        return lsp_features_;
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
    friend SessionDelta derive_session_delta(SessionSnapshot const&,
                                             SessionSnapshot const&);
    friend SessionReplayResult replay_session_delta(SessionSnapshot const&,
                                                     SessionDelta const&);

    SessionDelta(
        Revision base_revision, Revision revision, ClientId client_id,
        ViewId view_id, std::vector<CapabilityId> capabilities,
        std::optional<SessionTopology> topology,
        std::optional<DocumentDelta> document,
        std::optional<ByteOffset> document_caret,
        SelectionViewDelta selection, HistoryDelta history,
        ClipboardDelta clipboard, PromptStatusDelta prompt_status,
        SearchDelta search, FindReplaceDelta find_replace,
        SettingsSectionDelta settings, KeymapDelta keymap,
        std::optional<TextEncodingDelta> text_encoding, TabDelta tabs,
        DiffDelta diff, ExternalModificationDelta external_modification,
        FollowEditsDelta follow_edits, TreeDelta tree, SyntaxDelta syntax,
        LspSyncDelta lsp_sync, LspFeatureDelta lsp_features,
        ThemeSectionDelta theme, ShellSectionDelta shell,
        ViewportDelta viewport);

    Revision base_revision_;
    Revision revision_;
    ClientId client_id_;
    ViewId view_id_;
    std::vector<CapabilityId> capabilities_;
    std::optional<SessionTopology> topology_;
    std::optional<DocumentDelta> document_;
    std::optional<ByteOffset> document_caret_;
    SelectionViewDelta selection_;
    HistoryDelta history_;
    ClipboardDelta clipboard_;
    PromptStatusDelta prompt_status_;
    SearchDelta search_;
    FindReplaceDelta find_replace_;
    SettingsSectionDelta settings_;
    KeymapDelta keymap_;
    std::optional<TextEncodingDelta> text_encoding_;
    TabDelta tabs_;
    DiffDelta diff_;
    ExternalModificationDelta external_modification_;
    FollowEditsDelta follow_edits_;
    TreeDelta tree_;
    SyntaxDelta syntax_;
    LspSyncDelta lsp_sync_;
    LspFeatureDelta lsp_features_;
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

[[nodiscard]] SessionSnapshot assemble_session_snapshot(
    Revision revision, SessionTopology topology,
    InvocationPrincipal const& principal, ViewId view_id,
    ViewportViewState viewport, SessionSnapshotSections sections);
[[nodiscard]] SessionDelta derive_session_delta(SessionSnapshot const& before,
                                                SessionSnapshot const& after);
[[nodiscard]] SessionReplayResult replay_session_delta(
    SessionSnapshot const& base, SessionDelta const& delta);

}  // namespace ssg
