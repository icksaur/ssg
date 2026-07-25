#include <ssg/EditorSessionBuilder.h>

#include <ssg/ClipboardRegister.h>
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

template <typename Range>
void appendIds(std::vector<CommandDescriptor>& output, Range const& range) {
    for (auto const& descriptor : range) {
        std::vector<CapabilityId> capabilities;
        if (descriptor.id == std::string_view{"file.open_dropped_content"}) {
            capabilities.emplace_back("local_file_drop");
        }
        output.push_back({std::string{descriptor.id}, CommandEffect::Mutation,
                          std::move(capabilities)});
    }
}

}  // namespace

std::vector<CommandDescriptor> p0CommandDescriptors() {
    std::vector<CommandDescriptor> result;
    result.reserve(165);

    appendIds(result, textInputCommandSet().descriptors());
    appendIds(result, selectionNavigationCommandSet().descriptors());
    appendIds(result, historyCommandSet().descriptors());
    appendIds(result, editCommandSuiteCommandSet().descriptors());
    appendIds(result, clipboardCommandSet().descriptors());
    appendIds(result, searchCommandSet().descriptors());
    appendIds(result, lspFeatureCommandSet().descriptors());
    appendIds(result, lspWorkspaceEditCommandSet().descriptors());
    appendIds(result, findReplaceCommandSet().descriptors());
    appendIds(result, ShellCommandSet{}.descriptors);
    appendIds(result, treeCommandSet().descriptors());
    appendIds(result, PromptStatusCommandSet{}.descriptors);
    appendIds(result, fileCommandsCommandSet().descriptors());
    appendIds(result, kTextEncodingCommandSet.descriptors);
    appendIds(result, tabManagementCommandSet().descriptors());
    appendIds(result, externalModificationCommandSet().descriptors());
    appendIds(result, SettingsCommandSet{}.descriptors);
    appendIds(result, followEditsCommandSet().descriptors());
    appendIds(result, diffCommandSet().descriptors());
    appendIds(result, ThemeCommandSet{}.descriptors);
    appendIds(result, KeymapCommandSet{}.descriptors);

    constexpr std::array<std::string_view, 4> viewportCommands{
        "view.toggle_word_wrap", "view.scroll_lines", "view.scroll_pages",
        "view.scroll_to_fraction"};
    struct ViewportDescriptor {
        std::string_view id;
    };
    std::array<ViewportDescriptor, viewportCommands.size()> viewport{};
    for (std::size_t index = 0; index < viewport.size(); ++index) {
        viewport[index].id = viewportCommands[index];
    }
    appendIds(result, viewport);

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
