#include <ssg/EditorSessionBuilder.h>

#include <ssg/ClipboardRegister.h>
#include <ssg/Commands.h>
#include <ssg/DiffModel.h>
#include <ssg/EditCommands.h>
#include <ssg/ExternalModificationFlow.h>
#include <ssg/FileCommands.h>
#include <ssg/FindReplace.h>
#include <ssg/FollowEditsModel.h>
#include <ssg/DocumentHistory.h>
#include <ssg/Keymap.h>
#include <ssg/LspFeatureController.h>
#include <ssg/LspWorkspaceEditController.h>
#include <ssg/PromptSurface.h>
#include <ssg/Search.h>
#include <ssg/Selection.h>
#include <ssg/Settings.h>
#include <ssg/TabManager.h>
#include <ssg/TextCodec.h>
#include <ssg/TextInputCommands.h>
#include <ssg/Theme.h>
#include <ssg/TreeModel.h>
#include <ssg/ShellState.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ssg {
namespace {

}  // namespace

std::vector<CommandDescriptor> p0CommandDescriptors() {
    // A projection of the one authored catalog (src/Commands.cpp).  Effect and
    // capabilities used to be invented here -- every command was hardcoded
    // `Mutation`, and one capability was assigned by an `if` on a single id --
    // so the assembled registry disagreed with the declared catalog by
    // construction.  Both are now declared per command.
    auto const catalog = commandCatalog();
    std::vector<CommandDescriptor> result;
    result.reserve(catalog.size());
    for (auto const& command : catalog) {
        std::vector<CapabilityId> capabilities;
        capabilities.reserve(command.requiredCapabilities.size());
        for (auto const& capability : command.requiredCapabilities) {
            capabilities.emplace_back(std::string{capability});
        }
        result.push_back({std::string{command.id}, command.effect,
                          std::move(capabilities)});
    }

    std::unordered_set<std::string> unique;
    for (auto const& descriptor : result) {
        if (!unique.emplace(descriptor.id).second) {
            throw std::logic_error{"duplicate P0 command descriptor: " +
                                   descriptor.id};
        }
    }
    return result;
}

struct EditorSessionBuilder::Impl {
    std::unordered_map<std::string, CommandHandler> handlers;
    CommandServices* services = nullptr;
};

EditorSessionBuilder::EditorSessionBuilder() : impl_{std::make_unique<Impl>()} {}
EditorSessionBuilder::~EditorSessionBuilder() = default;
EditorSessionBuilder::EditorSessionBuilder(EditorSessionBuilder&&) noexcept =
    default;
EditorSessionBuilder& EditorSessionBuilder::operator=(
    EditorSessionBuilder&&) noexcept = default;

EditorSessionBuilder& EditorSessionBuilder::bind(std::string commandId,
                                                 CommandHandler handler) {
    if (commandId.empty() || !handler) {
        throw std::invalid_argument{"command binding must have an ID and handler"};
    }

    if (!impl_->handlers.emplace(std::move(commandId), std::move(handler))
             .second) {
        throw std::invalid_argument{"duplicate command binding"};
    }
    return *this;
}

EditorSessionBuilder& EditorSessionBuilder::services(
    CommandServices& services) noexcept {
    impl_->services = &services;
    return *this;
}

std::unique_ptr<EditorSession> EditorSessionBuilder::build() {
    auto descriptors = p0CommandDescriptors();
    if (impl_->handlers.size() != descriptors.size()) {
        throw std::invalid_argument{
            "command bindings must equal the complete P0 catalog"};
    }

    std::vector<CommandRegistration> registrations;
    registrations.reserve(descriptors.size());
    for (auto& descriptor : descriptors) {
        auto found = impl_->handlers.find(descriptor.id);
        if (found == impl_->handlers.end()) {
            throw std::invalid_argument{"missing P0 command binding: " +
                                        descriptor.id};
        }
        registrations.push_back(
            {std::move(descriptor), std::move(found->second)});
    }
    impl_->handlers.clear();
    return std::make_unique<EditorSession>(
        CommandRegistry{
            std::vector<CommandSet>{CommandSet{std::move(registrations)}}},
        impl_->services);
}

}  // namespace ssg
