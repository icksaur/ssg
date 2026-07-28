#include <ssg/EditorSessionBuilder.h>

#include <ssg/ClipboardRegister.h>
#include <ssg/CommandCatalog.h>
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
