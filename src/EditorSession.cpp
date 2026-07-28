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
    Impl(std::shared_ptr<CommandCatalog> commandCatalog,
         CommandServices* commandServices)
        : catalog{std::move(commandCatalog)}, services{commandServices} {}

    mutable std::mutex mutex;
    // Held, never copied: the catalog is what dispatch reads, so a command
    // registered after this session was built is dispatchable immediately.
    std::shared_ptr<CommandCatalog> catalog;
    CommandServices* services;
    Revision revision{1};
    SessionTopology topology;
    std::unordered_map<ClientId, AttachedClient, ClientIdHash> clients;
};

EditorSession::EditorSession(std::shared_ptr<CommandCatalog> catalog,
                             CommandServices* services)
    : impl_{std::make_unique<Impl>(std::move(catalog), services)} {
    if (!impl_->catalog) {
        throw std::invalid_argument{"a session requires a command catalog"};
    }
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

    // Straight from the live catalog, so a command registered a moment ago is
    // dispatchable now.  A snapshot taken when the session was built would
    // publish new commands to the palette and the keymap while refusing to run
    // them (doc/spec-command-registry.md, R8).
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
        if (!client->second.principal.hasCapability(CapabilityId{capability})) {
            return rejected(
                CommandError::CapabilityDenied, currentRevision,
                "principal lacks required capability: " + capability);
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
                           impl_->services};
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
