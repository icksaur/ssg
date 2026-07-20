#pragma once

#include <ssg/keymap.h>
#include <ssg/session.h>
#include <ssg/session_snapshot.h>

#include <array>
#include <any>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg::tui {

class TerminalInputCapture {
public:
    [[nodiscard]] std::optional<SemanticCommand> capture(
        CommittedText const& text, KeymapViewState const& keymap,
        std::string_view context);
    [[nodiscard]] std::optional<SemanticCommand> capture(
        KeyStroke const& stroke, KeymapViewState const& keymap,
        std::string_view context);
    [[nodiscard]] std::optional<SemanticCommand> capture(
        SemanticHitTarget const& target, KeymapViewState const& keymap,
        std::string_view context);
    void reset() noexcept;

private:
    KeySequence pending_;
};

class TuiClient {
public:
    using SnapshotProvider = std::function<SessionSnapshot()>;

    TuiClient(EditorSession& session, InvocationPrincipal principal,
              ViewId view_id, SnapshotProvider snapshot_provider);
    ~TuiClient();

    TuiClient(TuiClient const&) = delete;
    TuiClient& operator=(TuiClient const&) = delete;
    TuiClient(TuiClient&&) = delete;
    TuiClient& operator=(TuiClient&&) = delete;

    [[nodiscard]] CommandResult submit(SemanticCommand const& command);
    [[nodiscard]] CommandResult submit(std::string command_id,
                                       std::any payload = {});
    [[nodiscard]] SessionSnapshot const& snapshot() const noexcept {
        return *snapshot_;
    }

private:
    void refresh();

    EditorSession* session_;
    InvocationPrincipal principal_;
    ViewId view_id_;
    SnapshotProvider snapshot_provider_;
    std::optional<SessionSnapshot> snapshot_;
};

}  // namespace ssg::tui
