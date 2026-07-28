#include <ssg/CommandRegistry.h>


#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ssg {

CapabilityId::CapabilityId(std::string value) : value_{std::move(value)} {
    if (value_.empty()) {
        throw std::invalid_argument{"capability ID must not be empty"};
    }
}

InvocationPrincipal::InvocationPrincipal(
    ClientId clientId, InvocationOrigin origin,
    std::vector<CapabilityId> capabilities)
    : clientId_{clientId},
      origin_{origin},
      capabilities_{std::move(capabilities)} {
    std::sort(capabilities_.begin(), capabilities_.end());
    capabilities_.erase(
        std::unique(capabilities_.begin(), capabilities_.end()),
        capabilities_.end());
}

bool InvocationPrincipal::hasCapability(
    CapabilityId const& capability) const {
    return std::binary_search(capabilities_.begin(), capabilities_.end(),
                              capability);
}

void CommandContext::setActiveWorkspace(WorkspaceId workspace) noexcept {
    workspaceChanged_ = true;
    activeWorkspace_ = workspace;
}

void CommandContext::setActiveView(ViewId view) noexcept {
    viewChanged_ = true;
    activeView_ = view;
}

CommandHandlerResult CommandHandlerResult::success() {
    return {true, {}};
}

CommandHandlerResult CommandHandlerResult::failure(std::string message) {
    return {false, std::move(message)};
}

CommandSet::CommandSet(std::vector<CommandRegistration> commands)
    : commands_{std::move(commands)} {
    std::unordered_set<std::string> ids;
    for (auto const& command : commands_) {
        if (command.descriptor.id.empty()) {
            throw std::invalid_argument{"command ID must not be empty"};
        }
        if (!command.handler) {
            throw std::invalid_argument{"command handler must not be empty: " +
                                        command.descriptor.id};
        }
        if (!ids.emplace(command.descriptor.id).second) {
            throw std::invalid_argument{"duplicate command ID in command set: " +
                                        command.descriptor.id};
        }
    }
}

struct CommandRegistry::Impl {
    std::unordered_map<std::string, CommandRegistration> commands;
    // Parallel to the catalog: byHandle[h.index()] is the registration for that
    // catalog row, or nullptr when the row is unbound.  Stable because the map
    // is never mutated after construction.
    std::vector<CommandRegistration const*> byHandle;
};

CommandRegistry::CommandRegistry(std::vector<CommandSet> commandSets)
    : impl_{std::make_unique<Impl>()} {
    std::vector<std::string> ordered;
    for (auto& commandSet : commandSets) {
        for (auto const& command : commandSet.commands()) {
            auto [unused, inserted] =
                impl_->commands.emplace(command.descriptor.id, command);
            if (!inserted) {
                throw std::invalid_argument{
                    "duplicate command ID across command sets: " +
                    command.descriptor.id};
            }
            ordered.push_back(command.descriptor.id);
        }
    }
    // A handle is a position in the catalog, and registrations arrive in
    // catalog order (EditorSessionBuilder::build walks it), so the nth
    // registration is the command the nth handle names.  Pointers into the map
    // are stable because nothing is inserted after this loop.
    impl_->byHandle.reserve(impl_->commands.size());
    for (auto const& id : ordered) {
        auto const found = impl_->commands.find(id);
        impl_->byHandle.push_back(found == impl_->commands.end()
                                      ? nullptr
                                      : &found->second);
    }
}

CommandRegistry::~CommandRegistry() = default;
CommandRegistry::CommandRegistry(CommandRegistry&&) noexcept = default;
CommandRegistry& CommandRegistry::operator=(CommandRegistry&&) noexcept =
    default;

CommandRegistration const* CommandRegistry::find(
    std::string_view commandId) const {
    auto found = impl_->commands.find(std::string{commandId});
    return found == impl_->commands.end() ? nullptr : &found->second;
}

CommandRegistration const* CommandRegistry::find(CommandHandle command) const {
    return command.valid() ? impl_->byHandle[command.index()] : nullptr;
}

}  // namespace ssg
