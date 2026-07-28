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
    // Nothing is copied out of the catalog here.  The session holds it, so a
    // command registered after this returns is dispatchable without any
    // propagation step -- and cannot appear in the palette while being refused
    // by dispatch (doc/spec-command-registry.md, R8).
    return std::make_unique<EditorSession>(impl_->catalog, impl_->services);
}

}  // namespace ssg
