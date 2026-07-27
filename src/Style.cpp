#include "ssg/Style.h"

#include <algorithm>

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

std::string_view Style::scrollbarCell(int row, int thumbStart, int thumbSize,
                                        int trackHeight) const {
    if (row < 0 || row >= trackHeight) return scrollbar.gutter;
    if (thumbSize <= 0 || trackHeight <= 0) return scrollbar.gutter;

    // A thumb that claims more rows than the track has would otherwise index
    // past the bottom cap.  Clamp instead of trusting the caller's metrics.
    thumbSize = std::min(thumbSize, trackHeight);
    thumbStart = std::clamp(thumbStart, 0, trackHeight - thumbSize);

    int const offset = row - thumbStart;
    if (offset < 0 || offset >= thumbSize) return scrollbar.track;

    if (thumbSize == 1) return scrollbar.single;
    if (offset == 0) return scrollbar.top;
    if (offset == thumbSize - 1) return scrollbar.bottom;
    return scrollbar.body;
}

}  // namespace ssg
