#include <ssg/Widget.h>

#include <ssg/GraphemeLayout.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ssg {

namespace {
constexpr std::array kWidgetKindNames{
    std::string_view{"container"}, std::string_view{"label"},
    std::string_view{"field"},     std::string_view{"checkbox"},
    std::string_view{"text_input"}, std::string_view{"spacer"},
    std::string_view{"view"},      std::string_view{"status_actions"},
};
static_assert(kWidgetKindNames.size() == kWidgetKindCount);

constexpr std::array kViewSurfaceNames{
    std::string_view{"tabview"}, std::string_view{"filetree"},
    std::string_view{"gitstatus"}, std::string_view{"findresults"},
    std::string_view{"symbols"}, std::string_view{"footer_prompt"},
    std::string_view{"notice"}, std::string_view{"external_modification"},
};
static_assert(kViewSurfaceNames.size() == kViewSurfaceCount);
}  // namespace

std::string_view widgetKindName(WidgetKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= kWidgetKindCount) {
        throw std::invalid_argument("widgetKindName: unrecognized WidgetKind");
    }
    return kWidgetKindNames[index];
}

std::string_view viewSurfaceName(ViewSurface surface) {
    const auto index = static_cast<std::size_t>(surface);
    if (index >= kViewSurfaceCount) {
        throw std::invalid_argument("viewSurfaceName: unrecognized ViewSurface");
    }
    return kViewSurfaceNames[index];
}

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

InputLineLayout layoutInputLine(std::string_view sigil, std::string_view query,
                                std::string_view ghost, int available) {
    const auto input = layoutTextInput(sigil, query, available);
    InputLineLayout line{input.text, input.width, {}, 0};
    // The ghost fills the cells the query left, clamped to its own display width;
    // it is dropped when the query consumed the row. The renderer clips the whole
    // ghost string to this width (the text is not truncated here).
    const int remaining = available - input.width;
    if (!ghost.empty() && remaining > 0) {
        const int ghostCells =
            static_cast<int>(GraphemeLayout{}.computeRun(ghost).totalCells);
        line.ghostWidth = std::min(remaining, ghostCells);
        line.ghostText = std::string{ghost};
    }
    return line;
}

namespace {

// Resolve one item's CONTENT fit within `granted` cells: the display text and
// the size the placement actually occupies. `None`/`Truncate` keep the full text
// at the granted width (the renderer clips a `Truncate` item to its rect);
// `ScrollTail` composes the sigil + value tail via `layoutTextInput`, whose
// width (caret-reserved, <= granted) becomes the placement size.
std::pair<std::string, int> resolveContent(const StackItem& item, int granted) {
    if (item.overflow == Overflow::ScrollTail) {
        const auto laid = layoutTextInput(item.sigil, item.content, granted);
        return {laid.text, laid.width};
    }
    return {item.content, granted};
}

}  // namespace

WidgetStack& WidgetStack::packLeft(StackItem item) {
    left_.push_back(std::move(item));
    return *this;
}

WidgetStack& WidgetStack::packRight(StackItem item) {
    right_.push_back(std::move(item));
    return *this;
}

WidgetStack& WidgetStack::center(StackItem item, CenterWidth width, int fixed) {
    if (center_) {
        centerConflict_ = true;  // a second center is a fail-loud authoring error
        return *this;
    }
    center_ = std::move(item);
    centerWidth_ = width;
    centerFixed_ = fixed;
    return *this;
}

std::optional<StackLayout> WidgetStack::resolve(int extent) const {
    if (centerConflict_) return std::nullopt;

    StackLayout layout;

    // --- Right group: fill from the trailing edge leftward via `packEnd` (the
    // single source of that rule: rightmost item full width, leftmost with room
    // clamp-truncated, zero-room dropped). A right item owns its full reserved
    // slot; overflow only decides the TEXT (a `ScrollTail` value tail is anchored
    // within the slot, never shrinking it, so the reserved region has no hole).
    std::vector<FitItem> rightItems;
    rightItems.reserve(right_.size());
    for (const auto& item : right_)
        rightItems.push_back({item.id, item.desired, 0});
    const RowFit rightFit = packEnd(rightItems, extent);
    std::vector<StackPlacement> rightPlaced;
    rightPlaced.reserve(rightFit.placed.size());
    for (const auto& p : rightFit.placed) {
        auto [text, size] = resolveContent(right_[p.index], p.size);
        rightPlaced.push_back({right_[p.index].id, p.offset, p.size,
                               std::move(text)});
    }
    const int rightStart =
        rightFit.placed.empty() ? extent : rightFit.placed.front().offset;

    // --- Left group: `keep` items are always retained and pre-consume budget;
    // the rest collapse by rank via `fitRow` over the remaining width. Placements
    // are emitted in original order with `separator` between neighbors.
    int keepFootprint = 0;
    int keepCount = 0;
    std::vector<FitItem> collapse;
    std::vector<std::size_t> collapseSource;
    for (std::size_t i = 0; i < left_.size(); ++i) {
        if (left_[i].keep) {
            keepFootprint += left_[i].desired;
            ++keepCount;
        } else {
            collapse.push_back({left_[i].id, left_[i].desired, left_[i].rank});
            collapseSource.push_back(i);
        }
    }
    if (keepCount > 1) keepFootprint += separator_ * (keepCount - 1);
    const int joinSep = keepCount > 0 ? separator_ : 0;
    const int collapseExtent = rightStart - keepFootprint - joinSep;
    const RowFit collapseFit = fitRow(collapse, collapseExtent, separator_,
                                      Align::Start);
    std::vector<bool> retained(left_.size(), false);
    for (std::size_t i = 0; i < left_.size(); ++i)
        if (left_[i].keep) retained[i] = true;
    for (const auto& placed : collapseFit.placed)
        retained[collapseSource[placed.index]] = true;

    // Emit retained left items in original order. Each is clamped to the room
    // remaining before `rightStart`, so an over-budget `keep` item (which is
    // never dropped) TRUNCATES rather than overlapping the right group -- the
    // non-overlap invariant holds without a fail-loud path. Retained collapse
    // items fit by construction, so the clamp is a no-op for them.
    int leftOffset = 0;
    bool leftFirst = true;
    for (std::size_t i = 0; i < left_.size(); ++i) {
        if (!retained[i]) continue;
        const int start = leftOffset + (leftFirst ? 0 : separator_);
        const int avail = rightStart - start;
        if (avail <= 0) break;  // no room before the right group; stop placing
        leftFirst = false;
        const int granted = std::min(left_[i].desired, avail);
        auto [text, size] = resolveContent(left_[i], granted);
        layout.placed.push_back({left_[i].id, start, size, std::move(text)});
        leftOffset = start + granted;
    }
    const int leftEnd = leftOffset;

    // --- Center: the gap between the left group's end and the right group's
    // start. `Fixed` clamps to that gap; `Flex` takes all of it.
    if (center_) {
        const int gap = std::max(0, rightStart - leftEnd);
        const int width =
            centerWidth_ == CenterWidth::Fixed ? std::min(centerFixed_, gap) : gap;
        if (width > 0) {
            auto [text, size] = resolveContent(*center_, width);
            layout.placed.push_back({center_->id, leftEnd, size, std::move(text)});
        }
    }

    for (auto& placement : rightPlaced)
        layout.placed.push_back(std::move(placement));

    return layout;
}

}  // namespace ssg
