#include "ssg/ChromeLowering.h"

#include "chrome_authoring.h"
#include "ssg/ShellState.h"
#include "ssg/Widget.h"
#include "test_helpers.h"

#include <optional>
#include <string>
#include <vector>

namespace {

using namespace ssg;
using ssgtest::composeFooterRegion;

Style defaultStyle() { return Style{}; }

// Stage-(i) bridge: computeShellLayout reads panel presence and focus from the request;
// source them from the ShellState under test, exactly as production does.
ShellLayoutResult layoutFor(ShellLayoutRequest request, const ShellState& state) {
    request.panelPresent = state.panelRequested();
    request.focus = state.focus();
    return computeShellLayout(request, state);
}

// A resolver from a fixed id -> (value, label, command) table.
ChromeProviderResolver resolverFrom(
    std::vector<std::pair<std::string, ResolvedProvider>> table) {
    return [table = std::move(table)](
               std::string_view id) -> std::optional<ResolvedProvider> {
        for (const auto& [key, value] : table)
            if (key == id) return value;
        return std::nullopt;
    };
}

WidgetDescriptor literal(WidgetKind kind, std::string id, std::string text) {
    WidgetDescriptor w;
    w.kind = kind;
    w.id = std::move(id);
    w.value = ValueSource{false, std::move(text), ""};
    return w;
}

WidgetDescriptor providerField(std::string id, std::string providerId,
                               int rank) {
    WidgetDescriptor w;
    w.kind = WidgetKind::Field;
    w.id = std::move(id);
    w.value = ValueSource{true, "", std::move(providerId)};
    w.rank = rank;
    return w;
}

// A literal Label labels itself with its text; a literal Field with a command
// carries that command. Hand-computed geometry: sep 1, measureFieldCells = cells
// + 2. "AB" -> 4 at offset 0; "main" (a provider value) -> 6 at offset 5.
TEST(lowersLiteralAndProviderWidgets) {
    const auto region = composeFooterRegion(
        {literal(WidgetKind::Label, "l", "AB"), providerField("branch", "branch", 0)});

    const auto resolver = resolverFrom(
        {{"branch", {"main", "Git branch", std::optional<std::string>{
                                               "panel.show_git_status"}}}});
    std::vector<AccessibilityNode> out;
    const auto lowered =
        lowerUiChromeRegion(region, {0, 0, 80, 1}, ShellNodeKind::HeaderField,
                            SemanticRole::Header, defaultStyle(), resolver, out);
    ASSERT_TRUE(lowered.ok());

    ASSERT_EQ(out.size(), std::size_t{2});
    ASSERT_EQ(out[0], (AccessibilityNode{ShellNodeKind::HeaderField, "l", "AB",
                                         {0, 0, 4, 1}, SemanticRole::Header, "AB",
                                         std::nullopt}));
    ASSERT_EQ(out[1],
              (AccessibilityNode{ShellNodeKind::HeaderField, "branch",
                                 "Git branch", {5, 0, 6, 1}, SemanticRole::Header,
                                 "main",
                                 std::optional<std::string>{"panel.show_git_status"}}));
}

// A checkbox composes its glyph text via checkboxText; a widget `role` overrides
// the region default. Content/role/rect are checked; the glyph itself is
// checkboxText's own tested output.
TEST(lowersCheckboxWithRoleOverride) {
    const Style style = defaultStyle();
    WidgetDescriptor cb;
    cb.kind = WidgetKind::Checkbox;
    cb.id = "opt";
    cb.value = ValueSource{false, "case", ""};
    cb.checked = ValueSource{false, "true", ""};
    cb.role = "footer";
    const auto region = composeFooterRegion({cb});

    std::vector<AccessibilityNode> out;
    const auto lowered =
        lowerUiChromeRegion(region, {3, 4, 80, 1}, ShellNodeKind::FooterField,
                            SemanticRole::Footer, style, resolverFrom({}), out);
    ASSERT_TRUE(lowered.ok());

    const std::string content = checkboxText(true, "case", style.toggle);
    ASSERT_EQ(out.size(), std::size_t{1});
    ASSERT_EQ(out[0].content, content);
    ASSERT_TRUE(out[0].role == SemanticRole::Footer);
    ASSERT_EQ(out[0].rect, (Rect{3, 4, measureFieldCells(content), 1}));
    ASSERT_EQ(out[0].id, std::string{"opt"});
}

// An empty/unknown provider drops the widget (as the built-in drops an
// empty-value field); the survivor packs from offset 0.
TEST(dropsEmptyProviderWidget) {
    const auto region = composeFooterRegion(
        {providerField("a", "missing", 0), literal(WidgetKind::Field, "b", "keep")});

    std::vector<AccessibilityNode> out;
    const auto lowered =
        lowerUiChromeRegion(region, {0, 0, 80, 1}, ShellNodeKind::HeaderField,
                            SemanticRole::Header, defaultStyle(), resolverFrom({}),
                            out);
    ASSERT_TRUE(lowered.ok());

    ASSERT_EQ(out.size(), std::size_t{1});
    ASSERT_EQ(out[0].id, std::string{"b"});
    ASSERT_EQ(out[0].rect, (Rect{0, 0, measureFieldCells("keep"), 1}));
}

// A Spacer occupies stack space (creating a gap) but emits no node. "x"(3) at 0,
// sep, spacer(5) at 4, sep, "y"(3) at 10.
TEST(spacerCreatesGapWithoutANode) {
    WidgetDescriptor spacer;
    spacer.kind = WidgetKind::Spacer;
    spacer.id = "s";
    spacer.width = 5;
    const auto region = composeFooterRegion(
        {literal(WidgetKind::Field, "a", "x"), spacer,
         literal(WidgetKind::Field, "b", "y")});

    std::vector<AccessibilityNode> out;
    const auto lowered =
        lowerUiChromeRegion(region, {0, 0, 80, 1}, ShellNodeKind::HeaderField,
                            SemanticRole::Header, defaultStyle(), resolverFrom({}),
                            out);
    ASSERT_TRUE(lowered.ok());

    ASSERT_EQ(out.size(), std::size_t{2});
    ASSERT_EQ(out[0].id, std::string{"a"});
    ASSERT_EQ(out[0].rect, (Rect{0, 0, 3, 1}));
    ASSERT_EQ(out[1].id, std::string{"b"});
    ASSERT_EQ(out[1].rect, (Rect{10, 0, 3, 1}));  // 3 + 1(sep) + 5 + 1(sep)
}

// PARITY: a provider-only composed header projects the SAME full nodes
// (id/label/rect/role/content/commandId) as the built-in header path
// (computeShellLayout), proving replace is transparent for providers.
TEST(composedProviderHeaderMatchesBuiltinSpanForSpan) {
    // Drive the real built-in header emission with two status fields (value +
    // accessible label + collapse rank + command all set as the runtime would).
    ShellLayoutRequest req;
    req.viewport = {80, 24};
    req.headerFields = {
        StatusField{.id = "path",
                    .accessibleLabel = "Current path",
                    .value = "~/proj",
                    .collapseRank = 1,
                    .commandId = std::optional<std::string>{"panel.show_files"}},
        StatusField{.id = "git_branch",
                    .accessibleLabel = "Git branch",
                    .value = "main",
                    .collapseRank = 5,
                    .commandId =
                        std::optional<std::string>{"panel.show_git_status"}}};
    req.tabs = {{"main.cpp", "main.cpp tab", true}};
    req.panelProviderLabel = "Files";

    ShellState state;
    const auto result = layoutFor(req, state);
    ASSERT_TRUE(result.accepted());
    if (!result.accepted()) return;
    ASSERT_TRUE(result.view->header.has_value());

    std::vector<AccessibilityNode> builtin;
    for (const auto& node : result.view->accessibilityNodes)
        if (node.kind == ShellNodeKind::HeaderField) builtin.push_back(node);
    ASSERT_EQ(builtin.size(), std::size_t{2});

    // Compose a mirroring header: provider Field widgets with matching ids +
    // ranks; the resolver returns exactly each field's (value,label,command).
    const auto region = composeFooterRegion(
        {providerField("path", "path", 1), providerField("git_branch", "git_branch", 5)});
    const auto resolver = resolverFrom(
        {{"path", {"~/proj", "Current path",
                   std::optional<std::string>{"panel.show_files"}}},
         {"git_branch", {"main", "Git branch",
                         std::optional<std::string>{"panel.show_git_status"}}}});

    const Rect header = *result.view->header;
    std::vector<AccessibilityNode> composed;
    const auto lowered = lowerUiChromeRegion(
        region, {header.x, header.y, header.width, 1},
        ShellNodeKind::HeaderField, SemanticRole::Header, req.style, resolver,
        composed);
    ASSERT_TRUE(lowered.ok());

    ASSERT_TRUE(composed == builtin);
}

// A provider with a non-empty value but an EMPTY accessible label is dropped,
// matching the built-in skip on `accessibleLabel.empty() || value.empty()`.
TEST(dropsProviderWithEmptyLabel) {
    const auto region = composeFooterRegion(
        {providerField("a", "labelless", 0), literal(WidgetKind::Field, "b", "keep")});

    const auto resolver = resolverFrom(
        {{"labelless", {"has-value", "", std::nullopt}}});
    std::vector<AccessibilityNode> out;
    const auto lowered =
        lowerUiChromeRegion(region, {0, 0, 80, 1}, ShellNodeKind::HeaderField,
                            SemanticRole::Header, defaultStyle(), resolver, out);
    ASSERT_TRUE(lowered.ok());

    ASSERT_EQ(out.size(), std::size_t{1});
    ASSERT_EQ(out[0].id, std::string{"b"});
}

}  // namespace

int main() {
    RUN(lowersLiteralAndProviderWidgets);
    RUN(lowersCheckboxWithRoleOverride);
    RUN(dropsEmptyProviderWidget);
    RUN(dropsProviderWithEmptyLabel);
    RUN(spacerCreatesGapWithoutANode);
    RUN(composedProviderHeaderMatchesBuiltinSpanForSpan);
    return 0;
}
