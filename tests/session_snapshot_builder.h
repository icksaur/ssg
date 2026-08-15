#pragma once

// Kind: seam.
//
// Builds a SessionSnapshot directly, so a test of presentation does not have to
// stand up an EditorRuntime over a real directory first.
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
// from `computeShellLayout`, syntax from `SyntaxViewState::plainText`, and colour
// from `defaultTheme`.  A test using it therefore exercises the same projection
// the runtime does.  Anything it cannot express is set through `sections()`.
//
// It is deliberately test-only: no production caller needs it, so it does not
// enlarge the public library surface.

#include <ssg/GraphemeLayout.h>
#include <ssg/Renderer.h>
#include <ssg/ShellState.h>
#include <ssg/SyntaxModel.h>
#include <ssg/Theme.h>
#include <ssg/Viewport.h>
#include <ssg/session_snapshot.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ssg::test {

class SessionSnapshotBuilder {
public:
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

    SessionSnapshotBuilder& tabs(std::vector<TabLabel> labels) {
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

    // Mutate the shell layout request before layout runs (input line, fields).
    SessionSnapshotBuilder& shellRequest(
        std::function<void(ShellLayoutRequest&)> mutate) {
        shellMutators_.push_back(std::move(mutate));
        return *this;
    }

    // Override the grid-presentation Style (a projection field, not a semantic
    // section). Applied when the PresentationSnapshot is built.
    SessionSnapshotBuilder& style(Style style) {
        style_ = std::move(style);
        return *this;
    }

    [[nodiscard]] SessionSnapshot build() const {
        auto const caret = caret_ > text_.size() ? text_.size() : caret_;

        // ShellState now owns only panes + distraction-free; panel presence and focus are
        // authority-owned in production and come from the request/sections here, both
        // derived from the SAME builder-owned values so the snapshot is coherent.
        ShellState shell{{"Files"}};
        const FocusTarget focus = (panel_ && panelFocused_) ? FocusTarget::Panel
                                                            : FocusTarget::Editor;

        ShellLayoutRequest request;
        request.viewport = {columns_, rows_};
        request.emptyState = text_.empty();
        request.tabs = tabs_;
        request.panelPresent = panel_;
        request.focus = focus;
        // Caller mutators run LAST so a test can override any request field.
        for (auto const& mutate : shellMutators_) mutate(request);
        auto layout = computeShellLayout(request, shell);

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
            PromptStatusViewState{},
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

        for (auto const& mutate : mutators_) mutate(sections);

        ShellViewState shellView = layout.view ? *layout.view : ShellViewState{};
        ClientSnapshotState client{ClientId{1}, ViewId{1}, {}};
        return SessionSnapshot{revision_, SessionTopology{}, std::move(client),
                                std::move(sections),
                                PresentationSnapshot{std::move(viewportState),
                                                     style_, std::nullopt,
                                                     std::move(shellView),
                                                     SelectionNavigation{
                                                         firstRow_, 0,
                                                         std::nullopt}}};
    }

private:
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
    std::vector<TabLabel> tabs_;
    std::vector<std::function<void(SessionSnapshotSections&)>> mutators_;
    std::vector<std::function<void(ShellLayoutRequest&)>> shellMutators_;
    Style style_{};
};

}  // namespace ssg::test
