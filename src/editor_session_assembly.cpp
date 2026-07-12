#include <ssg/editor_session_assembly.h>

#include <ssg/clipboard.h>
#include <ssg/diff.h>
#include <ssg/edit_commands.h>
#include <ssg/external_modification.h>
#include <ssg/file_commands.h>
#include <ssg/find_replace.h>
#include <ssg/follow_edits.h>
#include <ssg/history.h>
#include <ssg/lsp_features.h>
#include <ssg/lsp_workspace_edit.h>
#include <ssg/prompt.h>
#include <ssg/search.h>
#include <ssg/selection.h>
#include <ssg/settings.h>
#include <ssg/tabs.h>
#include <ssg/text_encoding.h>
#include <ssg/text_input_commands.h>
#include <ssg/tree.h>
#include <ssg/ui_layout.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ssg {
namespace {

template <typename Range>
void append_ids(std::vector<CommandDescriptor>& output, Range const& range) {
    for (auto const& descriptor : range) {
        std::vector<CapabilityId> capabilities;
        if (descriptor.id == std::string_view{"file.open_dropped_content"}) {
            capabilities.emplace_back("local_file_drop");
        }
        output.push_back({std::string{descriptor.id}, CommandEffect::mutation,
                          std::move(capabilities)});
    }
}

}  // namespace

std::vector<CommandDescriptor> p0_command_descriptors() {
    std::vector<CommandDescriptor> result;
    result.reserve(161);

    append_ids(result, text_input_command_set().descriptors());
    append_ids(result, selection_navigation_command_set().descriptors());
    append_ids(result, history_command_set().descriptors());
    append_ids(result, edit_command_suite_command_set().descriptors());
    append_ids(result, clipboard_command_set().descriptors());
    append_ids(result, search_command_set().descriptors());
    append_ids(result, lsp_feature_command_set().descriptors());
    append_ids(result, lsp_workspace_edit_command_set().descriptors());
    append_ids(result, find_replace_command_set().descriptors());
    append_ids(result, ShellCommandSet{}.descriptors);
    append_ids(result, tree_command_set().descriptors());
    append_ids(result, PromptStatusCommandSet{}.descriptors);
    append_ids(result, file_commands_command_set().descriptors());
    append_ids(result, text_encoding_command_set.descriptors);
    append_ids(result, tab_management_command_set().descriptors());
    append_ids(result, external_modification_command_set().descriptors());
    append_ids(result, SettingsCommandSet{}.descriptors);
    append_ids(result, follow_edits_command_set().descriptors());
    append_ids(result, diff_command_set().descriptors());

    constexpr std::array<std::string_view, 4> viewport_commands{
        "view.toggle_word_wrap", "view.scroll_lines", "view.scroll_pages",
        "view.scroll_to_fraction"};
    struct ViewportDescriptor {
        std::string_view id;
    };
    std::array<ViewportDescriptor, viewport_commands.size()> viewport{};
    for (std::size_t index = 0; index < viewport.size(); ++index) {
        viewport[index].id = viewport_commands[index];
    }
    append_ids(result, viewport);

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

EditorSessionBuilder& EditorSessionBuilder::bind(std::string command_id,
                                                 CommandHandler handler) {
    if (command_id.empty() || !handler) {
        throw std::invalid_argument{"command binding must have an ID and handler"};
    }

    if (!impl_->handlers.emplace(std::move(command_id), std::move(handler))
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
    auto descriptors = p0_command_descriptors();
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
