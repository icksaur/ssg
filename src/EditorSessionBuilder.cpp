#include <ssg/EditorSessionBuilder.h>

#include <ssg/ClipboardRegister.h>
#include <ssg/CommandCatalog.h>
#include <ssg/Commands.h>
#include <ssg/Protocol.h>
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
    std::shared_ptr<CommandCatalog> catalog = std::make_shared<CommandCatalog>();
    CommandServices* services = nullptr;
};

EditorSessionBuilder::EditorSessionBuilder() : impl_{std::make_unique<Impl>()} {}
EditorSessionBuilder::~EditorSessionBuilder() = default;
EditorSessionBuilder::EditorSessionBuilder(EditorSessionBuilder&&) noexcept =
    default;
EditorSessionBuilder& EditorSessionBuilder::operator=(
    EditorSessionBuilder&&) noexcept = default;

EditorSessionBuilder& EditorSessionBuilder::add(CommandSpecBuilder spec) {
    impl_->catalog->add(std::move(spec));
    return *this;
}

EditorSessionBuilder& EditorSessionBuilder::bind(std::string commandId,
                                                 CommandHandler handler) {
    // Migration only: a component that has not yet moved still binds a handler
    // to an id declared in the static table, so the declaration is fetched from
    // there and joined to the handler here.  A migrated component calls `add`
    // and states both in one expression (doc/spec-command-registry.md, D5).
    if (commandId.empty() || !handler) {
        throw std::invalid_argument{"command binding must have an ID and handler"};
    }
    auto const* declared = findCommand(commandId);
    if (declared == nullptr) {
        throw std::invalid_argument{"bound command is not declared: " + commandId};
    }

    CommandSpecBuilder spec{std::string{declared->id}};
    spec.owner(std::string{declared->owner})
        .label(std::string{declared->label})
        .summary(std::string{declared->summary});
    if (declared->effect == CommandEffect::Mutation) {
        spec.mutates();
    } else {
        spec.observes();
    }
    for (auto const& capability : declared->requiredCapabilities) {
        spec.capability(std::string{capability});
    }
    if (declared->surfaces.initScript) {
        spec.initScript();
    } else if (declared->surfaces.luaApi) {
        spec.lua();
    }
    spec.adoptBoundHandler(std::move(handler),
                           argumentTypeForKind(declared->argument));
    impl_->catalog->add(std::move(spec));
    return *this;
}

EditorSessionBuilder& EditorSessionBuilder::services(
    CommandServices& services) noexcept {
    impl_->services = &services;
    return *this;
}

std::shared_ptr<CommandCatalog> EditorSessionBuilder::catalog() const {
    return impl_->catalog;
}

std::unique_ptr<EditorSession> EditorSessionBuilder::build() {
    // There is deliberately no "bindings must equal the catalog" check here.
    // It existed to prove every declared command had a handler and every
    // handler a declaration; registration now states both in one act, so the
    // check compared a thing to itself (doc/spec-command-registry.md).
    std::vector<CommandRegistration> registrations;
    for (auto const* command : impl_->catalog->commands()) {
        std::vector<CapabilityId> capabilities;
        capabilities.reserve(command->requiredCapabilities.size());
        for (auto const& capability : command->requiredCapabilities) {
            capabilities.emplace_back(capability);
        }
        registrations.push_back(
            {CommandDescriptor{command->id, command->effect,
                               std::move(capabilities)},
             command->handler});
    }
    return std::make_unique<EditorSession>(
        CommandRegistry{
            std::vector<CommandSet>{CommandSet{std::move(registrations)}}},
        impl_->services, impl_->catalog);
}

}  // namespace ssg
