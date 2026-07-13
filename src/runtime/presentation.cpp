#include "editor_runtime_internal.h"

#include <algorithm>
#include <stdexcept>
#include <variant>

namespace ssg {
namespace {

template <typename T>
T const* payload_as(std::any const& payload) { return std::any_cast<T>(&payload); }

bool bool_setting(SettingsModel const& settings, SettingKey key, bool fallback) {
    auto value = settings.resolve(key).value;
    if (auto const* typed = std::get_if<bool>(&value)) return *typed;
    return fallback;
}

CommandHandlerResult set_word_wrap(EditorRuntime::Impl& runtime) {
    bool next = !bool_setting(runtime.settings, SettingKey::word_wrap, runtime.word_wrap);
    auto mutation = runtime.settings.set(SettingScope::workspace, SettingKey::word_wrap, next);
    if (!mutation.accepted()) return failure(mutation.error->message);
    runtime.word_wrap = next;
    return success();
}

CommandHandlerResult scroll_lines(EditorRuntime::Impl& runtime, std::any const& payload) {
    auto const* arguments = payload_as<ScrollLinesArguments>(payload);
    if (arguments == nullptr) return failure("view.scroll_lines requires scroll-lines payload");
    auto rows = static_cast<std::int64_t>(runtime.requested_first_visual_row) + arguments->rows;
    runtime.requested_first_visual_row = rows < 0 ? 0U : static_cast<std::uint32_t>(rows);
    runtime.selection.first_visual_row = runtime.requested_first_visual_row;
    return success();
}

CommandHandlerResult scroll_pages(EditorRuntime::Impl& runtime, std::any const& payload) {
    auto const* arguments = payload_as<ScrollPagesArguments>(payload);
    if (arguments == nullptr) return failure("view.scroll_pages requires scroll-pages payload");
    auto rows = static_cast<std::int64_t>(runtime.requested_first_visual_row) + arguments->pages * 24;
    runtime.requested_first_visual_row = rows < 0 ? 0U : static_cast<std::uint32_t>(rows);
    runtime.selection.first_visual_row = runtime.requested_first_visual_row;
    return success();
}

CommandHandlerResult scroll_fraction(EditorRuntime::Impl& runtime, std::any const& payload) {
    auto const* arguments = payload_as<ScrollFractionArguments>(payload);
    if (arguments == nullptr) return failure("view.scroll_to_fraction requires scroll-fraction payload");
    auto runs = runtime.active_cell_runs();
    auto view = compute_viewport(runs, ViewportDimensions{80, 24});
    runtime.requested_first_visual_row = arguments->denominator == 0 ? 0 :
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(view.scrollbar.maximum_first_row) * arguments->numerator) / arguments->denominator);
    runtime.selection.first_visual_row = runtime.requested_first_visual_row;
    return success();
}

CommandHandlerResult shell_command(EditorRuntime::Impl& runtime, std::string_view id) {
    if (id == "pane.split_horizontal") runtime.shell.split_active(SplitAxis::horizontal);
    else if (id == "pane.split_vertical") runtime.shell.split_active(SplitAxis::vertical);
    else if (id == "pane.close") (void)runtime.shell.close_active_pane();
    else if (id == "pane.next") runtime.shell.next_pane();
    else if (id == "pane.previous") runtime.shell.previous_pane();
    else if (id == "pane.focus_left") (void)runtime.shell.focus_pane(PaneDirection::left, runtime.shell_view(ViewportDimensions{80, 24}));
    else if (id == "pane.focus_right") (void)runtime.shell.focus_pane(PaneDirection::right, runtime.shell_view(ViewportDimensions{80, 24}));
    else if (id == "pane.focus_up") (void)runtime.shell.focus_pane(PaneDirection::up, runtime.shell_view(ViewportDimensions{80, 24}));
    else if (id == "pane.focus_down") (void)runtime.shell.focus_pane(PaneDirection::down, runtime.shell_view(ViewportDimensions{80, 24}));
    else if (id == "panel.toggle") runtime.shell.toggle_panel();
    else if (id == "panel.focus") (void)runtime.shell.focus_panel();
    else if (id == "panel.next_provider") runtime.shell.next_panel_provider();
    else if (id == "panel.previous_provider") runtime.shell.previous_panel_provider();
    else if (id == "view.toggle_distraction_free") runtime.shell.toggle_distraction_free();
    else return failure("unknown shell command");
    return success();
}

CommandHandlerResult prompt_status_command(EditorRuntime::Impl& runtime, std::string_view id, std::any const& payload) {
    if (id == "prompt.submit") {
        auto result = runtime.prompt.submit();
        return result.accepted() ? success() : failure(result.error->message);
    }
    if (id == "prompt.cancel") {
        auto result = runtime.prompt.cancel();
        return result.accepted() ? success() : failure(result.error->message);
    }
    if (id == "status.next") runtime.status.next();
    else if (id == "status.previous") runtime.status.previous();
    else if (id == "status.dismiss") runtime.status.dismiss();
    else if (id == "status.invoke_action") {
        auto const* invocation = payload_as<StatusActionInvocation>(payload);
        if (invocation == nullptr) return failure("status.invoke_action requires an action payload");
        auto result = runtime.status.invoke_action(*invocation);
        return result.accepted() ? success() : failure("status action is unavailable");
    }
    return success();
}

std::string setting_message(SettingMutation const& mutation) {
    return mutation.error ? mutation.error->message : "setting mutation failed";
}

void sync_runtime_settings(EditorRuntime::Impl& runtime) {
    runtime.word_wrap = bool_setting(runtime.settings, SettingKey::word_wrap,
                                     runtime.word_wrap);
}

CommandHandlerResult settings_command(EditorRuntime::Impl& runtime, std::string_view id, std::any const& payload) {
    if (id == "settings.open") {
        (void)runtime.prompt.open(PromptRequest{
            PromptKind::settings, "Settings",
            {{"settings.query", "Settings query", ""}}, {}, std::nullopt});
        return success();
    }
    if (id == "settings.export_workspace") {
        runtime.enqueue_status(StatusPriority::information, runtime.settings.export_scope(SettingScope::workspace));
        return success();
    }
    if (id == "settings.import_workspace") {
        auto const* document = payload_as<std::string>(payload);
        if (document == nullptr) return failure("settings.import_workspace requires a document payload");
        auto result = runtime.settings.import_scope(SettingScope::workspace, *document);
        sync_runtime_settings(runtime);
        return result.ok ? success() : failure(result.message);
    }
    if (id == "settings.set") {
        auto const* arguments = payload_as<SettingSetArguments>(payload);
        if (arguments == nullptr) {
            return failure("settings.set requires a typed settings payload");
        }
        auto mutation = runtime.settings.set(arguments->scope, arguments->key,
                                             arguments->value);
        if (!mutation.accepted()) return failure(setting_message(mutation));
        sync_runtime_settings(runtime);
        return success();
    }
    if (id == "settings.reset") {
        auto const* arguments = payload_as<SettingResetArguments>(payload);
        if (arguments == nullptr) {
            return failure("settings.reset requires a typed settings payload");
        }
        auto mutation = runtime.settings.reset(arguments->scope, arguments->key);
        if (!mutation.accepted()) return failure(setting_message(mutation));
        sync_runtime_settings(runtime);
        return success();
    }
    if (id == "settings.reset_scope") {
        auto const* arguments = payload_as<SettingResetScopeArguments>(payload);
        if (arguments == nullptr) {
            return failure("settings.reset_scope requires a typed settings payload");
        }
        try {
            for (auto const& entry : runtime.settings.view_state().entries) {
                if (!runtime.settings.scoped_value(arguments->scope, entry.key)) continue;
                auto mutation = runtime.settings.reset(arguments->scope, entry.key);
                if (!mutation.accepted()) return failure(setting_message(mutation));
            }
        } catch (std::invalid_argument const& error) {
            return failure(error.what());
        }
        sync_runtime_settings(runtime);
        return success();
    }
    return failure(std::string{id} + " requires a typed settings payload");
}

} // namespace

void bind_runtime_presentation(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    builder.bind("view.toggle_word_wrap", [&runtime](CommandContext&, std::any const&) {
        return runtime.run_transaction([&] { return set_word_wrap(runtime); });
    });
    builder.bind("view.scroll_lines", [&runtime](CommandContext&, std::any const& payload) {
        return runtime.run_transaction([&] { return scroll_lines(runtime, payload); });
    });
    builder.bind("view.scroll_pages", [&runtime](CommandContext&, std::any const& payload) {
        return runtime.run_transaction([&] { return scroll_pages(runtime, payload); });
    });
    builder.bind("view.scroll_to_fraction", [&runtime](CommandContext&, std::any const& payload) {
        return runtime.run_transaction([&] { return scroll_fraction(runtime, payload); });
    });
    for (auto const& descriptor : ShellCommandSet{}.descriptors) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const&) {
            return runtime.run_transaction([&] { return shell_command(runtime, descriptor.id); });
        });
    }
    for (auto const& descriptor : PromptStatusCommandSet{}.descriptors) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.run_transaction([&] { return prompt_status_command(runtime, descriptor.id, payload); });
        });
    }
    for (auto const& descriptor : SettingsCommandSet{}.descriptors) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.run_transaction([&] { return settings_command(runtime, descriptor.id, payload); });
        });
    }
}

} // namespace ssg
