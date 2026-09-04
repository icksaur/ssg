#include "tui_fixture.h"

#include <algorithm>
#include <any>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ssg::tui {
namespace {

std::any commandPayload(SemanticInputArguments const& arguments) {
    return std::visit(
        [](auto const& value) -> std::any {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::monostate>) {
                return {};
            } else {
                return value;
            }
        },
        arguments);
}

bool startsWith(KeySequence const& sequence, KeySequence const& prefix) {
    return prefix.size() <= sequence.size() &&
           std::equal(prefix.begin(), prefix.end(), sequence.begin());
}

}  // namespace

std::optional<SemanticCommand> TerminalInputCapture::capture(
    CommittedText const& text, KeymapViewState const&, std::string_view) {
    reset();
    return SemanticInputRouter{}.semanticInput(text);
}

std::optional<SemanticCommand> TerminalInputCapture::capture(
    KeyStroke const& stroke, KeymapViewState const& keymap,
    std::string_view context) {
    pending_.push_back(stroke);
    bool prefix = false;
    for (auto const& binding : keymap.bindings) {
        if (binding.context != context ||
            !startsWith(binding.sequence, pending_)) {
            continue;
        }
        prefix = true;
        if (binding.sequence == pending_) {
            reset();
            return SemanticCommand{binding.commandId, {}};
        }
    }
    if (!prefix) reset();
    return std::nullopt;
}

void TerminalInputCapture::reset() noexcept { pending_.clear(); }

TuiClient::TuiClient(EditorSession& runtime,
                     ViewportDimensions dimensions)
    : runtime_{&runtime},
      dimensions_{dimensions},
      presenter_{} {
    refresh();
}

TuiClient::~TuiClient() = default;

CommandResult TuiClient::submit(SemanticCommand const& command) {
    return submit(command.commandId, commandPayload(command.arguments));
}

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
