#pragma once

#include <ssg/Keymap.h>
#include <ssg/EditorSession.h>
#include <tui/GridPresenter.h>

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
    void reset() noexcept;

private:
    KeySequence pending_;
};

class TuiClient {
public:
    TuiClient(EditorSession& runtime, ViewportDimensions dimensions);
    ~TuiClient();

    TuiClient(TuiClient const&) = delete;
    TuiClient& operator=(TuiClient const&) = delete;
    TuiClient(TuiClient&&) = delete;
    TuiClient& operator=(TuiClient&&) = delete;

    [[nodiscard]] CommandResult submit(SemanticCommand const& command);
    [[nodiscard]] CommandResult submit(std::string command_id,
                                       std::any payload = {});
    [[nodiscard]] GridPresentation const& snapshot() const noexcept {
        return *snapshot_;
    }

private:
    void refresh();

    EditorSession* runtime_;
    ViewportDimensions dimensions_;
    GridPresenter presenter_;
    std::optional<GridPresentation> snapshot_;
};

}  // namespace ssg::tui
