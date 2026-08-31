#pragma once

#include "legacy_grid_frame.h"

// Kind: seam.
//
// Builds a SessionSnapshot directly, so a test of presentation does not have to
// stand up an EditorSession over a real directory first.
//
// The renderer is already a pure function of a snapshot
// (`Renderer::render(SessionSnapshot const&)`), but a snapshot used to be
// obtainable only from a runtime.  So every presentation test created temp
// directories, wrote files to disk, constructed the whole editor, attached a
// client, and dispatched commands -- eight steps of setup to exercise one pure
// function.  test_render.cpp alone did that 31 times.
//
// This builder ASSEMBLES PRODUCTION COMPONENTS; it does not reimplement them.
// The viewport projection comes from `Viewport::computeUnwrapped`, the shell
// from the solved UI tree, syntax from `SyntaxViewState::plainText`, and colour
// from `defaultTheme`.  A test using it therefore exercises the same projection
// the runtime does.  Anything it cannot express is set through `sections()`.
//
// It is deliberately test-only: no production caller needs it, so it does not
// enlarge the public library surface.

#include <ssg/GraphemeLayout.h>
#include <ssg/GridPresenter.h>
#include <ssg/InteractionState.h>
#include <ssg/Renderer.h>
#include <ssg/ShellState.h>
#include <ssg/StatusQueue.h>
#include <ssg/WholeScreenAssembly.h>
#include <ssg/WholeScreenInteraction.h>
#include <ssg/SyntaxModel.h>
#include <ssg/Theme.h>
#include <ssg/Viewport.h>
#include <ssg/session_snapshot.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ssg::test {

class SessionSnapshotBuilder {
public:
    struct TabSpec {
        std::string title;
        std::string accessibleLabel;
        bool active = false;
        bool dirty = false;
    };

    // The document text the viewport projects and the renderer paints.
    SessionSnapshotBuilder& document(std::string text) {
        text_ = std::move(text);
        return *this;
    }

    // Caret position as a byte offset, clamped to the document on build.
    SessionSnapshotBuilder& caret(std::size_t offset) {
        caret_ = offset;
        return *this;
    }

    SessionSnapshotBuilder& viewport(int columns, int rows) {
        columns_ = columns;
        rows_ = rows;
        return *this;
    }

    SessionSnapshotBuilder& firstVisualRow(std::uint32_t row) {
        firstRow_ = row;
        return *this;
    }

    SessionSnapshotBuilder& revision(Revision value) {
        revision_ = value;
        return *this;
    }

    // Show the side panel, and give the tree the supplied providers.
    SessionSnapshotBuilder& panel(bool shown) {
        panel_ = shown;
        return *this;
    }

    // When the panel is shown, whether it holds focus. Showing a panel focuses
    // it by default (matching togglePanel); pass false to leave focus in the
    // editor, which is the only state that paints the unfocused-panel roles.
    SessionSnapshotBuilder& panelFocused(bool focused) {
        panelFocused_ = focused;
        return *this;
    }

    SessionSnapshotBuilder& tabs(std::vector<TabSpec> labels) {
        tabs_ = std::move(labels);
        return *this;
    }

    // Escape hatch: mutate any section the named setters do not cover (find
    // matches, tree providers, diff state, ...).  Applied last, so it wins.
    SessionSnapshotBuilder& sections(
        std::function<void(SessionSnapshotSections&)> mutate) {
        mutators_.push_back(std::move(mutate));
        return *this;
    }

    SessionSnapshotBuilder& noticePresent(bool present = true) {
        noticePresent_ = present;
        return *this;
    }

    SessionSnapshotBuilder& externalModificationPresent(bool present = true) {
        externalModificationPresent_ = present;
        return *this;
    }

    SessionSnapshotBuilder& chromeProviderResolver(
        ChromeProviderResolver resolver) {
        chromeProviderResolver_ = std::move(resolver);
        return *this;
    }

    // Mutate the projected legacy shell after layout, for migration tests that
    // prove a solved-tree consumer no longer reads its former sidecar.
    SessionSnapshotBuilder& shellProjection(
        std::function<void(ShellViewState&)> mutate) {
        shellProjectionMutators_.push_back(std::move(mutate));
        return *this;
    }

    // Override the grid-presentation Style (a projection field, not a semantic
    // section). Applied when the PresentationSnapshot is built.
    SessionSnapshotBuilder& style(Style style) {
        style_ = std::move(style);
        return *this;
    }

    SessionSnapshotBuilder& paletteReport(PaletteReport palette) {
        palette_ = std::move(palette);
        return *this;
    }

    // Drive the header prompt input (palette query line): when `visible`, make it
    // present and set the grid-only query/ghost sidecar, as the runtime does when a
    // picker is open. When not visible, the input stays hidden.
    SessionSnapshotBuilder& promptInput(bool visible, std::string query,
                                        std::string ghost = {}) {
        if (visible)
            promptInput_ = PromptInput{std::move(query), std::move(ghost)};
        return *this;
    }

    SessionSnapshotBuilder& schema(ValidatedSchema schema) {
        schema_ = std::move(schema);
        return *this;
    }

    SessionSnapshotBuilder& status(StatusViewState status) {
        status_ = std::move(status);
        return *this;
    }

    [[nodiscard]] GridFrame build() const {
        auto const caret = caret_ > text_.size() ? text_.size() : caret_;

        // ShellState now owns only panes + distraction-free; panel presence and focus are
        // authority-owned in production and come from the request/sections here, both
        // derived from the SAME builder-owned values so the snapshot is coherent.
        ShellState shell;
        const FocusTarget focus = (panel_ && panelFocused_) ? FocusTarget::Panel
                                                            : FocusTarget::Editor;

        const auto statusActions = projectStatusActionNodes(status_);
        UiComposition composition;
        composition.root =
            schema_
                ? schema_->schema().root
                : assembleWholeScreen({}, "help.open", style_.dimensions,
                                      style_.inputLineSigil, std::nullopt)
                      .root;
        UiSchema effectiveUiSchema;
        effectiveUiSchema.root =
            withStatusActions(std::move(composition), statusActions).root;
        auto effectiveSchema =
            ValidatedSchema::validate(std::move(effectiveUiSchema)).takeSchema();
        const ValidatedSchema& schema = effectiveSchema;
        WholeScreenTruth truth;
        truth.panelPresent = panel_;
        truth.baseFocus = focus == FocusTarget::Panel ? BaseFocus::Panel
                                                      : BaseFocus::Editor;
        truth.noticePresent = noticePresent_;
        truth.externalModificationPresent = externalModificationPresent_;
        if (promptInput_) truth.openPicker = PickerKind::Command;
        UiInteractionState interaction = buildWholeScreenInteraction(
            schema, truth,
            promptInput_ ? std::optional{PromptRegion::Header} : std::nullopt);
        ViewportDimensions const dimensions{
            static_cast<std::uint32_t>(columns_),
            static_cast<std::uint32_t>(rows_)};
        auto viewportState = Viewport{}.computeUnwrapped(text_, dimensions,
                                                          firstRow_, 0, 4);

        SessionSnapshotSections sections{
            DocumentViewState{revision_, text_, ByteOffset{caret}, std::nullopt},
            SelectionSet{{Selection{caretPosition(caret),
                                    caretPosition(caret)}}},
            HistoryViewState{},
            ClipboardViewState{},
            PromptStatusViewState{
                status_,
                promptInput_ ? std::optional<PromptKind>{PromptKind::Palette}
                             : std::nullopt},
            SearchViewState{},
            FindReplaceViewState{},
            SettingsViewState{},
            KeymapViewState{},
            TextEncodingViewState{},
            TabViewState{},
            DiffViewState{},
            ExternalModificationViewState{},
            FollowEditsViewState{0, FollowMode::Following, PaneId{0},
                                  std::nullopt, {}, {}},
            TreeViewState{},
            SyntaxViewState::plainText(revision_, LanguageId{"plain"}, text_, 4),
            LspSyncViewState{},
            LspFeatureViewState{},
            defaultTheme(),
            focus,
            PaletteViewState{}};
        const ChromeProviderResolver resolver =
            [configured = chromeProviderResolver_,
             statusActions](std::string_view id)
                -> std::optional<ResolvedProvider> {
            if (configured) {
                if (auto value = configured(id)) return value;
            }
            const auto action = std::find_if(
                statusActions.begin(), statusActions.end(),
                [&](const StatusActionNode& item) {
                    return item.id.value() == id;
                });
            if (action == statusActions.end()) return std::nullopt;
            return ResolvedProvider{action->accessibleLabel,
                                    action->accessibleLabel,
                                    action->commandId};
        };
        auto uiState = resolveUiState(schema, resolver);
        uiState.focusPath = interaction.focusPath();
        sections.uiFrame = UiFrame::require(
            schema.schema(), std::move(uiState),
            buildPresenceSection(schema, interaction.presence()));
        for (std::size_t index = 0; index < tabs_.size(); ++index) {
            TabState tab;
            tab.id = TabId{index + 1};
            tab.label = tabs_[index].title;
            tab.dirty = tabs_[index].dirty;
            sections.tabs.tabs.push_back(std::move(tab));
            if (tabs_[index].active) {
                sections.tabs.active = TabId{index + 1};
            }
        }

        for (auto const& mutate : mutators_) mutate(sections);
        PaletteReport framePalette = palette_;
        if (promptInput_) {
            sections.palette.activePicker =
                PickerActivation{SearchMode::Command,
                                 PickerActivationId{1}};
            framePalette.query = promptInput_->query;
            framePalette.ghost = promptInput_->ghost;
        }

        ShellViewState shellView;
        if (columns_ >= style_.dimensions.minimumColumns &&
            rows_ >= style_.dimensions.minimumRows) {
            shellView.viewport = {columns_, rows_};
        }
        for (auto const& mutate : shellProjectionMutators_) mutate(shellView);
        ClientSnapshotState client{ClientId{1}, ViewId{1}, {}};
        auto frame = test::gridFrameFromLegacy(
            LegacyPresentationSnapshot{
                SessionSnapshot{
                    revision_, SessionTopology{}, std::move(client),
                    std::move(sections)},
                PresentationSnapshot{
                    std::move(viewportState), style_, std::nullopt,
                    std::move(shellView),
                    SelectionNavigation{firstRow_, 0, std::nullopt}}},
            std::move(framePalette));
        return std::move(*frame);
    }

private:
    struct PromptInput {
        std::string query;
        std::string ghost;
    };

    // A caret at `offset`, with the line/cell coordinates derived from the text
    // so the position is internally consistent rather than merely non-empty.
    [[nodiscard]] DocumentPosition caretPosition(std::size_t offset) const {
        std::uint32_t line = 0;
        std::size_t lineStart = 0;
        for (std::size_t i = 0; i < offset && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                lineStart = i + 1;
            }
        }
        auto const cells = GraphemeLayout{}
                               .computeRun(std::string_view{text_}.substr(
                                   lineStart, offset - lineStart))
                               .totalCells;
        return {ByteOffset{offset}, LineIndex{line}, CellIndex{cells}};
    }

    std::string text_;
    std::size_t caret_ = 0;
    int columns_ = 80;
    int rows_ = 24;
    std::uint32_t firstRow_ = 0;
    Revision revision_{1};
    bool panel_ = false;
    bool panelFocused_ = true;
    std::vector<TabSpec> tabs_;
    std::vector<std::function<void(SessionSnapshotSections&)>> mutators_;
    std::vector<std::function<void(ShellViewState&)>>
        shellProjectionMutators_;
    Style style_{};
    PaletteReport palette_;
    std::optional<ValidatedSchema> schema_;
    std::optional<PromptInput> promptInput_;
    bool noticePresent_ = false;
    bool externalModificationPresent_ = false;
    ChromeProviderResolver chromeProviderResolver_;
    StatusViewState status_;
};

}  // namespace ssg::test
