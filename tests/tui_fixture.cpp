#include "tui_fixture.h"

#include <stdexcept>
#include <utility>

namespace ssg::tui {

TuiClient::TuiClient(Editor& runtime,
                     ViewportDimensions dimensions)
    : runtime_{&runtime},
      dimensions_{dimensions},
      presenter_{} {
    refresh();
}

TuiClient::~TuiClient() = default;

CommandResult TuiClient::submit(std::string commandId) {
    auto result = runtime_->dispatch(commandId);
    if (result.accepted()) {
        applyViewAction(result);
        refresh();
    }
    return result;
}

ClientInputResult TuiClient::input(ClientInput input) {
    auto result = runtime_->input(std::move(input));
    if (result.command && result.command->accepted()) {
        applyViewAction(*result.command);
        refresh();
    }
    return result;
}

void TuiClient::applyViewAction(CommandResult const& result) {
    if (!result.viewAction) return;
    if (!snapshot_) refresh();
    auto applied = presenter_.apply(*result.viewAction, *snapshot_);
    if (!applied.accepted()) {
        throw std::logic_error{applied.message};
    }
    if (applied.transition) {
        auto transition = runtime_->input(*applied.transition);
        if (transition.outcome == ClientInputOutcome::Rejected) {
            throw std::logic_error{
                transition.command
                    ? "view transition was rejected: " +
                          transition.command->message
                    : "view transition was rejected"};
        }
    }
}

void TuiClient::refresh() {
    auto next = presenter_.project(
        *runtime_, GridPresentationRequest{dimensions_, PaletteReport{}});
    if (!next) {
        throw std::logic_error{"TUI runtime did not return its attached snapshot"};
    }
    snapshot_ = std::move(*next);
}

}  // namespace ssg::tui
