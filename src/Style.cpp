#include "ssg/Style.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <optional>

#include "ssg/GraphemeLayout.h"

namespace ssg {
namespace {

// Every glyph field style.define can set, keyed by the same name the wire codec
// uses.  One table, so a new glyph is added in exactly one place here.
//
// An ACCESSOR rather than a setter: assigning through it sets the field, and
// reading it from a default-constructed Style gives the field's default -- which
// is what says how wide the glyph is allowed to be.  A separate width table
// would be a second thing to keep in step.
std::unordered_map<std::string, std::function<std::string&(Style&)>> const&
glyphSetters() {
    static auto const table = [] {
        std::unordered_map<std::string, std::function<std::string&(Style&)>>
            map;
        map["scrollbar_gutter"] = [](Style& s) -> std::string& { return s.scrollbar.gutter; };
        map["scrollbar_track"] = [](Style& s) -> std::string& { return s.scrollbar.track; };
        map["scrollbar_single"] = [](Style& s) -> std::string& { return s.scrollbar.single; };
        map["scrollbar_top"] = [](Style& s) -> std::string& { return s.scrollbar.top; };
        map["scrollbar_body"] = [](Style& s) -> std::string& { return s.scrollbar.body; };
        map["scrollbar_bottom"] = [](Style& s) -> std::string& { return s.scrollbar.bottom; };
        map["tree_expanded"] = [](Style& s) -> std::string& { return s.tree.expanded; };
        map["tree_collapsed"] = [](Style& s) -> std::string& { return s.tree.collapsed; };
        map["tab_dirty_suffix"] = [](Style& s) -> std::string& { return s.tab.dirtySuffix; };
        map["tab_live_diff_prefix"] = [](Style& s) -> std::string& { return s.tab.liveDiffPrefix; };
        map["toggle_checked"] = [](Style& s) -> std::string& { return s.toggle.checked; };
        map["toggle_unchecked"] = [](Style& s) -> std::string& { return s.toggle.unchecked; };
        map["truncation"] = [](Style& s) -> std::string& { return s.truncation; };
        map["input_line_sigil"] = [](Style& s) -> std::string& { return s.inputLineSigil; };
        map["unrenderable"] = [](Style& s) -> std::string& { return s.unrenderable; };
        map["prompt_label_separator"] = [](Style& s) -> std::string& { return s.promptLabelSeparator; };
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

namespace {

// Why a glyph is unacceptable, or nullopt if it is fine.
//
// Glyph strings are the one input that reaches a terminal cell WITHOUT passing
// through GraphemeLayout on the way. Document text and file names are already
// classified, so a control byte in either draws as a replacement glyph; a glyph
// was copied verbatim, so scrollbar_track = "<ESC>(0" switched the terminal's
// character set and every later byte drew as line art
// (doc/spec-terminal-escape-discipline.md).
//
// Asked of the same layout engine everything else uses, so "safe to emit" and
// "how wide is it" have one answer in this codebase rather than two.
std::optional<std::string> rejectGlyph(std::string const& key,
                                       std::string const& value,
                                       std::string const& defaultValue) {
    if (value.find('\n') != std::string::npos ||
        value.find('\r') != std::string::npos) {
        return "style.define value for '" + key +
               "' must not contain a line break";
    }
    GraphemeLayout const layout;
    auto const run = layout.computeRun(value);
    for (auto const& span : run.spans) {
        if (span.kind == CellKind::Control) {
            return "style.define value for '" + key +
                   "' must not contain a control character: it would change "
                   "how the terminal interprets what follows";
        }
        if (span.kind == CellKind::InvalidUtf8) {
            return "style.define value for '" + key + "' must be valid UTF-8";
        }
    }
    // A glyph fills a fixed slot, so it must be exactly as wide as the one it
    // replaces. Otherwise the columns the server described and the columns the
    // terminal draws disagree and the row shifts. The field's DEFAULT is the
    // width, so a newly added glyph brings its own rule with it.
    auto const width = run.totalCells;
    auto const expected = layout.computeRun(defaultValue).totalCells;
    if (width != expected) {
        return "style.define value for '" + key + "' must be " +
               std::to_string(expected) + " column(s) wide, but '" + value +
               "' is " + std::to_string(width);
    }
    return std::nullopt;
}

}  // namespace

StyleDefineResult applyStyleDefine(Style const& current,
                                   StyleDefineArguments const& arguments) {
    Style next = current;
    // The widths come from a default-constructed Style, so a glyph's allowed
    // width is a property of the field and not of whatever was set before it.
    Style defaults{};
    for (auto const& [key, value] : arguments.values) {
        if (auto const it = glyphSetters().find(key);
            it != glyphSetters().end()) {
            if (auto rejection = rejectGlyph(key, value, it->second(defaults))) {
                return {StyleDefineError{std::move(*rejection)}, {}};
            }
            it->second(next) = value;
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

std::vector<std::string> styleDefineKeys() {
    std::vector<std::string> keys;
    keys.reserve(glyphSetters().size() + dimensionSetters().size());
    for (auto const& [key, _] : glyphSetters()) keys.push_back(key);
    for (auto const& [key, _] : dimensionSetters()) keys.push_back(key);
    return keys;
}

}  // namespace ssg
