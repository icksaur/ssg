#pragma once

#include "grid_test_frame.h"

#include <stdexcept>
#include <utility>

namespace ssg::test {

class GridTestView {
public:
    GridTestView(ClientId client, ViewId view, ViewportDimensions dimensions)
        : client_{client}, dimensions_{dimensions}, presenter_{view} {}

    [[nodiscard]] std::optional<GridPresentation> present(
        EditorSession& session, PaletteReport palette = {}) {
        return presenter_.project(
            session, client_, {dimensions_, std::move(palette)});
    }

    void resize(ViewportDimensions dimensions) noexcept {
        dimensions_ = dimensions;
    }

    [[nodiscard]] CommandResult dispatch(EditorSession& session,
                                         ClientCommand command) {
        auto result = session.dispatch(client_, std::move(command));
        if (!result.viewAction) return result;

        auto frame = present(session);
        if (!frame) {
            throw std::runtime_error{"view action has no current grid frame"};
        }
        auto applied = presenter_.apply(*result.viewAction, *frame);
        if (!applied.accepted()) {
            throw std::runtime_error{applied.message};
        }
        if (applied.transition) {
            auto transition = session.input(client_, *applied.transition);
            if (transition.outcome == ClientInputOutcome::Rejected) {
                throw std::runtime_error{
                    transition.command
                        ? "view transition was rejected: " +
                              transition.command->message
                        : "view transition was rejected"};
            }
        }
        return result;
    }

private:
    ClientId client_;
    ViewportDimensions dimensions_;
    GridPresenter presenter_;
};

}  // namespace ssg::test
