#include <ssg/session.h>

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
    explicit Impl(CommandRegistry command_registry, CommandServices* services)
        : registry{std::move(command_registry)}, services{services} {}

    mutable std::mutex mutex;
    CommandRegistry registry;
    CommandServices* services;
    Revision revision{1};
    SessionTopology topology;
    std::unordered_map<ClientId, AttachedClient, ClientIdHash> clients;
};

EditorSession::EditorSession(CommandRegistry registry, CommandServices* services)
    : impl_{std::make_unique<Impl>(std::move(registry), services)} {}

EditorSession::~EditorSession() = default;

AttachResult EditorSession::attach(InvocationPrincipal principal,
                                   ViewId view_id) {
    std::lock_guard lock{impl_->mutex};
    ClientId const client_id = principal.clientId();
    auto [unused, inserted] = impl_->clients.emplace(
        client_id, AttachedClient{std::move(principal), view_id});
    if (!inserted) {
        return {AttachError::DuplicateClient,
                "client ID is already attached"};
    }
    return {AttachError::None, {}};
}

bool EditorSession::detach(ClientId client_id) {
    std::lock_guard lock{impl_->mutex};
    return impl_->clients.erase(client_id) != 0;
}

CommandResult EditorSession::dispatch(ClientId client_id,
                                      ClientCommand const& command) {
    std::lock_guard lock{impl_->mutex};
    Revision const current_revision = impl_->revision;

    auto const client = impl_->clients.find(client_id);
    if (client == impl_->clients.end()) {
        return rejected(CommandError::UnknownClient, current_revision,
                        "client ID is not attached");
    }

    auto const* registration = impl_->registry.find(command.id);
    if (registration == nullptr) {
        return rejected(CommandError::UnknownCommand, current_revision,
                        "command is not registered: " + command.id);
    }

    for (auto const& capability :
         registration->descriptor.required_capabilities) {
        if (!client->second.principal.hasCapability(capability)) {
            return rejected(
                CommandError::CapabilityDenied, current_revision,
                "principal lacks required capability: " +
                    std::string{capability.value()});
        }
    }

    bool const mutates =
        registration->descriptor.effect == CommandEffect::Mutation;
    if (mutates && command.base_revision != current_revision) {
        return rejected(CommandError::StaleRevision, current_revision,
                        "mutation base revision does not match session revision");
    }
    if (mutates &&
        current_revision.value() ==
            std::numeric_limits<std::uint64_t>::max()) {
        return rejected(CommandError::RevisionExhausted, current_revision,
                        "session revision is exhausted");
    }

    CommandContext context{current_revision, client->second.principal,
                           impl_->services};
    CommandHandlerResult handler_result;
    try {
        handler_result = registration->handler(context, command.payload);
    } catch (std::exception const& exception) {
        return rejected(CommandError::HandlerFailed, current_revision,
                        "command handler threw: " +
                            std::string{exception.what()});
    } catch (...) {
        return rejected(CommandError::HandlerFailed, current_revision,
                        "command handler threw an unknown exception");
    }

    if (!handler_result.accepted) {
        return rejected(CommandError::HandlerFailed, current_revision,
                        std::move(handler_result.message));
    }

    if (mutates) {
        if (context.workspace_changed_) {
            impl_->topology.active_workspace = context.active_workspace_;
        }
        if (context.view_changed_) {
            impl_->topology.active_view = context.active_view_;
        }
        impl_->revision = Revision{current_revision.value() + 1};
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
    ClientId client_id) const {
    std::lock_guard lock{impl_->mutex};
    auto const found = impl_->clients.find(client_id);
    if (found == impl_->clients.end()) {
        return std::nullopt;
    }
    return found->second;
}

}  // namespace ssg
