#pragma once

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

enum class GridActionStatus : std::uint8_t {
    Applied,
    TransitionRequired,
    Rejected,
};

struct GridActionResult {
    GridActionStatus status = GridActionStatus::Rejected;
    std::optional<ClientInput> transition;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return status != GridActionStatus::Rejected;
    }
};

class GridFrame {
public:
    GridFrame(GridFrame const&) = delete;
    GridFrame& operator=(GridFrame const&) = delete;
    GridFrame(GridFrame&&) noexcept = default;
    GridFrame& operator=(GridFrame&&) noexcept = default;

    [[nodiscard]] GridBasis basis() const noexcept { return basis_; }
    [[nodiscard]] SessionSnapshot const& semantic() const noexcept {
        return semantic_;
    }
    [[nodiscard]] Revision revision() const noexcept {
        return semantic_.revision();
    }
    [[nodiscard]] SessionSnapshotSections const& sections() const noexcept {
        return semantic_.sections();
    }
    [[nodiscard]] PresentationSnapshot const* presentation() const noexcept {
        return semantic_.presentation() ? &*semantic_.presentation() : nullptr;
    }
    // Temporary adapter for presentation tests and the compatibility bridge.
    // Plan 6 removes this with SessionSnapshot::presentation().
    [[nodiscard]] static std::optional<GridFrame> fromDeprecatedSnapshot(
        SessionSnapshot snapshot);

private:
    friend class GridPresenter;
    GridFrame(SessionSnapshot semantic, GridBasis basis)
        : semantic_{std::move(semantic)}, basis_{basis} {}

    SessionSnapshot semantic_;
    GridBasis basis_;
};

class GridPresenter {
public:
    explicit GridPresenter(ViewId viewId);
    ~GridPresenter();

    GridPresenter(GridPresenter const&) = delete;
    GridPresenter& operator=(GridPresenter const&) = delete;
    GridPresenter(GridPresenter&&) noexcept;
    GridPresenter& operator=(GridPresenter&&) noexcept;

    [[nodiscard]] std::optional<GridFrame> project(
        EditorSession& session, ClientId client,
        GridPresentationRequest request);
    [[nodiscard]] GridActionResult apply(
        ViewActionRequest const& request, GridFrame const& frame);

private:
    ViewId viewId_;
    std::unique_ptr<detail::GridProjectionState> state_;
};

}  // namespace ssg
