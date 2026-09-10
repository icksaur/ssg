#pragma once

#include "grid_test_frame.h"

#include <stdexcept>
#include <utility>

namespace ssg::test {

class GridTestView {
public:
    explicit GridTestView(ViewportDimensions dimensions)
        : dimensions_{dimensions}, presenter_{} {}

    [[nodiscard]] std::optional<GridPresentation> present(
        Editor& session, PaletteReport palette = {}) {
        return presenter_.project(session, {dimensions_, std::move(palette)});
    }

    void resize(ViewportDimensions dimensions) noexcept {
        dimensions_ = dimensions;
    }

    [[nodiscard]] CommandResult dispatch(Editor& session,
                                         std::string_view commandId) {
        auto result = session.dispatch(commandId);
        applyViewAction(session, result);
        return result;
    }

    [[nodiscard]] CommandResult input(Editor& session, ClientInput input) {
        auto result = session.input(input);
        if (!result.command) {
            throw std::runtime_error{"client input produced no command result"};
        }
        applyViewAction(session, *result.command);
        return std::move(*result.command);
    }

private:
    void applyViewAction(Editor& session, CommandResult const& result) {
        if (!result.viewAction) return;

        auto frame = present(session);
        if (!frame) {
            throw std::runtime_error{"view action has no current grid frame"};
        }
        auto applied = presenter_.apply(*result.viewAction, *frame);
        if (!applied.accepted()) {
            throw std::runtime_error{applied.message};
        }
        if (applied.transition) {
            auto transition = session.input(*applied.transition);
            if (transition.outcome == ClientInputOutcome::Rejected) {
                throw std::runtime_error{
                    transition.command
                        ? "view transition was rejected: " +
                              transition.command->message
                        : "view transition was rejected"};
            }
        }
    }

    ViewportDimensions dimensions_;
    GridPresenter presenter_;
};

}  // namespace ssg::test
