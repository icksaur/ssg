#include <ssg/Widget.h>

#include <ssg/GraphemeLayout.h>

#include <algorithm>
#include <numeric>

namespace ssg {

RowFit fitRow(const std::vector<FitItem>& items, int extent, int separator,
              Align align) {
    // Priority order for the collapse scan: stable-sort by rank so equal-rank
    // items keep their original relative order (matching addFields'
    // stable_sort). Indices, so the retained set can be restored to original
    // order afterwards.
    std::vector<std::size_t> byRank(items.size());
    std::iota(byRank.begin(), byRank.end(), std::size_t{0});
    std::ranges::stable_sort(
        byRank, [&](std::size_t a, std::size_t b) {
            return items[a].rank < items[b].rank;
        });

    // Forward scan: append while it fits, STOP at the first non-fit. Later,
    // lower-priority items are not reconsidered even if a smaller one would fit
    // -- this exact rule is what a "drop-until-fits" implementation would get
    // wrong (spec §Relative layout).
    std::vector<std::size_t> retained;
    int used = 0;
    for (std::size_t idx : byRank) {
        // A non-positive width is skipped, never retained and never ending the
        // scan -- matching addFields, which `continue`s past empty-value fields
        // before the fit loop so they cannot occupy a cell or a separator.
        if (items[idx].desired <= 0) continue;
        const int sep = retained.empty() ? 0 : separator;
        if (used + sep + items[idx].desired > extent) break;
        used += sep + items[idx].desired;
        retained.push_back(idx);
    }

    // Restore original order for placement.
    std::ranges::sort(retained);

    RowFit fit;
    fit.placed.reserve(retained.size());
    // Start packs from offset 0; End packs the whole retained run flush to the
    // trailing edge (`extent - used`).
    int offset = align == Align::End ? extent - used : 0;
    bool first = true;
    for (std::size_t idx : retained) {
        if (!first) offset += separator;
        first = false;
        fit.placed.push_back(
            {items[idx].id, offset, items[idx].desired, idx});
        offset += items[idx].desired;
    }
    return fit;
}

RowFit packEnd(const std::vector<FitItem>& items, int extent) {
    // Fill from the right: the last item is rightmost. Each is clamped to the
    // space still remaining (truncated, not dropped, when it partially fits);
    // an item with no room is dropped. No separators.
    int cursor = extent;
    std::vector<PlacedItem> reversed;
    for (std::size_t i = items.size(); i-- > 0;) {
        if (items[i].desired <= 0) continue;
        const int width = std::min(cursor, items[i].desired);
        if (width <= 0) continue;
        cursor -= width;
        reversed.push_back({items[i].id, cursor, width, i});
    }
    std::ranges::reverse(reversed);  // right-to-left -> original order
    return RowFit{std::move(reversed)};
}

std::vector<Rect> layoutRow(const RowFit& fit, const Rect& container) {
    std::vector<Rect> rects;
    rects.reserve(fit.placed.size());
    for (const auto& item : fit.placed) {
        rects.push_back(
            Rect{container.x + item.offset, container.y, item.size, 1});
    }
    return rects;
}

int measureFieldCells(std::string_view value) {
    return std::max(1, static_cast<int>(
                            GraphemeLayout{}.computeRun(value).totalCells) +
                            2);
}

std::string checkboxText(bool checked, std::string_view caption,
                         const ToggleGlyphs& toggle) {
    return (checked ? toggle.checked : toggle.unchecked) + std::string(caption);
}

std::string textInputText(std::string_view prefix, std::string_view separator,
                          std::string_view value) {
    std::string text;
    text.reserve(prefix.size() + separator.size() + value.size());
    text.append(prefix).append(separator).append(value);
    return text;
}

std::string visibleTail(std::string_view value, int cells) {
    if (cells <= 0) return {};
    auto const run = GraphemeLayout{}.computeRun(value);
    if (static_cast<int>(run.totalCells) <= cells) return std::string{value};
    // Walk backwards from the end, taking clusters while they fit.
    int used = 0;
    std::size_t begin = value.size();
    for (auto span = run.spans.rbegin(); span != run.spans.rend(); ++span) {
        auto const width =
            static_cast<int>(std::max<std::uint32_t>(span->cellWidth, 1));
        if (used + width > cells) break;
        used += width;
        begin = span->byteOffset;
    }
    return std::string{value.substr(begin)};
}

TextInputLayout layoutTextInput(std::string_view sigil, std::string_view value,
                                int available) {
    // One column is held back for the caret: text filling the field to its last
    // column would leave the terminal cursor nowhere to sit.
    const int drawable = std::max(0, available - 1);
    const int sigilCells =
        static_cast<int>(GraphemeLayout{}.computeRun(sigil).totalCells);
    // The value scrolls against the room AFTER the pinned sigil.
    const int textRoom = std::max(0, drawable - sigilCells);
    std::string text = textInputText(sigil, {}, visibleTail(value, textRoom));
    const int cells = static_cast<int>(GraphemeLayout{}.computeRun(text).totalCells);
    return {std::move(text), std::min(drawable, cells)};
}

}  // namespace ssg
