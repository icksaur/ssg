#include "editor_runtime_internal.h"

#include <ssg/CommandCatalog.h>

#include <ssg/CommandCatalog.h>
#include <ssg/ChromeLowering.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/UiTree.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <variant>

namespace ssg {

namespace {

// Overlay the live find/replace state onto resolved prompt controls. Templated
// over the control type so the grid PromptControlView and the geometry-free
// PromptControl share ONE value/checked resolution -- the only difference between
// the two renderings is geometry, never content.
template <typename Control>
void applyFindReplaceValues(std::vector<Control>& controls, PromptKind kind,
                            FindReplaceViewState const& findState) {
    if (kind != PromptKind::Find && kind != PromptKind::Replace) return;
    for (auto& control : controls) {
        switch (control.kind) {
        case PromptControlKind::Input:
            if (control.id == "find.query") control.value = findState.query;
            else if (control.id == "replace.replacement")
                control.value = findState.replacement;
            break;
        case PromptControlKind::Count: {
            auto position =
                findState.activeMatch ? *findState.activeMatch + 1 : 0;
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

// A resolver from projected status fields: id -> (value, label, command). Shared by
// the grid lowering and the semantic dynamic-state resolution so a composed
// provider widget resolves to the same values on either path.
ChromeProviderResolver chromeResolverFor(std::vector<StatusField> header,
                                         std::vector<StatusField> footer,
                                         std::string helpLabel) {
    return [header = std::move(header), footer = std::move(footer),
            helpLabel = std::move(helpLabel)](
               std::string_view id) -> std::optional<ResolvedProvider> {
        if (id == "footer.hint") {
            return ResolvedProvider{helpLabel, helpLabel, std::string{"help.open"}};
        }
        for (const auto* group : {&header, &footer}) {
            for (const auto& field : *group) {
                if (field.id == id) {
                    return ResolvedProvider{field.value, field.accessibleLabel,
                                            field.commandId};
                }
            }
        }
        return std::nullopt;
    };
}

std::string helpHintLabel(const KeymapViewState& keymap) {
    std::string keys;
    if (auto sequence = KeymapMatcher{keymap}.preferredBinding("help.open")) {
        keys = KeyCodec{}.formatSequence(*sequence);
    }
    return keys.empty() ? std::string{"help"} : keys + "  help";
}

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

PromptStatusViewState EditorRuntime::Impl::promptStatusView() const {
    // Semantic prompt state: which prompt is open (authoritative, present even for
    // a header-hosted prompt with no footer view) and the status queue. No
    // dimensions needed, so a semantic snapshot is obtainable without geometry.
    PromptStatusViewState view;
    if (interaction.prompt().request()) view.activeKind = interaction.prompt().request()->kind;
    view.status = status.viewState();
    return view;
}

std::optional<PromptViewState> EditorRuntime::Impl::promptProjection(
    ViewportDimensions dimensions,
    std::optional<Rect> promptReservation) const {
    // Grid projection of the footer-anchored interaction.prompt(). Single-source prompt rect:
    // when the shell laid out a footer-anchored prompt it passes that rect here,
    // so the controls are laid out into the SAME reservation the shell reserved
    // (identical a11y node, hit region, and render). The fallback -- a full-width
    // bottom strip derived from the viewport -- covers the palette (zero prompt
    // rows, its input lives in the header) and the no-active-prompt default.
    auto rows = promptRowCount(interaction.prompt().request() ? interaction.prompt().request()->kind : PromptKind::CommandArgument);
    Rect reservation =
        promptReservation.value_or(
            Rect{0, static_cast<int>(dimensions.rows > rows ? dimensions.rows - rows : 0),
                 static_cast<int>(dimensions.columns), static_cast<int>(rows)});
    auto promptLayout = computePromptLayout(interaction.prompt(), reservation);
    if (!promptLayout.accepted()) return std::nullopt;
    auto view = promptLayout.view;
    projectFindReplacePrompt(*view);
    return view;
}

void EditorRuntime::Impl::projectFindReplacePrompt(PromptViewState& promptView) const {
    applyFindReplaceValues(promptView.controls, promptView.kind,
                           findReplace.viewState());
}

std::optional<PromptView> EditorRuntime::Impl::promptView() const {
    auto const& request = interaction.prompt().request();
    if (!request) return std::nullopt;
    // Only a footer-region prompt is published here; the header-hosted palette
    // finder lives on its own semantic channel (PaletteViewState).
    if (promptFocusRegion(request->kind) != PromptRegion::Footer) {
        return std::nullopt;
    }
    PromptView view;
    view.kind = request->kind;
    view.accessibleLabel = request->accessibleLabel;
    view.controls = resolvePromptControls(*request);
    applyFindReplaceValues(view.controls, request->kind, findReplace.viewState());
    view.activeInput = interaction.prompt().activeInput();
    return view;
}

std::optional<NoticeView> EditorRuntime::Impl::draftNotice() const {
    // Only the Conflict outcome raises the notice; a Restored draft is a quieter
    // state with no external change to resolve. The action command ids are already
    // registered; the host only dispatches them.
    const auto id = activeDocumentId();
    if (!id) return std::nullopt;
    const auto found = documentRuntimeStates.find(id->value());
    if (found == documentRuntimeStates.end() ||
        found->second.reopen != DraftReopenOutcome::Conflict) {
        return std::nullopt;
    }
    return NoticeView{
        "Unsaved draft: file changed on disk externally.",
        {{"draft.notice.diff", "diff", "draft.diff"},
         {"draft.notice.use_disk", "use disk", "draft.discard"},
         {"draft.notice.dismiss", "dismiss", "draft.dismiss"}}};
}

std::optional<NoticeView> EditorRuntime::Impl::noticeView() const {
    return draftNotice();
}

bool EditorRuntime::Impl::noticePresent() const {
    return draftNotice().has_value();
}

StatusFieldProjection EditorRuntime::Impl::chromeStatusFields(
    ChromeFieldMode mode) const {
    auto statusProjection = status.footerProjection();
    auto followProjection = follow.footerProjection();
    auto fields = projectStatusFields(
        statusFieldCatalog, statusFieldProviders,
        {.workspaceRoot = root,
         .homeDirectory = homeDirectory,
         .currentBranch = currentGitBranch,
         .statusValue = statusProjection.value,
         .followMode = followProjection.mode,
         .cwdPrefix = mode == ChromeFieldMode::Grid ? style.cwdPrefix
                                                    : std::string{}});
    bindStatusFieldCommands(fields.header, followProjection);
    bindStatusFieldCommands(fields.footer, followProjection);
    return fields;
}

ShellViewState EditorRuntime::Impl::shellView(ViewportDimensions dimensions,
                                               PaletteReport const& paletteReport) const {
    std::vector<TabLabel> labels;
    for (auto const& tab : tabs.viewState().tabs) {
        labels.push_back({composedTabTitle(tab, style), tab.label,
                          tabs.viewState().active == tab.id, tab.dirty});
    }
    auto statusFields = chromeStatusFields(ChromeFieldMode::Grid);
    ShellLayoutRequest request;    request.viewport = {static_cast<int>(dimensions.columns), static_cast<int>(dimensions.rows)};
    request.reservedPromptRows = interaction.prompt().active() ? promptRowCount(interaction.prompt().request()->kind) : 0;
    request.lineNumberGutterWidth = lineNumberGutterWidth();
    // Surface the draft-conflict notice for the active document (M15) from the one
    // resolver; the grid ShellNotice is that geometry-free notice plus rects, added
    // by the shell layout. Reserving a chrome row (rather than stealing document
    // row 0) keeps the document's own coordinate space intact.
    if (auto notice = draftNotice()) {
        std::vector<ShellNoticeAction> actions;
        actions.reserve(notice->actions.size());
        for (auto const& action : notice->actions) {
            actions.push_back({action.id, action.label, action.command});
        }
        request.notice = ShellNotice{std::move(notice->text), std::move(actions)};
    }
    // Surface the external-modification bar (7A-5b) from the library-owned view
    // state: one row per externally-changed file (status glyph + path) carrying
    // its offered actions, plus the ABSOLUTE selected index. computeShellLayout
    // windows and bounds it; an empty section reserves zero rows.
    if (auto externalView = external.viewState(); !externalView.files.empty()) {
        auto actionLabel = [](ExternalAction action) -> std::string {
            switch (action) {
            case ExternalAction::Reload:
                return "Reload";
            case ExternalAction::KeepBuffer:
                return "Keep";
            case ExternalAction::OpenDiff:
                return "Diff";
            }
            return {};
        };
        auto actionCommand = [](ExternalAction action) -> std::string {
            switch (action) {
            case ExternalAction::Reload:
                return "external.reload";
            case ExternalAction::KeepBuffer:
                return "external.keep_buffer";
            case ExternalAction::OpenDiff:
                return "external.open_diff";
            }
            return {};
        };
        ShellExternalBar bar;
        for (auto const& file : externalView.files) {
            const char* glyph =
                file.status == ExternalDocumentStatus::ExternallyRemoved ? "D"
                                                                         : "M";
            ShellExternalRow row;
            row.fileId = file.id.value();
            row.text = std::string{glyph} + " " + file.path.string();
            for (auto const& action : file.actions) {
                row.actions.push_back(
                    {actionLabel(action), actionCommand(action)});
            }
            bar.rows.push_back(std::move(row));
        }
        bar.message = std::to_string(externalView.files.size()) +
                      (externalView.files.size() == 1 ? " file changed on disk"
                                                      : " files changed on disk");
        if (externalView.selected) {
            for (std::size_t i = 0; i < externalView.files.size(); ++i) {
                if (externalView.files[i].id == *externalView.selected) {
                    bar.selected = static_cast<std::uint32_t>(i);
                    break;
                }
            }
        }
        request.externalBar = std::move(bar);
    }
    request.emptyState = activeDocument() == nullptr;
    request.panelProviderLabel = std::string{panelProviderLabel(interaction.truth().selectedProvider)};
    request.panelPresent = interaction.truth().panelPresent;
    request.focus = interaction.effectiveFocus();
    auto promptStatus = promptStatusView();
    request.chromeProviderResolver = chromeResolverFor(
        std::move(statusFields.header), std::move(statusFields.footer),
        helpHintLabel(keymap));
    request.tabs = std::move(labels);
    request.style = style;
    // Anchoring decision: a HEADER-anchored prompt hosts its query in the header
    // input line. The query/ghost
    // text come from the picker report, which today only the palette populates.
    bool const headerPrompt = interaction.prompt().active() && interaction.prompt().request() &&
                              promptFocusRegion(interaction.prompt().request()->kind) ==
                                  PromptRegion::Header;
    // Picker identity: the palette picker specifically (drives candidate ranking
    // below). Distinct from the anchoring decision so a future non-palette header
    // prompt does not inherit palette-picker plumbing.
    bool const paletteOpen = interaction.prompt().active() && interaction.prompt().request() &&
                              interaction.prompt().request()->kind == PromptKind::Palette;
    // The prompt input's visibility is owned by presence (set when a header prompt
    // is open); its query/ghost text is a grid-only sidecar, ignored when hidden.
    PromptInputReport promptInput;
    if (headerPrompt) {
        promptInput.query = paletteReport.query;
        promptInput.ghost = paletteReport.ghost;
    }
    auto result = computeShellLayout(request, shell, interaction.interaction(),
                                     promptStatus.status, promptInput);
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

SessionSnapshotSections EditorRuntime::Impl::sections(
    PaletteReport const& paletteReport) const {
    auto currentHistory = HistoryViewState{false, false, 0};
    if (auto id = activeDocumentId()) {
        auto found = documentRuntimeStates.find(id->value());
        if (found != documentRuntimeStates.end()) {
            currentHistory = found->second.history.viewState();
        }
    }
    // The shell layout is computed by the caller (EditorRuntime::snapshot) before
    // this, warming the panel-height cache that treeView() and viewport() read.
    auto treeSection = treeView();
    const UiInteractionState& interactionState = interaction.interaction();
    const ValidatedSchema& validatedSchema = interactionState.schema();
    UiSchema uiSchema = validatedSchema.schema();
    // Schema, resolved state, and presence all derive from the interaction
    // authority's single ValidatedSchema, so they correspond node-for-node and
    // share one generation.
    if (!validateWellKnownAreas(uiSchema).ok()) {
        throw std::logic_error(
            "resolveUiState: composed schema violates the well-known-area contract");
    }
    UiStateSection uiState = [&] {
        auto fields = chromeStatusFields(ChromeFieldMode::Semantic);
        return resolveUiState(
            validatedSchema, chromeResolverFor(std::move(fields.header),
                                               std::move(fields.footer),
                                               helpHintLabel(keymap)));
    }();
    UiPresenceSection uiPresence =
        buildPresenceSection(validatedSchema, interactionState.presence());
    return {documentView(),
            selection.selections,
            currentHistory,
            clipboard.viewState(),
            promptStatusView(),
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
            interaction.legacyEffectiveFocus(),
            paletteView(),
            std::move(uiSchema),
            std::move(uiState),
            std::move(uiPresence),
            promptView(),
            noticeView(),
            watcherAvailable.load(std::memory_order_relaxed),
            interaction.effectiveFocus() == FocusTarget::ExternalModification};
}

TreeViewState EditorRuntime::Impl::treeView() const {
    // Semantic only: providers, nodes, selection, expansion. The grid scroll
    // window is a presentation projection produced by treeWindows().
    return tree.viewState();
}

std::vector<TreeWindow> EditorRuntime::Impl::treeWindows() const {
    auto view = tree.viewState();
    if (view.providers.empty()) return {};
    // Only the active (front) provider is rendered. Resolve a display window from
    // the command-set offset and the current client's panel height WITHOUT
    // persisting anything: keep-visible ran on the command path, so here we only
    // clamp the offset to this height and window the nodes. This keeps snapshot
    // generation a pure read (no cross-client scroll interference).
    auto const& provider = view.providers.front();
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
    TreeWindow window;
    window.firstVisible = scroll.firstVisible;
    window.scrollbar = scroll.scrollbar;
    window.visibleNodeIds.reserve(scroll.visibleCount);
    for (std::uint32_t row = 0; row < scroll.visibleCount; ++row) {
        window.visibleNodeIds.push_back(
            provider.nodes[scroll.firstVisible + row].node.id);
    }
    return {std::move(window)};
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
    if (!interaction.openPicker()) return view;
    auto const* picker = pickerCatalog().find(*interaction.openPicker());
    if (picker == nullptr) return view;
    view.mode = picker->wireMode;
    view.pickerEpoch = interaction.pickerEpoch();
    switch (*interaction.openPicker()) {
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
                        {command->id, command->displayLabel(), std::move(detail)});
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
