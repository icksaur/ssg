#pragma once

#include <ssg/PaletteSearcher.h>
#include <ssg/Selection.h>
#include <ssg/Style.h>
#include <ssg/Viewport.h>
#include <ssg/session_snapshot.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace ssg {

class EditorSession;

struct PresentationCapture {
    SessionSnapshot semantic;
    Style style;
};

struct ViewportProjectionRequest {
    ClientId clientId;
    ViewId viewId;
    Revision semanticRevision;
    ViewportDimensions dimensions;
    std::uint32_t paneContentRows;
    std::uint32_t paneContentColumns;
    SelectionNavigation proposedNavigation;
    bool revealPrimarySelection;
};

struct ViewportProjectionResult {
    ViewportViewState viewport;
    SelectionNavigation navigation;
};

class ViewportProjectionState {
public:
    struct Impl;

    ViewportProjectionState();
    ~ViewportProjectionState();

    ViewportProjectionState(const ViewportProjectionState&) = delete;
    ViewportProjectionState& operator=(const ViewportProjectionState&) = delete;
    ViewportProjectionState(ViewportProjectionState&&) noexcept;
    ViewportProjectionState& operator=(ViewportProjectionState&&) noexcept;

private:
    std::unique_ptr<Impl> impl_;
    friend class EditorSession;
};

}  // namespace ssg
