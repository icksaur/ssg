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
    return semanticInput(text);
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

std::optional<SemanticCommand> TerminalInputCapture::capture(
    SemanticHitTarget const& target, KeymapViewState const&,
    std::string_view) {
    reset();
    return activateHitTarget(target);
}

void TerminalInputCapture::reset() noexcept { pending_.clear(); }

TuiClient::TuiClient(EditorSession& session, InvocationPrincipal principal,
                     ViewId viewId, SnapshotProvider snapshotProvider)
    : session_{&session},
      principal_{std::move(principal)},
      view_id_{viewId},
      snapshot_provider_{std::move(snapshotProvider)} {
    if (!snapshot_provider_) {
        throw std::invalid_argument{"TUI snapshot provider is required"};
    }
    auto attached = session_->attach(principal_, view_id_);
    if (!attached.accepted()) throw std::invalid_argument{attached.message};
    try {
        refresh();
    } catch (...) {
        (void)session_->detach(principal_.clientId());
        throw;
    }
}

TuiClient::~TuiClient() {
    if (session_) (void)session_->detach(principal_.clientId());
}

CommandResult TuiClient::submit(SemanticCommand const& command) {
    return submit(command.commandId, commandPayload(command.arguments));
}

CommandResult TuiClient::submit(std::string commandId, std::any payload) {
    auto result = session_->dispatch(
        principal_.clientId(),
        {std::move(commandId), snapshot_->revision(), std::move(payload)});
    if (result.accepted()) refresh();
    return result;
}

void TuiClient::refresh() {
    auto next = snapshot_provider_();
    if (next.client().clientId != principal_.clientId() ||
        next.client().viewId != view_id_ ||
        next.revision() != session_->revision()) {
        throw std::logic_error{
            "TUI snapshot provider returned a different attachment or revision"};
    }
    snapshot_ = std::move(next);
}

}  // namespace ssg::tui
