#include "tui_fixture.h"

#include <algorithm>
#include <any>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ssg::tui {
namespace {

std::any command_payload(SemanticInputArguments const& arguments) {
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

bool starts_with(KeySequence const& sequence, KeySequence const& prefix) {
    return prefix.size() <= sequence.size() &&
           std::equal(prefix.begin(), prefix.end(), sequence.begin());
}

}  // namespace

std::optional<SemanticCommand> TerminalInputCapture::capture(
    CommittedText const& text, KeymapViewState const&, std::string_view) {
    reset();
    return semantic_input(text);
}

std::optional<SemanticCommand> TerminalInputCapture::capture(
    KeyStroke const& stroke, KeymapViewState const& keymap,
    std::string_view context) {
    pending_.push_back(stroke);
    bool prefix = false;
    for (auto const& binding : keymap.bindings) {
        if (binding.context != context ||
            !starts_with(binding.sequence, pending_)) {
            continue;
        }
        prefix = true;
        if (binding.sequence == pending_) {
            reset();
            return SemanticCommand{binding.command_id, {}};
        }
    }
    if (!prefix) reset();
    return std::nullopt;
}

std::optional<SemanticCommand> TerminalInputCapture::capture(
    SemanticHitTarget const& target, KeymapViewState const&,
    std::string_view) {
    reset();
    return activate_hit_target(target);
}

void TerminalInputCapture::reset() noexcept { pending_.clear(); }

TuiClient::TuiClient(EditorSession& session, InvocationPrincipal principal,
                     ViewId view_id, SnapshotProvider snapshot_provider)
    : session_{&session},
      principal_{std::move(principal)},
      view_id_{view_id},
      snapshot_provider_{std::move(snapshot_provider)} {
    if (!snapshot_provider_) {
        throw std::invalid_argument{"TUI snapshot provider is required"};
    }
    auto attached = session_->attach(principal_, view_id_);
    if (!attached.accepted()) throw std::invalid_argument{attached.message};
    try {
        refresh();
    } catch (...) {
        (void)session_->detach(principal_.client_id());
        throw;
    }
}

TuiClient::~TuiClient() {
    if (session_) (void)session_->detach(principal_.client_id());
}

CommandResult TuiClient::submit(SemanticCommand const& command) {
    return submit(command.command_id, command_payload(command.arguments));
}

CommandResult TuiClient::submit(std::string command_id, std::any payload) {
    auto result = session_->dispatch(
        principal_.client_id(),
        {std::move(command_id), snapshot_->revision(), std::move(payload)});
    if (result.accepted()) refresh();
    return result;
}

void TuiClient::refresh() {
    auto next = snapshot_provider_();
    if (next.client().client_id != principal_.client_id() ||
        next.client().view_id != view_id_ ||
        next.revision() != session_->revision()) {
        throw std::logic_error{
            "TUI snapshot provider returned a different attachment or revision"};
    }
    snapshot_ = std::move(next);
}

}  // namespace ssg::tui
