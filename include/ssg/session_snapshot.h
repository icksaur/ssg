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
#include <ssg/Search.h>
#include <ssg/Selection.h>
#include <ssg/EditorSession.h>
#include <ssg/Settings.h>
#include <ssg/snapshot.h>
#include <ssg/StatusQueue.h>
#include <ssg/SyntaxModel.h>
#include <ssg/TabManager.h>
#include <ssg/TextCodec.h>
#include <ssg/Theme.h>
#include <ssg/Style.h>
#include <ssg/TreeModel.h>
#include <ssg/UiTree.h>
#include <ssg/UiNodeState.h>
#include <ssg/UiPresence.h>
#include <ssg/ShellState.h>
#include <ssg/Viewport.h>

#include <optional>
#include <string>
#include <vector>

namespace ssg {

class SessionSnapshotCodec;

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
    // Semantic interaction: which surface has keyboard focus. A client routes
    // input by this; it is not grid geometry. The shell's layout projection lives
    // in PresentationSnapshot.
    FocusTarget focus = FocusTarget::Editor;
    PaletteViewState palette;
    // The medium-agnostic UI-VM tree: the header/footer chrome as a generic node
    // tree a native client renders directly (the grid client lowers the same tree
    // through lowerUiChromeRegion). Published alongside the legacy grid path; a
    // client that does not consume it simply ignores it. Defaults to a valid empty
    // root (emptyUiRoot) when no chrome is composed.
    UiSchema ui;
    // The generation-scoped resolved dynamic state for the `ui` schema: one record
    // per node with, for a renderable leaf, its resolved semantic (value, label,
    // command, checked). A non-grid client needs this because the schema carries
    // value SOURCES it cannot resolve. Stamped with the same generation as `ui`.
    UiStateSection uiState;
    // The authoritative per-node presence for the `ui` schema, basis-stamped so a
    // client can reconcile an optimistically-predicted mutation (phase 7B). A
    // separate authority from uiState (visibility vs. resolved content); one record
    // per schema node, at the same generation as `ui`.
    UiPresenceSection uiPresence = defaultUiPresence();
};

[[nodiscard]] bool operator==(SessionSnapshotSections const& left,
                              SessionSnapshotSections const& right);

struct ClientSnapshotState {
    ClientId clientId;
    ViewId viewId;
    std::vector<CapabilityId> capabilities;

    bool operator==(ClientSnapshotState const&) const = default;
};

// The optional grid-presentation projection of a snapshot. Present only when a
// client requested geometry by supplying ViewportDimensions; a native-layout
// client (one that lays out the semantic model itself) receives a snapshot with
// no presentation at all, so semantic state is never gated on grid geometry.
struct PresentationSnapshot {
    ViewportViewState viewport;
    Style style;
    // The footer-anchored prompt's layout view (kind, rect, controls). Absent for
    // a header-hosted prompt (palette/file finder) and when no prompt is active.
    // Pure grid projection; "which prompt is open" is the semantic
    // PromptStatusViewState::activeKind.
    std::optional<PromptViewState> prompt;
    // The shell's grid layout: viewport GridSize, chrome rects, panes, tab hits,
    // accessibility geometry, palette projection. Pure projection; the semantic
    // focus is SessionSnapshotSections::focus.
    ShellViewState shell;
    // The selection's grid scroll projection (scroll anchor + desired cell); the
    // semantic selection set is SessionSnapshotSections::selection.
    SelectionNavigation selectionNav;
    // Per-provider tree scroll windows (grid projection); the semantic tree
    // (nodes, selection, expansion) is SessionSnapshotSections::tree.
    std::vector<TreeWindow> treeWindows;

    // Not defaulted: ShellViewState has no operator==; it is compared field-wise
    // via shellEqual (see the .cpp).
    bool operator==(PresentationSnapshot const&) const;
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
                    SessionSnapshotSections sections,
                    std::optional<PresentationSnapshot> presentation = std::nullopt);

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
    [[nodiscard]] std::optional<PresentationSnapshot> const& presentation()
        const noexcept {
        return presentation_;
    }

    bool operator==(SessionSnapshot const&) const;

  private:
    Revision revision_;
    SessionTopology topology_;
    ClientSnapshotState client_;
    SessionSnapshotSections sections_;
    std::optional<PresentationSnapshot> presentation_;
};

struct SettingsSectionDelta {
    std::vector<SettingsDelta> changes;
};

struct ThemeSectionDelta {
    std::optional<ThemeSnapshot> replacement;
};

// Whole-value delta of the medium-agnostic UI section (like ThemeSectionDelta):
// the tree changes only on a chrome/generation change, so a change replaces it
// wholesale; nullopt means unchanged.
struct UiSectionDelta {
    std::optional<UiSchema> replacement;
};

// Whole-value delta of the dynamic node state (like UiSectionDelta): the resolved
// values change together each frame, so a change replaces the section wholesale;
// nullopt means unchanged.
struct UiStateSectionDelta {
    std::optional<UiStateSection> replacement;
};

// Whole-value delta of the presence section: presence advances atomically through a
// mutation patch, so a change replaces the section wholesale; nullopt means
// unchanged.
struct UiPresenceSectionDelta {
    std::optional<UiPresenceSection> replacement;
};

struct StyleSectionDelta {
    std::optional<Style> replacement;
};

struct ShellSectionDelta {
    std::optional<ShellViewState> replacement;
};

// Delta of the optional footer-prompt projection. `changed` is explicit because
// the prompt is itself optional: {true, nullopt} means the prompt closed,
// {true, value} a new prompt, {false, _} unchanged. Read `changed` first.
struct PromptProjectionDelta {
    bool changed = false;
    std::optional<PromptViewState> replacement;
};

// Delta of the tree scroll windows (whole-value replacement; the windows are a
// pure projection recomputed each snapshot, so no incremental encoding).
struct TreeWindowsDelta {
    bool changed = false;
    std::optional<std::vector<TreeWindow>> replacement;
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
    [[nodiscard]] SelectionNavigationDelta const& selectionNav() const noexcept {
        return selectionNav_;
    }
    [[nodiscard]] PromptProjectionDelta const& promptProjection() const noexcept {
        return promptProjection_;
    }
    [[nodiscard]] TreeWindowsDelta const& treeWindows() const noexcept {
        return treeWindows_;
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
    [[nodiscard]] UiSectionDelta const& ui() const noexcept { return ui_; }
    [[nodiscard]] UiStateSectionDelta const& uiState() const noexcept {
        return uiState_;
    }
    [[nodiscard]] UiPresenceSectionDelta const& uiPresence() const noexcept {
        return uiPresence_;
    }
    [[nodiscard]] StyleSectionDelta const& style() const noexcept {
        return style_;
    }
    [[nodiscard]] ShellSectionDelta const& shell() const noexcept {
        return shell_;
    }
    [[nodiscard]] ViewportDelta const& viewport() const noexcept {
        return viewport_;
    }
    [[nodiscard]] std::optional<FocusTarget> const& focus() const noexcept {
        return focus_;
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
        SearchDelta search, FindReplaceDelta findReplace,
        SettingsSectionDelta settings, KeymapDelta keymap,
        std::optional<TextEncodingDelta> textEncoding, TabDelta tabs,
        DiffDelta diff, ExternalModificationDelta externalModification,
        FollowEditsDelta followEdits, TreeDelta tree, SyntaxDelta syntax,
        LspSyncDelta lspSync, LspFeatureDelta lspFeatures,
        ThemeSectionDelta theme, StyleSectionDelta style,
        ShellSectionDelta shell,
        ViewportDelta viewport,
        std::optional<FocusTarget> focus = std::nullopt,
        SelectionNavigationDelta selectionNav = {},
        PromptProjectionDelta promptProjection = {},
        TreeWindowsDelta treeWindows = {}, UiSectionDelta ui = {},
        UiStateSectionDelta uiState = {},
        UiPresenceSectionDelta uiPresence = {});

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
    StyleSectionDelta style_;
    ShellSectionDelta shell_;
    ViewportDelta viewport_;
    std::optional<FocusTarget> focus_;
    SelectionNavigationDelta selectionNav_;
    PromptProjectionDelta promptProjection_;
    TreeWindowsDelta treeWindows_;
    UiSectionDelta ui_;
    UiStateSectionDelta uiState_;
    UiPresenceSectionDelta uiPresence_;
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
    [[nodiscard]] SessionSnapshot assemble(
        Revision revision, SessionTopology topology,
        InvocationPrincipal const& principal, ViewId viewId,
        ViewportViewState viewport, SessionSnapshotSections sections,
        Style style = {},
        std::optional<PromptViewState> prompt = {},
        ShellViewState shell = {},
        SelectionNavigation selectionNav = {},
        std::vector<TreeWindow> treeWindows = {}) const;
    [[nodiscard]] SessionDelta deriveDelta(SessionSnapshot const& before,
                                           SessionSnapshot const& after) const;
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
        SearchDelta search, FindReplaceDelta findReplace,
        SettingsSectionDelta settings, KeymapDelta keymap,
        std::optional<TextEncodingDelta> textEncoding, TabDelta tabs,
        DiffDelta diff, ExternalModificationDelta externalModification,
        FollowEditsDelta followEdits, TreeDelta tree, SyntaxDelta syntax,
        LspSyncDelta lspSync, LspFeatureDelta lspFeatures,
        ThemeSectionDelta theme, StyleSectionDelta style,
        ShellSectionDelta shell,
        ViewportDelta viewport,
        std::optional<FocusTarget> focus = std::nullopt,
        SelectionNavigationDelta selectionNav = {},
        PromptProjectionDelta promptProjection = {},
        TreeWindowsDelta treeWindows = {}, UiSectionDelta ui = {},
        UiStateSectionDelta uiState = {},
        UiPresenceSectionDelta uiPresence = {}) const;
};

}  // namespace ssg
