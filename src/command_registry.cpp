#include <ssg/command_registry.h>

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
    ClientId client_id, InvocationOrigin origin,
    std::vector<CapabilityId> capabilities)
    : client_id_{client_id},
      origin_{origin},
      capabilities_{std::move(capabilities)} {
    std::sort(capabilities_.begin(), capabilities_.end());
    capabilities_.erase(
        std::unique(capabilities_.begin(), capabilities_.end()),
        capabilities_.end());
}

bool InvocationPrincipal::has_capability(
    CapabilityId const& capability) const {
    return std::binary_search(capabilities_.begin(), capabilities_.end(),
                              capability);
}

void CommandContext::set_active_workspace(WorkspaceId workspace) noexcept {
    workspace_changed_ = true;
    active_workspace_ = workspace;
}

void CommandContext::set_active_view(ViewId view) noexcept {
    view_changed_ = true;
    active_view_ = view;
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
};

CommandRegistry::CommandRegistry(std::vector<CommandSet> command_sets)
    : impl_{std::make_unique<Impl>()} {
    for (auto& command_set : command_sets) {
        for (auto const& command : command_set.commands()) {
            auto [unused, inserted] =
                impl_->commands.emplace(command.descriptor.id, command);
            if (!inserted) {
                throw std::invalid_argument{
                    "duplicate command ID across command sets: " +
                    command.descriptor.id};
            }
        }
    }
}

CommandRegistry::~CommandRegistry() = default;
CommandRegistry::CommandRegistry(CommandRegistry&&) noexcept = default;
CommandRegistry& CommandRegistry::operator=(CommandRegistry&&) noexcept =
    default;

CommandRegistration const* CommandRegistry::find(
    std::string_view command_id) const {
    auto found = impl_->commands.find(std::string{command_id});
    return found == impl_->commands.end() ? nullptr : &found->second;
}

}  // namespace ssg
