#pragma once

#include <ssg/Keymap.h>
#include <ssg/EditorRuntime.h>
#include <ssg/session_snapshot.h>

#include <array>
#include <any>
#include <cstdint>
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
    TuiClient(EditorRuntime& runtime, InvocationPrincipal principal,
              ViewId viewId, ViewportDimensions dimensions);
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

    EditorRuntime* runtime_;
    InvocationPrincipal principal_;
    ViewId viewId_;
    ViewportDimensions dimensions_;
    std::optional<SessionSnapshot> snapshot_;
};

}  // namespace ssg::tui
