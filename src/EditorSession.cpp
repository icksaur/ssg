#include <ssg/EditorSession.h>

#include <ssg/CommandCatalog.h>

#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace ssg {
namespace {

struct ClientIdHash {
    std::size_t operator()(ClientId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value());
    }
};

CommandResult rejected(CommandError error, Revision revision,
                       std::string message) {
    return {error, revision, std::move(message)};
}

}  // namespace

struct EditorSession::Impl {
    explicit Impl(CommandRegistry commandRegistry, CommandServices* services)
        : registry{std::move(commandRegistry)}, services{services} {}

    mutable std::mutex mutex;
    CommandRegistry registry;
    CommandServices* services;
    std::shared_ptr<CommandCatalog> catalog;
    Revision revision{1};
    SessionTopology topology;
    std::unordered_map<ClientId, AttachedClient, ClientIdHash> clients;
};

EditorSession::EditorSession(CommandRegistry registry, CommandServices* services,
                             std::shared_ptr<CommandCatalog> catalog)
    : impl_{std::make_unique<Impl>(std::move(registry), services)} {
    impl_->catalog = std::move(catalog);
}

std::shared_ptr<CommandCatalog> const& EditorSession::catalog() const {
    return impl_->catalog;
}

EditorSession::~EditorSession() = default;

AttachResult EditorSession::attach(InvocationPrincipal principal,
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

bool EditorSession::detach(ClientId clientId) {
    std::lock_guard lock{impl_->mutex};
    return impl_->clients.erase(clientId) != 0;
}

CommandResult EditorSession::dispatch(ClientId clientId,
                                      ClientCommand const& command) {
    std::lock_guard lock{impl_->mutex};
    Revision const currentRevision = impl_->revision;

    auto const client = impl_->clients.find(clientId);
    if (client == impl_->clients.end()) {
        return rejected(CommandError::UnknownClient, currentRevision,
                        "client ID is not attached");
    }

    // A ref built from a name the catalog knows already carries its handle, so
    // the name lookup is only reached for a command outside the catalog.
    auto const* registration = command.id.handle().valid()
                                   ? impl_->registry.find(command.id.handle())
                                   : impl_->registry.find(command.id.name());
    if (registration == nullptr) {
        return rejected(CommandError::UnknownCommand, currentRevision,
                        "command is not registered: " +
                            std::string{command.id.name()});
    }

    for (auto const& capability :
         registration->descriptor.requiredCapabilities) {
        if (!client->second.principal.hasCapability(capability)) {
            return rejected(
                CommandError::CapabilityDenied, currentRevision,
                "principal lacks required capability: " +
                    std::string{capability.value()});
        }
    }

    bool const mutates =
        registration->descriptor.effect == CommandEffect::Mutation;
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
                           impl_->services};
    CommandHandlerResult handlerResult;
    try {
        handlerResult = registration->handler(context, command.payload);
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

Revision EditorSession::revision() const {
    std::lock_guard lock{impl_->mutex};
    return impl_->revision;
}

Revision EditorSession::advanceRevision() {
    std::lock_guard lock{impl_->mutex};
    if (impl_->revision.value() == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error{"session revision is exhausted"};
    }
    impl_->revision = Revision{impl_->revision.value() + 1};
    return impl_->revision;
}

SessionTopology EditorSession::topology() const {
    std::lock_guard lock{impl_->mutex};
    return impl_->topology;
}

std::optional<AttachedClient> EditorSession::attachedClient(
    ClientId clientId) const {
    std::lock_guard lock{impl_->mutex};
    auto const found = impl_->clients.find(clientId);
    if (found == impl_->clients.end()) {
        return std::nullopt;
    }
    return found->second;
}

}  // namespace ssg
