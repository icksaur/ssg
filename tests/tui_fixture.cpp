#include "tui_fixture.h"

#include <any>
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

CommandResult TuiClient::submit(std::string commandId, std::any payload) {
    auto result =
        runtime_->dispatch({std::move(commandId), std::move(payload)});
    if (result.accepted()) refresh();
    return result;
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
