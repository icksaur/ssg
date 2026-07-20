#include "editor_runtime_internal.h"

#include <algorithm>
#include <stdexcept>
#include <variant>

namespace ssg {
namespace {

template <typename T>
T const* payloadAs(std::any const& payload) { return std::any_cast<T>(&payload); }

bool boolSetting(SettingsModel const& settings, SettingKey key, bool fallback) {
    auto value = settings.resolve(key).value;
    if (auto const* typed = std::get_if<bool>(&value)) return *typed;
    return fallback;
}

CommandHandlerResult setWordWrap(EditorRuntime::Impl& runtime) {
    bool next = !boolSetting(runtime.settings, SettingKey::WordWrap, runtime.word_wrap);
    auto mutation = runtime.settings.set(SettingScope::Workspace, SettingKey::WordWrap, next);
    if (!mutation.accepted()) return failure(mutation.error->message);
    runtime.word_wrap = next;
    return success();
}

CommandHandlerResult scrollLines(EditorRuntime::Impl& runtime, std::any const& payload) {
    auto const* arguments = payloadAs<ScrollLinesArguments>(payload);
    if (arguments == nullptr) return failure("view.scroll_lines requires scroll-lines payload");
    auto rows = static_cast<std::int64_t>(runtime.requested_first_visual_row) + arguments->rows;
    runtime.requested_first_visual_row = rows < 0 ? 0U : static_cast<std::uint32_t>(rows);
    runtime.selection.first_visual_row = runtime.requested_first_visual_row;
    return success();
}

CommandHandlerResult scrollPages(EditorRuntime::Impl& runtime, std::any const& payload) {
    auto const* arguments = payloadAs<ScrollPagesArguments>(payload);
    if (arguments == nullptr) return failure("view.scroll_pages requires scroll-pages payload");
    // A page is the real pane height cached from the last snapshot, not a fake 24.
    auto const page_rows = static_cast<std::int64_t>(
        std::max<std::uint32_t>(runtime.last_pane_content_rows, 1));
    auto rows = static_cast<std::int64_t>(runtime.requested_first_visual_row) + arguments->pages * page_rows;
    runtime.requested_first_visual_row = rows < 0 ? 0U : static_cast<std::uint32_t>(rows);
    runtime.selection.first_visual_row = runtime.requested_first_visual_row;
    return success();
}

CommandHandlerResult scrollFraction(EditorRuntime::Impl& runtime, std::any const& payload) {
    auto const* arguments = payloadAs<ScrollFractionArguments>(payload);
    if (arguments == nullptr) return failure("view.scroll_to_fraction requires scroll-fraction payload");
    // Resolve maximum_first_row against the REAL pane cached from the last
    // snapshot, so a scrollbar drag to the bottom reaches the true last line on a
    // terminal that is not 24 rows tall (see doc/spec-scroll.md R6). Route through
    // the same wrap-gated viewport the snapshot uses so the drag maps to the same
    // total the scrollbar thumb was drawn from (M12).
    ViewportDimensions const viewport{
        std::max<std::uint32_t>(runtime.last_pane_content_columns, 1),
        std::max<std::uint32_t>(runtime.last_pane_content_rows, 1)};
    auto view = runtime.computeEditorViewport(viewport, 0, 0);
    runtime.requested_first_visual_row = arguments->denominator == 0 ? 0 :
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(view.scrollbar.maximum_first_row) * arguments->numerator) / arguments->denominator);
    runtime.selection.first_visual_row = runtime.requested_first_visual_row;
    return success();
}

CommandHandlerResult shellCommand(EditorRuntime::Impl& runtime, std::string_view id) {
    if (id == "pane.split_horizontal") runtime.shell.splitActive(SplitAxis::Horizontal);
    else if (id == "pane.split_vertical") runtime.shell.splitActive(SplitAxis::Vertical);
    else if (id == "pane.close") (void)runtime.shell.closeActivePane();
    else if (id == "pane.next") runtime.shell.nextPane();
    else if (id == "pane.previous") runtime.shell.previousPane();
    else if (id == "pane.focus_left") (void)runtime.shell.focusPane(PaneDirection::Left, runtime.shellView(ViewportDimensions{80, 24}));
    else if (id == "pane.focus_right") (void)runtime.shell.focusPane(PaneDirection::Right, runtime.shellView(ViewportDimensions{80, 24}));
    else if (id == "pane.focus_up") (void)runtime.shell.focusPane(PaneDirection::Up, runtime.shellView(ViewportDimensions{80, 24}));
    else if (id == "pane.focus_down") (void)runtime.shell.focusPane(PaneDirection::Down, runtime.shellView(ViewportDimensions{80, 24}));
    else if (id == "panel.toggle") runtime.shell.togglePanel();
    else if (id == "panel.focus") (void)runtime.shell.focusPanel();
    else if (id == "panel.next_provider") runtime.shell.nextPanelProvider();
    else if (id == "panel.previous_provider") runtime.shell.previousPanelProvider();
    else if (id == "view.toggle_distraction_free") runtime.shell.toggleDistractionFree();
    else return failure("unknown shell command");
    return success();
}

CommandHandlerResult promptStatusCommand(EditorRuntime::Impl& runtime, std::string_view id, std::any const& payload) {
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
        auto const* invocation = payloadAs<StatusActionInvocation>(payload);
        if (invocation == nullptr) return failure("status.invoke_action requires an action payload");
        auto result = runtime.status.invokeAction(*invocation);
        return result.accepted() ? success() : failure("status action is unavailable");
    }
    return success();
}

std::string settingMessage(SettingMutation const& mutation) {
    return mutation.error ? mutation.error->message : "setting mutation failed";
}

void syncRuntimeSettings(EditorRuntime::Impl& runtime) {
    runtime.word_wrap = boolSetting(runtime.settings, SettingKey::WordWrap,
                                     runtime.word_wrap);
}

CommandHandlerResult settingsCommand(EditorRuntime::Impl& runtime, std::string_view id, std::any const& payload) {
    if (id == "settings.open") {
        (void)runtime.prompt.open(PromptRequest{
            PromptKind::Settings, "Settings",
            {{"settings.query", "Settings query", ""}}, {}, std::nullopt});
        return success();
    }
    if (id == "settings.export_workspace") {
        runtime.enqueueStatus(StatusPriority::Information, runtime.settings.exportScope(SettingScope::Workspace));
        return success();
    }
    if (id == "settings.import_workspace") {
        auto const* document = payloadAs<std::string>(payload);
        if (document == nullptr) return failure("settings.import_workspace requires a document payload");
        auto result = runtime.settings.importScope(SettingScope::Workspace, *document);
        syncRuntimeSettings(runtime);
        return result.ok ? success() : failure(result.message);
    }
    if (id == "settings.set") {
        auto const* arguments = payloadAs<SettingSetArguments>(payload);
        if (arguments == nullptr) {
            return failure("settings.set requires a typed settings payload");
        }
        auto mutation = runtime.settings.set(arguments->scope, arguments->key,
                                             arguments->value);
        if (!mutation.accepted()) return failure(settingMessage(mutation));
        syncRuntimeSettings(runtime);
        return success();
    }
    if (id == "settings.reset") {
        auto const* arguments = payloadAs<SettingResetArguments>(payload);
        if (arguments == nullptr) {
            return failure("settings.reset requires a typed settings payload");
        }
        auto mutation = runtime.settings.reset(arguments->scope, arguments->key);
        if (!mutation.accepted()) return failure(settingMessage(mutation));
        syncRuntimeSettings(runtime);
        return success();
    }
    if (id == "settings.reset_scope") {
        auto const* arguments = payloadAs<SettingResetScopeArguments>(payload);
        if (arguments == nullptr) {
            return failure("settings.reset_scope requires a typed settings payload");
        }
        try {
            for (auto const& entry : runtime.settings.viewState().entries) {
                if (!runtime.settings.scopedValue(arguments->scope, entry.key)) continue;
                auto mutation = runtime.settings.reset(arguments->scope, entry.key);
                if (!mutation.accepted()) return failure(settingMessage(mutation));
            }
        } catch (std::invalid_argument const& error) {
            return failure(error.what());
        }
        syncRuntimeSettings(runtime);
        return success();
    }
    return failure(std::string{id} + " requires a typed settings payload");
}

} // namespace

void bindRuntimePresentation(EditorSessionBuilder& builder, EditorRuntime::Impl& runtime) {
    builder.bind("view.toggle_word_wrap", [&runtime](CommandContext&, std::any const&) {
        return runtime.runTransaction([&] { return setWordWrap(runtime); });
    });
    builder.bind("view.scroll_lines", [&runtime](CommandContext&, std::any const& payload) {
        return runtime.runTransaction([&] { return scrollLines(runtime, payload); });
    });
    builder.bind("view.scroll_pages", [&runtime](CommandContext&, std::any const& payload) {
        return runtime.runTransaction([&] { return scrollPages(runtime, payload); });
    });
    builder.bind("view.scroll_to_fraction", [&runtime](CommandContext&, std::any const& payload) {
        return runtime.runTransaction([&] { return scrollFraction(runtime, payload); });
    });
    for (auto const& descriptor : ShellCommandSet{}.descriptors) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const&) {
            return runtime.runTransaction([&] { return shellCommand(runtime, descriptor.id); });
        });
    }
    for (auto const& descriptor : PromptStatusCommandSet{}.descriptors) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.runTransaction([&] { return promptStatusCommand(runtime, descriptor.id, payload); });
        });
    }
    for (auto const& descriptor : SettingsCommandSet{}.descriptors) {
        builder.bind(std::string{descriptor.id}, [&runtime, descriptor](CommandContext&, std::any const& payload) {
            return runtime.runTransaction([&] { return settingsCommand(runtime, descriptor.id, payload); });
        });
    }
}

} // namespace ssg
