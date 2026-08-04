#include "editor_runtime_internal.h"

#include <ssg/CommandCatalog.h>

#include <ssg/CommandCatalog.h>
#include <ssg/command_metadata.h>
#include <ssg/PaletteSearcher.h>

#include <algorithm>
#include <cstdlib>
#include <variant>

namespace ssg {

namespace {

std::string composedTabTitle(const TabState& tab, const Style& style) {
    // Per-mode affordance: a live-diff tab keeps its prefix; a read-only tab
    // (help, generated output, or a binary/decode-failure buffer) gets a
    // trailing marker so the user knows why editing does nothing. An ordinary
    // editable tab is unadorned.
    if (tab.kind == TabKind::LiveDiff) {
        return style.tab.liveDiffPrefix + tab.label;
    }
    if (tab.mode == DocumentMode::ReadOnly) {
        return tab.label + style.tab.readOnlySuffix;
    }
    return tab.label;
}

std::optional<std::string> statusFieldCommandId(
    std::string_view fieldId,
    const FollowEditsFooterProjection& followProjection) {
    if (fieldId == "path") return std::string{"panel.show_files"};
    if (fieldId == "branch") return std::string{"panel.show_git_status"};
    if (fieldId == "follow") return followProjection.resumeCommand;
    return std::nullopt;
}

void bindStatusFieldCommands(std::vector<StatusField>& fields,
                             const FollowEditsFooterProjection& followProjection) {
    for (auto& field : fields) {
        field.commandId = statusFieldCommandId(field.id, followProjection);
    }
}

} // namespace

DocumentViewState EditorRuntime::Impl::documentView() const {
    auto const* document = activeDocument();
    if (document == nullptr) return {Revision{0}, {}, ByteOffset{0}, std::nullopt};
    auto snapshot = document->snapshot();
    auto publishedRevision = snapshot.revision;
    auto caret = selection.selections.primary().active.byteOffset;
    if (caret.value() > snapshot.text.size()) caret = ByteOffset{snapshot.text.size()};
    std::optional<std::string> diffFileIdentity;
    if (const auto* tab = activeTabState();
        tab && tab->kind == TabKind::LiveDiff &&
        !tab->contentIdentity.empty()) {
        diffFileIdentity = tab->contentIdentity;
        publishedRevision = diff.viewState().revision;
    }
    return {publishedRevision, std::move(snapshot.text), caret,
            std::move(diffFileIdentity)};
}

TextEncodingViewState EditorRuntime::Impl::textEncodingView() const {
    auto state = activeWorkspaceState();
    return {state ? state->encoding : TextEncodingStatus{}};
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
                                               PaletteReport const& paletteReport) const {
    std::vector<TabLabel> labels;
    for (auto const& tab : tabs.viewState().tabs) {
        labels.push_back({composedTabTitle(tab, style), tab.label,
                          tabs.viewState().active == tab.id, tab.dirty});
    }
    auto statusProjection = status.footerProjection();
    auto followProjection = follow.footerProjection();
    auto statusFields = projectStatusFields(
        statusFieldCatalog, statusFieldProviders,
        {.workspaceRoot = root,
         .homeDirectory = homeDirectory,
         .currentBranch = currentGitBranch,
         .statusValue = statusProjection.value,
         .followMode = followProjection.mode,
         .cwdPrefix = style.cwdPrefix});
    bindStatusFieldCommands(statusFields.headerFields, followProjection);
    bindStatusFieldCommands(statusFields.footerFields, followProjection);
    ShellLayoutRequest request;
    request.viewport = {static_cast<int>(dimensions.columns), static_cast<int>(dimensions.rows)};
    request.reservedPromptRows = prompt.active() ? promptRowCount(prompt.request()->kind) : 0;
    // Surface the draft-conflict notice for the active document (M15). Only the
    // Conflict outcome raises the yellow bar; a Restored draft is a quieter
    // state with no external change to resolve.
    if (auto const id = activeDocumentId()) {
        auto const found = documentRuntimeStates.find(id->value());
        if (found != documentRuntimeStates.end() &&
            found->second.reopen == DraftReopenOutcome::Conflict) {
            request.notice = ShellNotice{
                "Unsaved draft: file changed on disk externally.",
                {{"draft.notice.diff", "Diff", "draft.diff"},
                 {"draft.notice.use_disk", "Use disk", "draft.discard"},
                 {"draft.notice.dismiss", "Dismiss", "draft.dismiss"}}};
        }
    }
    request.emptyState = activeDocument() == nullptr;
    request.panelProviderLabel = std::string{shell.activePanelProvider()};
    request.headerFields = std::move(statusFields.headerFields);
    request.footerFields = std::move(statusFields.footerFields);
    request.footerActions = statusProjection.actions;
    // The persistent bottom-right help hint. Its key label tracks the live
    // binding for help.open (label-only when unbound); it is never a hardcoded
    // key string.
    {
        ShellFooterHint hint;
        hint.commandId = "help.open";
        std::string keys;
        if (auto sequence =
                KeymapMatcher{keymap}.preferredBinding("help.open")) {
            keys = KeyCodec{}.formatSequence(*sequence);
        }
        hint.label = keys.empty() ? std::string{"Help"} : keys + "  Help";
        request.footerHint = std::move(hint);
    }
    request.tabs = std::move(labels);
    request.style = style;
    bool const paletteOpen = prompt.active() && prompt.request() &&
                              prompt.request()->kind == PromptKind::Palette;
    if (paletteOpen) {
        request.inputLineActive = true;
        request.inputLineQuery = paletteReport.query;
        request.inputLineGhost = paletteReport.ghost;
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
        // The client owns palette ranking for latency and normally supplies the
        // windowed report. On the frame the palette OPENS, though, the client has
        // not yet adopted the just-published candidates, so its report is empty --
        // and without this the list would paint blank and only fill in a frame
        // later. Rank the published candidates here for that one frame (empty
        // query, top of the list) so the palette is populated the instant it
        // appears. Every later frame carries the client's own non-empty report,
        // so this is skipped; a genuine no-match query keeps its non-empty query
        // and is never overridden.
        PaletteReport seeded;
        PaletteReport const* source = &paletteReport;
        if (paletteReport.rows.empty() && paletteReport.query.empty()) {
            auto candidates = paletteView().candidates;
            if (!candidates.empty()) {
                PaletteWindowState window;
                window.paneRows = static_cast<std::uint32_t>(
                    std::max(view.panes.front().content.height, 1));
                seeded = PaletteSearcher{}.report(candidates, window);
                source = &seeded;
            }
        }
        PaletteProjection projection;
        projection.rect = view.panes.front().content;
        projection.scrollbarRect = view.panes.front().scrollbar;
        projection.selected = source->selected;
        projection.firstVisible = source->firstVisible;
        projection.scrollbar = source->scrollbar;
        for (auto const& candidate : source->rows) {
            projection.rows.push_back({candidate.label, candidate.detail});
        }
        view.palette = std::move(projection);
    }
    return view;
}

SessionSnapshotSections EditorRuntime::Impl::sections(ViewportDimensions dimensions,
                                                     PaletteReport const& paletteReport) const {
    auto currentHistory = HistoryViewState{false, false, 0};
    if (auto id = activeDocumentId()) {
        auto found = documentRuntimeStates.find(id->value());
        if (found != documentRuntimeStates.end()) {
            currentHistory = found->second.history.viewState();
        }
    }
    // Compute the shell layout first: it caches the panel height that tree_view
    // resolves the tree scroll offset against (the aggregate below does not
    // guarantee evaluation order).
    auto shell = shellView(dimensions, paletteReport);
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
            activeSyntaxView(),
            lspSync,
            lspFeatures,
            theme,
            style,
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
    auto offset = ScrollOffset{treeFirstVisible};
    offset.revealSelection(*selectedIndex,
                           static_cast<std::uint32_t>(provider.nodes.size()),
                           lastPanelContentRows);
    treeFirstVisible = offset.firstVisible();
}

void EditorRuntime::Impl::scrollTreeToFraction(std::uint32_t numerator,
                                                std::uint32_t denominator) {
    // Only the node COUNT is needed, and this runs per pointer motion during a
    // thumb drag, so it must not rebuild every provider's view.
    auto const nodes = tree.activeVisibleNodeCount();
    if (nodes == 0) return;
    // The panel's counterpart to view.scroll_to_fraction: a gutter click or
    // thumb drag positions the tree along its track. Same ScrollOffset the
    // editor uses, so both gutters map a pointer row to a position identically.
    auto offset = ScrollOffset{treeFirstVisible};
    offset.toFraction(numerator, denominator,
                      static_cast<std::uint32_t>(nodes), lastPanelContentRows);
    treeFirstVisible = offset.firstVisible();
}

void EditorRuntime::Impl::scrollTree(std::int64_t rows) {
    // Same reasoning as scrollTreeToFraction: the wheel path needs the count,
    // not the view.
    auto const nodes = tree.activeVisibleNodeCount();
    if (nodes == 0) return;
    // keep-visible is not applied: a wheel scroll moves the viewport, not the
    // selection (a later revealTreeSelection re-snaps). The saturating
    // clamp-shift lives in ScrollOffset, shared with every other surface.
    auto offset = ScrollOffset{treeFirstVisible};
    offset.byLines(rows, static_cast<std::uint32_t>(nodes),
                   lastPanelContentRows);
    treeFirstVisible = offset.firstVisible();
}

// Publishes the candidate set of whichever picker is open.  The mode and the
// candidate source both come from the picker descriptor, so a new picker adds a
// case here rather than a second hardcoded view.
PaletteViewState EditorRuntime::Impl::paletteView() const {
    PaletteViewState view;
    if (!openPicker) return view;
    auto const* picker = pickerCatalog().find(*openPicker);
    if (picker == nullptr) return view;
    view.mode = picker->wireMode;
    switch (*openPicker) {
    case PickerKind::Command:
        // Every registered command is a palette candidate, read from the live
        // catalog rather than a static list: a command registered by a plugin
        // is findable the moment it exists.  Resolving each command's key hint
        // is O(bindings x commands), so the result is cached and reused until
        // the catalog or keymap changes -- otherwise this ran every frame the
        // palette was open.
        {
            auto const catalogRevision = session->catalog()->revision();
            if (!commandCandidateCacheValid ||
                catalogRevision != commandCandidateCatalogRevision ||
                keymap != commandCandidateKeymap) {
                commandCandidateCache.clear();
                for (auto const* command : session->catalog()->commands()) {
                    std::string detail;
                    if (auto sequence =
                            KeymapMatcher{keymap}.preferredBinding(command->id)) {
                        detail = KeyCodec{}.formatSequence(*sequence);
                    }
                    commandCandidateCache.push_back(
                        {command->id, commandLabel(*command), std::move(detail)});
                }
                commandCandidateCatalogRevision = catalogRevision;
                commandCandidateKeymap = keymap;
                commandCandidateCacheValid = true;
            }
            view.candidates = commandCandidateCache;
        }
        break;
    case PickerKind::File:
        view.candidates = fileCandidates;
        break;
    }
    return view;
}

} // namespace ssg
