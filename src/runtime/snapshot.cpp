#include "editor_runtime_internal.h"

#include <ssg/command_metadata.h>

#include <algorithm>
#include <variant>

namespace ssg {

DocumentViewState EditorRuntime::Impl::documentView() const {
    auto const* document = activeDocument();
    if (document == nullptr) return {Revision{0}, {}, ByteOffset{0}};
    auto snapshot = document->snapshot();
    auto caret = selection.selections.primary().active.byteOffset;
    if (caret.value() > snapshot.text.size()) caret = ByteOffset{snapshot.text.size()};
    return {snapshot.revision, std::move(snapshot.text), caret};
}

TextEncodingViewState EditorRuntime::Impl::textEncodingView() const {
    auto state = activeWorkspaceState();
    return {state ? state->encoding : TextEncodingStatus{}};
}

std::string EditorRuntime::Impl::currentPathLabel() const {
    auto state = activeWorkspaceState();
    return state ? state->displayLabel : root.filename().string();
}

PromptStatusViewState EditorRuntime::Impl::promptStatusView(ViewportDimensions dimensions) const {
    PromptStatusViewState view;
    auto rows = promptRowCount(prompt.request() ? prompt.request()->kind : PromptKind::CommandArgument);
    Rect reservation{0, static_cast<int>(dimensions.rows > rows ? dimensions.rows - rows : 0),
                     static_cast<int>(dimensions.columns), static_cast<int>(rows)};
    auto promptLayout = computePromptLayout(prompt, reservation);
    if (promptLayout.accepted()) {
        view.prompt = promptLayout.view;
        projectFindReplacePrompt(*view.prompt);
    }
    view.status = status.viewState();
    return view;
}

void EditorRuntime::Impl::projectFindReplacePrompt(PromptViewState& promptView) const {
    if (promptView.kind != PromptKind::Find && promptView.kind != PromptKind::Replace) {
        return;
    }
    auto const& findState = findReplace.viewState();
    for (auto& control : promptView.controls) {
        switch (control.kind) {
            case PromptControlKind::Input:
                if (control.id == "find.query") control.value = findState.query;
                else if (control.id == "replace.replacement")
                    control.value = findState.replacement;
                break;
            case PromptControlKind::Count: {
                auto position = findState.activeMatch ? *findState.activeMatch + 1 : 0;
                control.value = std::to_string(position) + "/" +
                                std::to_string(findState.matches.size());
                break;
            }
            case PromptControlKind::Toggle:
                if (control.id == "find.toggle_case")
                    control.checked = findState.options.caseSensitive;
                else if (control.id == "find.toggle_whole_word")
                    control.checked = findState.options.wholeWord;
                else if (control.id == "find.toggle_regex")
                    control.checked = findState.options.regex;
                break;
        }
    }
}

ShellViewState EditorRuntime::Impl::shellView(ViewportDimensions dimensions,
                                               KeySequence const& leaderPending,
                                               PaletteReport const& paletteReport) const {
    std::vector<TabLabel> labels;
    for (auto const& tab : tabs.viewState().tabs) {
        labels.push_back({tab.label, tab.label,
                          tabs.viewState().active == tab.id, tab.dirty});
    }
    auto statusProjection = status.footerProjection();
    auto followProjection = follow.footerProjection();
    ShellLayoutRequest request;
    request.viewport = {static_cast<int>(dimensions.columns), static_cast<int>(dimensions.rows)};
    request.reservedPromptRows = prompt.active() ? promptRowCount(prompt.request()->kind) : 0;
    request.emptyState = activeDocument() == nullptr;
    request.panelProviderLabel = std::string{shell.activePanelProvider()};
    request.headerFields = {{"cwd", "Workspace", root.string(), 0},
                             {"file", "File", currentPathLabel(), 1}};
    request.footerFields = {{"status", "Status", statusProjection.value, 0},
                             {"follow", "Follow edits", followProjection.mode, 1}};
    request.footerActions = statusProjection.actions;
    request.tabs = std::move(labels);
    if (!leaderPending.empty()) {
        std::string hint = "leader:";
        for (auto const& stroke : leaderPending) {
            hint += ' ';
            hint += KeyCodec{}.formatStroke(stroke);
        }
        request.leaderHint = std::move(hint);
    }
    bool const paletteOpen = prompt.active() && prompt.request() &&
                              prompt.request()->kind == PromptKind::Palette;
    if (paletteOpen) {
        request.paletteActive = true;
        request.paletteQuery = paletteReport.query;
        request.paletteGhost = paletteReport.ghost;
    }
    auto result = computeShellLayout(request, shell);
    if (!result.accepted()) return {};
    auto view = *result.view;

    if (!view.panes.empty()) {
        auto const& content = view.panes.front().content;
        lastPaneContentRows = static_cast<std::uint32_t>(std::max(content.height, 1));
        lastPaneContentColumns = static_cast<std::uint32_t>(std::max(content.width, 1));
        lastReservedPromptRows = static_cast<std::uint32_t>(request.reservedPromptRows);
    }
    // Cache the tree content height (panel height minus the provider-label row)
    // so tree_view can resolve the scroll offset against the real panel size.
    lastPanelContentRows =
        view.panel ? static_cast<std::uint32_t>(std::max(view.panel->height - 1, 0))
                   : 0;

    if (paletteOpen && !view.panes.empty()) {
        PaletteProjection projection;
        projection.rect = view.panes.front().content;
        projection.scrollbarRect = view.panes.front().scrollbar;
        projection.selected = paletteReport.selected;
        projection.firstVisible = paletteReport.firstVisible;
        projection.scrollbar = paletteReport.scrollbar;
        for (auto const& candidate : paletteReport.rows) {
            projection.rows.push_back({candidate.label, candidate.detail});
        }
        view.palette = std::move(projection);
    }
    return view;
}

SessionSnapshotSections EditorRuntime::Impl::sections(ViewportDimensions dimensions,
                                                     KeySequence const& leaderPending,
                                                     PaletteReport const& paletteReport) const {
    auto currentHistory = HistoryViewState{false, false, 0};
    if (auto id = activeDocumentId()) {
        auto found = histories.find(id->value());
        if (found != histories.end()) currentHistory = found->second.viewState();
    }
    // Compute the shell layout first: it caches the panel height that tree_view
    // resolves the tree scroll offset against (the aggregate below does not
    // guarantee evaluation order).
    auto shell = shellView(dimensions, leaderPending, paletteReport);
    auto treeSection = treeView();
    return {documentView(),
            selection,
            currentHistory,
            clipboard.viewState(),
            promptStatusView(dimensions),
            search.viewState(),
            findReplace.viewState(),
            settings.viewState(),
            keymap,
            textEncodingView(),
            tabs.viewState(),
            diff.viewState(),
            external.viewState(),
            follow.viewState(),
            std::move(treeSection),
            syntax.viewState(),
            lspSync,
            lspFeatures,
            theme,
            std::move(shell),
            paletteView()};
}

TreeViewState EditorRuntime::Impl::treeView() const {
    auto view = tree.viewState();
    if (view.providers.empty()) return view;
    // Only the active (front) provider is rendered. Resolve a display window from
    // the command-set offset and the current client's panel height WITHOUT
    // persisting anything: keep-visible ran on the command path, so here we only
    // clamp the offset to this height and window the nodes. This keeps snapshot
    // generation a pure read (no cross-client scroll interference).
    auto& provider = view.providers.front();
    std::optional<std::uint32_t> selectedIndex;
    if (provider.selected) {
        for (std::size_t i = 0; i < provider.nodes.size(); ++i) {
            if (provider.nodes[i].node.id == *provider.selected) {
                selectedIndex = static_cast<std::uint32_t>(i);
                break;
            }
        }
    }
    auto scroll = Viewport{}.listScrollView(
        static_cast<std::uint32_t>(provider.nodes.size()),
        lastPanelContentRows, treeFirstVisible, selectedIndex,
        /*keep_selection_visible=*/false);
    provider.firstVisible = scroll.firstVisible;
    provider.scrollbar = scroll.scrollbar;
    provider.visibleNodeIds.clear();
    provider.visibleNodeIds.reserve(scroll.visibleCount);
    for (std::uint32_t row = 0; row < scroll.visibleCount; ++row) {
        provider.visibleNodeIds.push_back(
            provider.nodes[scroll.firstVisible + row].node.id);
    }
    return view;
}

void EditorRuntime::Impl::revealTreeSelection() {
    auto view = tree.viewState();
    if (view.providers.empty()) return;
    auto const& provider = view.providers.front();
    if (!provider.selected) return;
    std::optional<std::uint32_t> selectedIndex;
    for (std::size_t i = 0; i < provider.nodes.size(); ++i) {
        if (provider.nodes[i].node.id == *provider.selected) {
            selectedIndex = static_cast<std::uint32_t>(i);
            break;
        }
    }
    if (!selectedIndex) return;
    auto scroll = Viewport{}.listScrollView(
        static_cast<std::uint32_t>(provider.nodes.size()),
        lastPanelContentRows, treeFirstVisible, selectedIndex,
        /*keep_selection_visible=*/true);
    treeFirstVisible = scroll.firstVisible;
}

void EditorRuntime::Impl::scrollTree(std::int64_t rows) {
    auto view = tree.viewState();
    if (view.providers.empty()) return;
    auto const& provider = view.providers.front();
    // Resolve the current scroll geometry (read-only) to bound the offset, then
    // shift it by `rows`. keep_selection_visible is false: a wheel scroll moves
    // the viewport, not the selection (a later reveal_tree_selection re-snaps).
    auto scroll = Viewport{}.listScrollView(
        static_cast<std::uint32_t>(provider.nodes.size()),
        lastPanelContentRows, treeFirstVisible, std::nullopt,
        /*keep_selection_visible=*/false);
    // Saturating add-then-clamp: `rows` is a wire-decoded int64 and may be huge,
    // so guard the extremes before the add to avoid signed overflow. Within the
    // guarded range |rows| < maximum <= UINT32_MAX, so cur + rows cannot overflow.
    auto const maximum = static_cast<std::int64_t>(scroll.scrollbar.maximumFirstRow);
    auto const current = static_cast<std::int64_t>(treeFirstVisible);
    std::int64_t next;
    if (rows >= maximum) {
        next = maximum;
    } else if (rows <= -maximum) {
        next = 0;
    } else {
        next = std::clamp<std::int64_t>(current + rows, 0, maximum);
    }
    treeFirstVisible = static_cast<std::uint32_t>(next);
}

PaletteViewState EditorRuntime::Impl::paletteView() const {
    PaletteViewState view;
    view.mode = SearchMode::Command;
    for (auto const& descriptor : descriptors()) {
        std::string detail;
        if (auto sequence = KeymapMatcher{keymap}.preferredBinding(descriptor.id)) {
            detail = KeyCodec{}.formatSequence(*sequence);
        }
        view.candidates.push_back(
            {descriptor.id, commandLabel(descriptor.id), std::move(detail)});
    }
    return view;
}

} // namespace ssg
