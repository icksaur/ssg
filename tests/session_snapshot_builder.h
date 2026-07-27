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
// from `computeShellLayout`, syntax from `plainTextSyntaxViewState`, and colour
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

    [[nodiscard]] SessionSnapshot build() const {
        auto const caret = caret_ > text_.size() ? text_.size() : caret_;

        ShellState shell{{"Files"}};
        if (panel_) shell.togglePanel();

        ShellLayoutRequest request;
        request.viewport = {columns_, rows_};
        request.emptyState = text_.empty();
        request.tabs = tabs_;
        for (auto const& mutate : shellMutators_) mutate(request);
        auto layout = computeShellLayout(request, shell);

        ViewportDimensions const dimensions{
            static_cast<std::uint32_t>(columns_),
            static_cast<std::uint32_t>(rows_)};
        auto viewportState = Viewport{}.computeUnwrapped(text_, dimensions,
                                                          firstRow_, 0, 4);

        SessionSnapshotSections sections{
            DocumentViewState{revision_, text_, ByteOffset{caret}, std::nullopt},
            SelectionViewState{
                SelectionSet{{Selection{caretPosition(caret),
                                        caretPosition(caret)}}},
                firstRow_, 0, std::nullopt},
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
            plainTextSyntaxViewState(revision_, LanguageId{"plain"}, text_, 4),
            LspSyncViewState{},
            LspFeatureViewState{},
            defaultTheme(),
            Style{},
            layout.view ? *layout.view : ShellViewState{},
            PaletteViewState{}};

        for (auto const& mutate : mutators_) mutate(sections);

        ClientSnapshotState client{ClientId{1}, ViewId{1}, {},
                                    std::move(viewportState)};
        return SessionSnapshot{revision_, SessionTopology{}, std::move(client),
                                std::move(sections)};
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
    std::vector<TabLabel> tabs_;
    std::vector<std::function<void(SessionSnapshotSections&)>> mutators_;
    std::vector<std::function<void(ShellLayoutRequest&)>> shellMutators_;
};

}  // namespace ssg::test
