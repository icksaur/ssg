#pragma once

#include <ssg/Keymap.h>
#include <ssg/Editor.h>
#include <ssg/GridPresenter.h>

#include <array>
#include <any>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssg::tui {

class TuiClient {
public:
    TuiClient(Editor& runtime, ViewportDimensions dimensions);
    ~TuiClient();

    TuiClient(TuiClient const&) = delete;
    TuiClient& operator=(TuiClient const&) = delete;
    TuiClient(TuiClient&&) = delete;
    TuiClient& operator=(TuiClient&&) = delete;

    [[nodiscard]] CommandResult submit(std::string command_id,
                                       std::any payload = {});
    [[nodiscard]] ClientInputResult input(ClientInput input);
    [[nodiscard]] GridPresentation const& snapshot() const noexcept {
        return *snapshot_;
    }

private:
    void applyViewAction(CommandResult const& result);
    void refresh();

    Editor* runtime_;
    ViewportDimensions dimensions_;
    GridPresenter presenter_;
    std::optional<GridPresentation> snapshot_;
};

}  // namespace ssg::tui
