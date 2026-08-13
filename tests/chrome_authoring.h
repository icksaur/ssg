#pragma once

// Test authoring for composed chrome. Tests describe a header/footer as widget
// groups; this helper encodes them into the `ssg.chrome` value tree and runs the
// REAL decoder, returning the canonical UiComposition. Authoring through the
// decoder (rather than hand-building region trees) is deliberate: it keeps the
// decoder and the lowerer from drifting together undetected -- a lowering golden
// built this way exercises the same path a Lua script does.

#include "ssg/ChromeDecode.h"
#include "ssg/UiTree.h"
#include "ssg/UiWidget.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace ssgtest {

inline ssg::ChromeValue widgetValue(const ssg::WidgetDescriptor& w) {
    using ssg::ChromeValue;
    std::vector<std::pair<std::string, ChromeValue>> t;
    const char* kindName = "label";
    switch (w.kind) {
    case ssg::WidgetKind::Label: kindName = "label"; break;
    case ssg::WidgetKind::Field: kindName = "field"; break;
    case ssg::WidgetKind::Checkbox: kindName = "checkbox"; break;
    case ssg::WidgetKind::TextInput: kindName = "text_input"; break;
    case ssg::WidgetKind::Spacer: kindName = "spacer"; break;
    case ssg::WidgetKind::Container: kindName = "container"; break;
    }
    t.emplace_back("kind", ChromeValue::ofString(kindName));
    if (!w.id.empty()) t.emplace_back("id", ChromeValue::ofString(w.id));
    if (w.value) {
        if (w.value->isProvider)
            t.emplace_back("provider", ChromeValue::ofString(w.value->provider));
        else
            t.emplace_back("text", ChromeValue::ofString(w.value->literal));
    }
    if (w.checked) {
        if (w.checked->isProvider)
            t.emplace_back("checked_provider",
                           ChromeValue::ofString(w.checked->provider));
        else
            t.emplace_back("checked",
                           ChromeValue::ofBool(w.checked->literal == "true"));
    }
    if (w.width) t.emplace_back("width", ChromeValue::ofInt(*w.width));
    if (w.role) t.emplace_back("role", ChromeValue::ofString(*w.role));
    if (w.command) t.emplace_back("command", ChromeValue::ofString(*w.command));
    if (w.rank != 0) t.emplace_back("rank", ChromeValue::ofInt(w.rank));
    if (w.keep) t.emplace_back("keep", ChromeValue::ofBool(true));
    if (w.overflow != ssg::Overflow::None) {
        t.emplace_back("overflow",
                       ChromeValue::ofString(w.overflow == ssg::Overflow::Truncate
                                                 ? "truncate"
                                                 : "scroll_tail"));
    }
    if (!w.sigil.empty()) t.emplace_back("sigil", ChromeValue::ofString(w.sigil));
    return ChromeValue::ofTable(std::move(t));
}

// Every provider id any widget in the row references, so the decoder (which
// rejects an unknown provider) accepts the authored row without the test having
// to restate the provider set.
inline void collectProviders(const std::vector<ssg::WidgetDescriptor>& widgets,
                             std::vector<std::string>& out) {
    for (const auto& w : widgets) {
        if (w.value && w.value->isProvider) out.push_back(w.value->provider);
        if (w.checked && w.checked->isProvider)
            out.push_back(w.checked->provider);
    }
}

inline ssg::ChromeValue rowTable(
    const std::vector<ssg::WidgetDescriptor>& left,
    const std::vector<ssg::WidgetDescriptor>& right,
    const std::optional<ssg::WidgetDescriptor>& center,
    ssg::CenterWidth centerWidth, int centerFixed, int separator) {
    using ssg::ChromeValue;
    std::vector<std::pair<std::string, ChromeValue>> region;
    const auto arr = [](const std::vector<ssg::WidgetDescriptor>& ws) {
        std::vector<ChromeValue> items;
        for (const auto& w : ws) items.push_back(widgetValue(w));
        return ChromeValue::ofArray(std::move(items));
    };
    if (!left.empty()) region.emplace_back("left", arr(left));
    if (!right.empty()) region.emplace_back("right", arr(right));
    if (center) {
        ssg::ChromeValue centerValue = widgetValue(*center);
        if (centerWidth == ssg::CenterWidth::Fixed)
            centerValue.table.emplace_back("width", ChromeValue::ofInt(centerFixed));
        region.emplace_back("center", std::move(centerValue));
    }
    if (separator != 1)
        region.emplace_back("separator", ChromeValue::ofInt(separator));
    return ChromeValue::ofTable(std::move(region));
}

// Decode a composed footer (footer allows left/right/center) and return its
// canonical UiComposition. Aborts on a decode error -- an authoring mistake in a
// test is a test bug, not an expected outcome.
inline ssg::UiComposition composeFooter(
    std::vector<ssg::WidgetDescriptor> left,
    std::vector<ssg::WidgetDescriptor> right = {},
    std::optional<ssg::WidgetDescriptor> center = std::nullopt,
    ssg::CenterWidth centerWidth = ssg::CenterWidth::Flex, int centerFixed = 0,
    int separator = 1) {
    using ssg::ChromeValue;
    std::vector<std::string> providers;
    collectProviders(left, providers);
    collectProviders(right, providers);
    if (center) collectProviders({*center}, providers);
    ChromeValue root = ChromeValue::ofTable(
        {{"footer",
          rowTable(left, right, center, centerWidth, centerFixed, separator)}});
    auto decoded = ssg::decodeChrome(root, providers);
    if (!decoded.ok()) std::abort();
    return decoded.composition->composition();
}

// The single region of a footer-only composition.
inline ssg::UiRegion composeFooterRegion(
    std::vector<ssg::WidgetDescriptor> left,
    std::vector<ssg::WidgetDescriptor> right = {},
    std::optional<ssg::WidgetDescriptor> center = std::nullopt,
    ssg::CenterWidth centerWidth = ssg::CenterWidth::Flex, int centerFixed = 0,
    int separator = 1) {
    return composeFooter(std::move(left), std::move(right), std::move(center),
                         centerWidth, centerFixed, separator)
        .regions.front();
}

// A header composition (header is left-group only) as a UiComposition.
inline ssg::UiComposition composeHeader(std::vector<ssg::WidgetDescriptor> left,
                                        int separator = 1) {
    using ssg::ChromeValue;
    std::vector<std::string> providers;
    collectProviders(left, providers);
    ChromeValue root = ChromeValue::ofTable(
        {{"header", rowTable(left, {}, std::nullopt, ssg::CenterWidth::Flex, 0,
                             separator)}});
    auto decoded = ssg::decodeChrome(root, providers);
    if (!decoded.ok()) std::abort();
    return decoded.composition->composition();
}

// A header composition as the VALIDATED type setComposedUi requires (only the
// decoder can produce one), for tests that drive the runtime rather than inspect.
inline ssg::ValidatedComposition composeHeaderValidated(
    std::vector<ssg::WidgetDescriptor> left, int separator = 1) {
    using ssg::ChromeValue;
    std::vector<std::string> providers;
    collectProviders(left, providers);
    ChromeValue root = ChromeValue::ofTable(
        {{"header", rowTable(left, {}, std::nullopt, ssg::CenterWidth::Flex, 0,
                             separator)}});
    auto decoded = ssg::decodeChrome(root, providers);
    if (!decoded.ok()) std::abort();
    return *decoded.composition;
}

// The row content reconstructed from a canonical chrome region tree: the inverse
// of the decoder's assembly, so a test can assert on the widget groups without
// walking the container tree by hand.
struct RowView {
    std::vector<ssg::WidgetDescriptor> left;
    std::vector<ssg::WidgetDescriptor> right;
    std::optional<ssg::WidgetDescriptor> center;
    int separator = 1;
    ssg::CenterWidth centerWidth = ssg::CenterWidth::Flex;
    int centerFixed = 0;
};

inline RowView rowOf(const ssg::UiRegion& region) {
    using namespace ssg;
    RowView v;
    const auto& root = std::get<UiContainer>(region.root.content);
    const auto& left = std::get<UiContainer>(root.children[0].content);
    v.separator = left.gap.extent();
    for (const auto& c : left.children)
        v.left.push_back(std::get<UiLeaf>(c.content).widget);
    const auto& middle = std::get<UiContainer>(root.children[1].content);
    if (!middle.children.empty()) {
        v.center = std::get<UiLeaf>(middle.children[0].content).widget;
        const auto size = middle.children[0].size;
        if (size.kind() == SizeKind::Exact) {
            v.centerWidth = CenterWidth::Fixed;
            v.centerFixed = size.extent();
        }
    }
    const auto& right = std::get<UiContainer>(root.children[2].content);
    for (const auto& c : right.children)
        v.right.push_back(std::get<UiLeaf>(c.content).widget);
    return v;
}

// A composition with BOTH a header (left-only) and a footer (full left/center/
// right), decoded in one pass so both region roles are present in one schema.
inline ssg::UiComposition composeHeaderAndFooter(
    std::vector<ssg::WidgetDescriptor> headerLeft,
    std::vector<ssg::WidgetDescriptor> footerLeft,
    std::vector<ssg::WidgetDescriptor> footerRight = {},
    std::optional<ssg::WidgetDescriptor> footerCenter = std::nullopt,
    ssg::CenterWidth centerWidth = ssg::CenterWidth::Flex, int centerFixed = 0,
    int headerSeparator = 1, int footerSeparator = 1) {
    using ssg::ChromeValue;
    std::vector<std::string> providers;
    collectProviders(headerLeft, providers);
    collectProviders(footerLeft, providers);
    collectProviders(footerRight, providers);
    if (footerCenter) collectProviders({*footerCenter}, providers);
    ChromeValue root = ChromeValue::ofTable(
        {{"header", rowTable(headerLeft, {}, std::nullopt,
                             ssg::CenterWidth::Flex, 0, headerSeparator)},
         {"footer", rowTable(footerLeft, footerRight, footerCenter, centerWidth,
                             centerFixed, footerSeparator)}});
    auto decoded = ssg::decodeChrome(root, providers);
    if (!decoded.ok()) std::abort();
    return decoded.composition->composition();
}

inline bool hasRegion(const ssg::UiComposition& comp, ssg::RegionRole role) {
    for (const auto& r : comp.regions)
        if (r.role == role) return true;
    return false;
}

inline const ssg::UiRegion& regionByRole(const ssg::UiComposition& comp,
                                         ssg::RegionRole role) {
    for (const auto& r : comp.regions)
        if (r.role == role) return r;
    std::abort();
}

}  // namespace ssgtest