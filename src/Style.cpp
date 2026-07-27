#include "ssg/Style.h"

#include <algorithm>
#include <cstdint>

#include "ssg/GraphemeLayout.h"

namespace ssg {

int Style::sigilWidth() const {
    GraphemeLayout layout;
    return static_cast<int>(layout.computeRun(inputLineSigil).totalCells);
}

int Style::inputLineReservation() const {
    return dimensions.inputLineSeparator + sigilWidth() +
           dimensions.inputLineQueryBudget;
}

ScrollbarCell Style::scrollbarCell(int row, int thumbStart, int thumbSize,
                                     int trackHeight) const {
    auto const gutter =
        ScrollbarCell{scrollbar.gutter, ScrollbarCellKind::Gutter};
    if (row < 0 || row >= trackHeight) return gutter;
    if (thumbSize <= 0 || trackHeight <= 0) return gutter;

    // A thumb that claims more rows than the track has would otherwise index
    // past the bottom cap.  Clamp instead of trusting the caller's metrics.
    thumbSize = std::min(thumbSize, trackHeight);
    thumbStart = std::clamp(thumbStart, 0, trackHeight - thumbSize);

    int const offset = row - thumbStart;
    if (offset < 0 || offset >= thumbSize) {
        return {scrollbar.track, ScrollbarCellKind::Track};
    }

    auto const thumb = [](std::string const& glyph) {
        return ScrollbarCell{glyph, ScrollbarCellKind::Thumb};
    };
    if (thumbSize == 1) return thumb(scrollbar.single);
    if (offset == 0) return thumb(scrollbar.top);
    if (offset == thumbSize - 1) return thumb(scrollbar.bottom);
    return thumb(scrollbar.body);
}

}  // namespace ssg
