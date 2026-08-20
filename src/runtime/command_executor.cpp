#include "command_executor.h"

#include <ssg/CommandCatalog.h>

#include <atomic>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace ssg {

namespace {

// Records which thread is inside dispatch, for as long as it is, so a handler
// that dispatches again is recognised instead of deadlocking.
class DispatchMarker {
public:
    DispatchMarker(std::atomic<std::thread::id>& slot,
                   std::atomic<std::uint64_t>& revisionSlot, Revision revision)
        : slot_{slot}, revisionSlot_{revisionSlot} {
        revisionSlot_.store(revision.value(), std::memory_order_relaxed);
        slot_.store(std::this_thread::get_id(), std::memory_order_release);
    }
    ~DispatchMarker() {
        slot_.store(std::thread::id{}, std::memory_order_release);
    }
    DispatchMarker(DispatchMarker const&) = delete;
    DispatchMarker& operator=(DispatchMarker const&) = delete;

private:
    std::atomic<std::thread::id>& slot_;
    std::atomic<std::uint64_t>& revisionSlot_;
};

}  // namespace
namespace {

struct ClientIdHash {
    std::size_t operator()(ClientId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value());
    }
};

ExecutorResult rejected(CommandError error, Revision revision,
                        std::string message) {
    return {error, revision, std::move(message)};
}

}  // namespace

struct CommandExecutor::Impl {
    Impl(std::shared_ptr<CommandCatalog> commandCatalog,
         CommandServices* commandServices)
        : catalog{std::move(commandCatalog)}, services{commandServices} {}

    mutable std::mutex mutex;
    std::atomic<std::thread::id> dispatchingThread{};
    std::atomic<std::uint64_t> dispatchRevision{};
    // Held, never copied: the catalog is what dispatch reads, so a command
    // registered after this session was built is dispatchable immediately.
    std::shared_ptr<CommandCatalog> catalog;
    CommandServices* services;
    Revision revision{1};
    SessionTopology topology;
    std::unordered_map<ClientId, AttachedClient, ClientIdHash> clients;
};

CommandExecutor::CommandExecutor(std::shared_ptr<CommandCatalog> catalog,
                                 CommandServices* services)
    : impl_{std::make_unique<Impl>(std::move(catalog), services)} {
    if (!impl_->catalog) {
        throw std::invalid_argument{"a session requires a command catalog"};
    }
}

std::shared_ptr<CommandCatalog> const& CommandExecutor::catalog() const {
    return impl_->catalog;
}

CommandExecutor::~CommandExecutor() = default;

AttachResult CommandExecutor::attach(InvocationPrincipal principal,
                                    ViewId viewId) {
    std::lock_guard lock{impl_->mutex};
    ClientId const clientId = principal.clientId();
    auto [unused, inserted] = impl_->clients.emplace(
        clientId, AttachedClient{std::move(principal), viewId});
    if (!inserted) {
        return {AttachError::DuplicateClient,
                "client ID is already attached"};
    }
    return {AttachError::None, {}};
}

bool CommandExecutor::detach(ClientId clientId) {
    std::lock_guard lock{impl_->mutex};
    return impl_->clients.erase(clientId) != 0;
}

std::optional<Revision> CommandExecutor::activeDispatchRevision() const noexcept {
    if (impl_->dispatchingThread.load(std::memory_order_acquire) !=
        std::this_thread::get_id()) {
        return std::nullopt;
    }
    // The revision the in-progress dispatch is running against: readable
    // without the lock precisely because this thread is the one holding it.
    return Revision{impl_->dispatchRevision.load(std::memory_order_relaxed)};
}

ExecutorResult CommandExecutor::dispatch(ClientId clientId,
                                         ClientCommand const& command) {
    std::lock_guard lock{impl_->mutex};
    Revision const currentRevision = impl_->revision;
    DispatchMarker const marker{impl_->dispatchingThread,
                                impl_->dispatchRevision, currentRevision};

    auto const client = impl_->clients.find(clientId);
    if (client == impl_->clients.end()) {
        return rejected(CommandError::UnknownClient, currentRevision,
                        "client ID is not attached");
    }

    // Straight from the live catalog, so a command registered a moment ago is
    // dispatchable now.  A snapshot taken when the session was built would
    // publish new commands to the palette and the keymap while refusing to run
    // them.
    //
    // A handle names the command directly; a caller that has not resolved one
    // supplies only the name, which costs a lookup.
    auto const* command_ = command.id.handle().valid()
                               ? impl_->catalog->find(command.id.handle())
                               : impl_->catalog->find(command.id.name());
    if (command_ == nullptr) {
        return rejected(CommandError::UnknownCommand, currentRevision,
                        "command is not registered: " +
                            std::string{command.id.name()});
    }

    for (auto const& capability : command_->requiredCapabilities) {
        if (!client->second.principal.hasCapability(capability)) {
            return rejected(CommandError::CapabilityDenied, currentRevision,
                            "principal lacks required capability: " +
                                std::string{capability.value()});
        }
    }

    bool const mutates = command_->effect == CommandEffect::Mutation;
    if (mutates && command.baseRevision != currentRevision) {
        return rejected(CommandError::StaleRevision, currentRevision,
                        "mutation base revision does not match session revision");
    }
    if (mutates &&
        currentRevision.value() ==
            std::numeric_limits<std::uint64_t>::max()) {
        return rejected(CommandError::RevisionExhausted, currentRevision,
                        "session revision is exhausted");
    }

    CommandContext context{currentRevision, client->second.principal,
                           client->second.viewId, impl_->services};
    CommandHandlerResult handlerResult;
    try {
        handlerResult = command_->handler(context, command.payload);
    } catch (std::exception const& exception) {
        return rejected(CommandError::HandlerFailed, currentRevision,
                        "command handler threw: " +
                            std::string{exception.what()});
    } catch (...) {
        return rejected(CommandError::HandlerFailed, currentRevision,
                        "command handler threw an unknown exception");
    }

    if (!handlerResult.accepted) {
        return rejected(CommandError::HandlerFailed, currentRevision,
                        std::move(handlerResult.message));
    }

    if (mutates) {
        if (context.workspaceChanged_) {
            impl_->topology.activeWorkspace = context.activeWorkspace_;
        }
        if (context.viewChanged_) {
            impl_->topology.activeView = context.activeView_;
        }
        impl_->revision = Revision{currentRevision.value() + 1};
    }
    return {CommandError::None, impl_->revision, {}};
}

Revision CommandExecutor::revision() const {
    // A handler asking for the revision is asking from INSIDE a dispatch, which
    // already holds this lock -- and already knows the answer.  Taking the lock
    // again would hang rather than answer.
    if (auto const nested = activeDispatchRevision()) return *nested;
    std::lock_guard lock{impl_->mutex};
    return impl_->revision;
}

Revision CommandExecutor::advanceRevision() {
    std::lock_guard lock{impl_->mutex};
    if (impl_->revision.value() == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error{"session revision is exhausted"};
    }
    impl_->revision = Revision{impl_->revision.value() + 1};
    return impl_->revision;
}

SessionTopology CommandExecutor::topology() const {
    std::lock_guard lock{impl_->mutex};
    return impl_->topology;
}

std::optional<AttachedClient> CommandExecutor::attachedClient(
    ClientId clientId) const {
    std::lock_guard lock{impl_->mutex};
    auto const found = impl_->clients.find(clientId);
    if (found == impl_->clients.end()) {
        return std::nullopt;
    }
    return found->second;
}

}  // namespace ssg
