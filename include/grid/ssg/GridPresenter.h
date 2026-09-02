#pragma once

#include <ssg/UiRegionProjection.h>
#include <ssg/ViewActionResult.h>
#include <ssg/Layout.h>
#include <ssg/PaletteSearcher.h>
#include <ssg/Viewport.h>
#include <ssg/ClientInput.h>
#include <ssg/session_snapshot.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace ssg {

class EditorSession;
namespace detail {
struct GridProjectionState;
}

struct GridBasis {
    ViewId viewId;
    Revision semanticRevision;
    // Advances whenever a frame is projected or an accepted action consumes the
    // current basis. Rejection leaves it unchanged.
    std::uint64_t presentationGeneration = 0;

    friend bool operator==(const GridBasis&, const GridBasis&) = default;
};

struct GridPresentationRequest {
    ViewportDimensions dimensions;
    PaletteReport palette;
};

struct GridProjection {
    ViewportViewState viewport;
    Style style;
    SelectionNavigation selectionNav;
};

class GridFrame {
public:
    GridFrame(GridFrame const&) = delete;
    GridFrame& operator=(GridFrame const&) = delete;
    GridFrame(GridFrame&&) noexcept = default;
    GridFrame& operator=(GridFrame&&) noexcept = default;

    [[nodiscard]] GridBasis basis() const noexcept { return basis_; }
    [[nodiscard]] GridProjection const& presentation() const noexcept {
        return projection_;
    }
    [[nodiscard]] SolvedGridTree const& layout() const noexcept {
        return layout_;
    }
    [[nodiscard]] PaletteReport const& palette() const noexcept {
        return palette_;
    }
    [[nodiscard]] std::optional<SolvedUiRegion> const& header() const noexcept {
        return header_;
    }
    [[nodiscard]] std::optional<SolvedUiRegion> const& footer() const noexcept {
        return footer_;
    }
    [[nodiscard]] std::optional<SolvedPanelSurface> const& panel() const noexcept {
        return panel_;
    }
    [[nodiscard]] std::optional<SolvedDocumentSurface> const& document() const noexcept {
        return document_;
    }
    // CONTRACT: Direct value construction requires a corresponding, solvable
    // semantic UI frame and throws std::logic_error otherwise. GridPresenter
    // reports the same rejection through project()'s nullopt result.
    GridFrame(SessionSnapshot const& semantic, GridProjection projection,
              GridBasis basis, PaletteReport palette = {});

private:
    friend class GridPresenter;
    GridFrame(GridProjection projection, SolvedGridTree layout, GridBasis basis,
              PaletteReport palette);
    [[nodiscard]] static std::optional<GridFrame> fromSemantic(
        SessionSnapshot const& semantic, Style style,
        ViewportDimensions dimensions,
        GridBasis basis, PaletteReport palette,
        std::uint32_t treeFirstVisible, bool revealTreeSelection,
        SelectionNavigation navigation);
    void finalizeViewport(ViewportViewState viewport, SelectionNavigation navigation);
    void solvePanel(SessionSnapshot const& semantic,
                    std::uint32_t treeFirstVisible, bool revealTreeSelection);
    void solveDocument(SessionSnapshot const& semantic);
    [[nodiscard]] std::optional<std::string> solveUiRegions(
        SessionSnapshot const& semantic);

    GridProjection projection_;
    SolvedGridTree layout_;
    PaletteReport palette_;
    std::optional<SolvedUiRegion> header_;
    std::optional<SolvedUiRegion> footer_;
    std::optional<SolvedPanelSurface> panel_;
    std::optional<SolvedDocumentSurface> document_;
    GridBasis basis_;
};

// A semantic snapshot and the grid presentation derived from that exact
// revision. Consumers must retain this pair rather than separately pairing a
// grid result with a snapshot.
class GridPresentation {
public:
    GridPresentation(SessionSnapshot semantic, GridFrame frame);

    GridPresentation(GridPresentation const&) = delete;
    GridPresentation& operator=(GridPresentation const&) = delete;
    GridPresentation(GridPresentation&&) noexcept = default;
    GridPresentation& operator=(GridPresentation&&) noexcept = default;

    [[nodiscard]] SessionSnapshot const& semantic() const noexcept {
        return semantic_;
    }
    [[nodiscard]] SessionSnapshotSections const& sections() const noexcept {
        return semantic_.sections();
    }
    [[nodiscard]] Revision revision() const noexcept { return semantic_.revision(); }
    [[nodiscard]] GridFrame const& frame() const noexcept { return frame_; }
    [[nodiscard]] GridBasis basis() const noexcept { return frame_.basis(); }
    [[nodiscard]] GridProjection const& presentation() const noexcept {
        return frame_.presentation();
    }
    [[nodiscard]] SolvedGridTree const& layout() const noexcept {
        return frame_.layout();
    }
    [[nodiscard]] PaletteReport const& palette() const noexcept {
        return frame_.palette();
    }
    [[nodiscard]] std::optional<SolvedUiRegion> const& header() const noexcept {
        return frame_.header();
    }
    [[nodiscard]] std::optional<SolvedUiRegion> const& footer() const noexcept {
        return frame_.footer();
    }
    [[nodiscard]] std::optional<SolvedPanelSurface> const& panel() const noexcept {
        return frame_.panel();
    }
    [[nodiscard]] std::optional<SolvedDocumentSurface> const& document() const noexcept {
        return frame_.document();
    }

private:
    SessionSnapshot semantic_;
    GridFrame frame_;
};

class GridPresenter {
public:
    explicit GridPresenter(ViewId viewId);
    ~GridPresenter();

    GridPresenter(GridPresenter const&) = delete;
    GridPresenter& operator=(GridPresenter const&) = delete;
    GridPresenter(GridPresenter&&) noexcept;
    GridPresenter& operator=(GridPresenter&&) noexcept;

    [[nodiscard]] std::optional<GridPresentation> project(
        EditorSession& session, ClientId client,
        GridPresentationRequest request);
    [[nodiscard]] ViewActionResult apply(
        ViewActionRequest const& request, GridPresentation const& presentation);

private:
    ViewId viewId_;
    std::unique_ptr<detail::GridProjectionState> state_;
};

}  // namespace ssg
