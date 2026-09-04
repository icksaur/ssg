#include "command_executor.h"

#include <ssg/CommandCatalog.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

namespace ssg {
namespace {

class DispatchMarker {
public:
    explicit DispatchMarker(std::atomic<std::thread::id>& slot) : slot_{slot} {
        slot_.store(std::this_thread::get_id(), std::memory_order_release);
    }
    ~DispatchMarker() {
        slot_.store(std::thread::id{}, std::memory_order_release);
    }
    DispatchMarker(DispatchMarker const&) = delete;
    DispatchMarker& operator=(DispatchMarker const&) = delete;

private:
    std::atomic<std::thread::id>& slot_;
};

ExecutorResult rejected(CommandError error, std::string message) {
    return {error, std::move(message), std::nullopt};
}

}  // namespace

struct CommandExecutor::Impl {
    explicit Impl(std::shared_ptr<CommandCatalog> commandCatalog)
        : catalog{std::move(commandCatalog)} {}

    mutable std::mutex mutex;
    std::atomic<std::thread::id> dispatchingThread{};
    std::shared_ptr<CommandCatalog> catalog;
    SessionTopology topology;
};

CommandExecutor::CommandExecutor(std::shared_ptr<CommandCatalog> catalog)
    : impl_{std::make_unique<Impl>(std::move(catalog))} {
    if (!impl_->catalog) {
        throw std::invalid_argument{"a session requires a command catalog"};
    }
}

CommandExecutor::~CommandExecutor() = default;

std::shared_ptr<CommandCatalog> const& CommandExecutor::catalog() const {
    return impl_->catalog;
}

bool CommandExecutor::dispatchInProgress() const noexcept {
    return impl_->dispatchingThread.load(std::memory_order_acquire) ==
           std::this_thread::get_id();
}

ExecutorResult CommandExecutor::dispatch(ClientCommand const& command) {
    std::lock_guard lock{impl_->mutex};
    DispatchMarker const marker{impl_->dispatchingThread};

    const auto* registered =
        command.id.handle().valid()
            ? impl_->catalog->find(command.id.handle())
            : impl_->catalog->find(command.id.name());
    if (registered == nullptr) {
        return rejected(CommandError::UnknownCommand,
                        "command is not registered: " +
                            std::string{command.id.name()});
    }

    CommandContext context{};
    CommandHandlerResult handlerResult;
    try {
        handlerResult = registered->handler(context, command.payload);
    } catch (std::exception const& exception) {
        return rejected(CommandError::HandlerFailed,
                        "command handler threw: " +
                            std::string{exception.what()});
    } catch (...) {
        return rejected(CommandError::HandlerFailed,
                        "command handler threw an unknown exception");
    }

    if (!handlerResult.accepted) {
        return rejected(CommandError::HandlerFailed,
                        std::move(handlerResult.message));
    }
    const bool viewOwned = registered->effect == CommandEffect::ViewAction;
    if (handlerResult.viewAction && !viewOwned) {
        return rejected(CommandError::HandlerFailed,
                        "only a view-action command may require a view action");
    }
    if (viewOwned && !handlerResult.viewAction) {
        return rejected(CommandError::HandlerFailed,
                        "a view-action command did not return a view action");
    }

    if (registered->effect == CommandEffect::Mutation) {
        if (context.workspaceChanged_) {
            impl_->topology.activeWorkspace = context.activeWorkspace_;
        }
    }
    return {CommandError::None, {}, std::move(handlerResult.viewAction)};
}

SessionTopology CommandExecutor::topology() const {
    std::lock_guard lock{impl_->mutex};
    return impl_->topology;
}

}  // namespace ssg
