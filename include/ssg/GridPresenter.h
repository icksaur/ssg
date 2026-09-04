#pragma once

#include <ssg/EditorSession.h>
#include <ssg/ViewActionResult.h>
#include <ssg/Layout.h>
#include <ssg/UiRegionProjection.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace ssg {

struct GridPresentationRequest {
    ViewportDimensions dimensions;
    PaletteReport palette;
};

struct GridPresentation {
    std::uint64_t presentationGeneration = 0;
    ViewportViewState viewport;
    Style style;
    SolvedGridTree layout;
    PaletteReport palette;
    std::optional<SolvedUiRegion> header;
    std::optional<SolvedUiRegion> footer;
    std::optional<SolvedPanelSurface> panel;
    std::optional<SolvedDocumentSurface> document;

    std::string documentText;
    std::uint64_t documentRevision = 0;
    std::optional<std::string> diffFileIdentity;
    SelectionSet selections;
    FindReplaceViewState findReplace;
    DiffViewState diff;
    LspSyncViewState lspSync;
    SyntaxViewState syntax;
    ThemeSnapshot theme;
    UiSchema uiTree;
    TabViewState tabs;
    std::optional<NoticeView> notice;
    ExternalModificationViewState externalModification;
    FollowMode followMode = FollowMode::Following;
    PromptStatusViewState promptStatus;
    PaletteViewState paletteView;
    std::optional<ClipboardWrite> clipboardWrite;
    bool wordWrap = false;
};

class GridPresenter final {
public:
    GridPresenter();
    ~GridPresenter();

    GridPresenter(GridPresenter const&) = delete;
    GridPresenter& operator=(GridPresenter const&) = delete;
    GridPresenter(GridPresenter&&) noexcept;
    GridPresenter& operator=(GridPresenter&&) noexcept;

    [[nodiscard]] std::optional<GridPresentation> project(
        EditorSession& session, GridPresentationRequest request);
    [[nodiscard]] ViewActionResult apply(
        ViewAction const& request, GridPresentation const& presentation);

private:
    struct State;

    [[nodiscard]] std::optional<GridPresentation> project(
        EditorSession::Impl& runtime, GridPresentationRequest request);

    std::unique_ptr<State> state_;
};

}  // namespace ssg
