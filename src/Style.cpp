#include "ssg/Style.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>

#include "ssg/GraphemeLayout.h"

namespace ssg {
namespace {

// Every glyph field style.define can set, keyed by the same name the wire codec
// uses.  One table, so a new glyph is added in exactly one place here.
std::unordered_map<std::string, std::function<void(Style&, std::string)>> const&
glyphSetters() {
    static auto const table = [] {
        std::unordered_map<std::string, std::function<void(Style&, std::string)>>
            map;
        map["scrollbar_gutter"] = [](Style& s, std::string v) { s.scrollbar.gutter = std::move(v); };
        map["scrollbar_track"] = [](Style& s, std::string v) { s.scrollbar.track = std::move(v); };
        map["scrollbar_single"] = [](Style& s, std::string v) { s.scrollbar.single = std::move(v); };
        map["scrollbar_top"] = [](Style& s, std::string v) { s.scrollbar.top = std::move(v); };
        map["scrollbar_body"] = [](Style& s, std::string v) { s.scrollbar.body = std::move(v); };
        map["scrollbar_bottom"] = [](Style& s, std::string v) { s.scrollbar.bottom = std::move(v); };
        map["tree_expanded"] = [](Style& s, std::string v) { s.tree.expanded = std::move(v); };
        map["tree_collapsed"] = [](Style& s, std::string v) { s.tree.collapsed = std::move(v); };
        map["tab_dirty_suffix"] = [](Style& s, std::string v) { s.tab.dirtySuffix = std::move(v); };
        map["tab_live_diff_prefix"] = [](Style& s, std::string v) { s.tab.liveDiffPrefix = std::move(v); };
        map["toggle_checked"] = [](Style& s, std::string v) { s.toggle.checked = std::move(v); };
        map["toggle_unchecked"] = [](Style& s, std::string v) { s.toggle.unchecked = std::move(v); };
        map["truncation"] = [](Style& s, std::string v) { s.truncation = std::move(v); };
        map["input_line_sigil"] = [](Style& s, std::string v) { s.inputLineSigil = std::move(v); };
        map["unrenderable"] = [](Style& s, std::string v) { s.unrenderable = std::move(v); };
        map["prompt_label_separator"] = [](Style& s, std::string v) { s.promptLabelSeparator = std::move(v); };
        return map;
    }();
    return table;
}

std::unordered_map<std::string, std::function<void(Style&, int)>> const&
dimensionSetters() {
    static auto const table = [] {
        std::unordered_map<std::string, std::function<void(Style&, int)>> map;
        map["tree_indent"] = [](Style& s, int v) { s.tree.indentPerDepth = v; };
        map["dim_minimum_columns"] = [](Style& s, int v) { s.dimensions.minimumColumns = v; };
        map["dim_minimum_rows"] = [](Style& s, int v) { s.dimensions.minimumRows = v; };
        map["dim_panel_target_width"] = [](Style& s, int v) { s.dimensions.panelTargetWidth = v; };
        map["dim_panel_minimum_width"] = [](Style& s, int v) { s.dimensions.panelMinimumWidth = v; };
        map["dim_editor_minimum_width"] = [](Style& s, int v) { s.dimensions.editorMinimumWidth = v; };
        map["dim_scrollbar_gutter_width"] = [](Style& s, int v) { s.dimensions.scrollbarGutterWidth = v; };
        map["dim_header_height"] = [](Style& s, int v) { s.dimensions.headerHeight = v; };
        map["dim_tab_bar_height"] = [](Style& s, int v) { s.dimensions.tabBarHeight = v; };
        map["dim_footer_height"] = [](Style& s, int v) { s.dimensions.footerHeight = v; };
        map["dim_label_padding"] = [](Style& s, int v) { s.dimensions.labelPadding = v; };
        map["dim_input_line_separator"] = [](Style& s, int v) { s.dimensions.inputLineSeparator = v; };
        map["dim_input_line_query_budget"] = [](Style& s, int v) { s.dimensions.inputLineQueryBudget = v; };
        return map;
    }();
    return table;
}

}  // namespace

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

StyleDefineResult applyStyleDefine(Style const& current,
                                   StyleDefineArguments const& arguments) {
    Style next = current;
    for (auto const& [key, value] : arguments.values) {
        if (auto const it = glyphSetters().find(key);
            it != glyphSetters().end()) {
            it->second(next, value);
            continue;
        }
        if (auto const it = dimensionSetters().find(key);
            it != dimensionSetters().end()) {
            int parsed = 0;
            auto const* begin = value.data();
            auto const* end = value.data() + value.size();
            auto const [ptr, ec] = std::from_chars(begin, end, parsed);
            if (ec != std::errc{} || ptr != end) {
                return {StyleDefineError{"style.define value for '" + key +
                                         "' must be an integer: '" + value + "'"},
                        {}};
            }
            if (parsed < 0) {
                return {StyleDefineError{"style.define value for '" + key +
                                         "' must not be negative: '" + value +
                                         "'"},
                        {}};
            }
            it->second(next, parsed);
            continue;
        }
        return {StyleDefineError{"unknown style.define key: '" + key + "'"}, {}};
    }
    return {std::nullopt, std::move(next)};
}

}  // namespace ssg
