#pragma once

#include <ssg/ClipboardRegister.h>
#include <ssg/DiffModel.h>
#include <ssg/DocumentHistory.h>
#include <ssg/EditorClient.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/FindReplace.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/Keymap.h>
#include <ssg/LspFeatureController.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/PromptSurface.h>
#include <ssg/Search.h>
#include <ssg/Selection.h>
#include <ssg/Settings.h>
#include <ssg/snapshot.h>
#include <ssg/StatusQueue.h>
#include <ssg/SyntaxModel.h>
#include <ssg/TabManager.h>
#include <ssg/TextCodec.h>
#include <ssg/Theme.h>
#include <ssg/TreeModel.h>
#include <ssg/UiFrame.h>
#include <ssg/lsp_sync_client.h>

#include <optional>
#include <string>
#include <vector>

namespace ssg {

struct NoticeAction {
    std::string id;
    std::string label;
    std::string command;
    friend bool operator==(const NoticeAction&, const NoticeAction&) = default;
};

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
    UiFrame uiFrame;
    std::optional<NoticeView> noticeView;
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
// it concurrently through const access, but moving or destroying it requires
// external exclusion, and handing it to a thread that may outlive the owner
// requires an explicit copy or serialization. It carries no internal
// synchronization for its own move or destruction.
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

}  // namespace ssg
