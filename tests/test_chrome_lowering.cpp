#include "ssg/ChromeLowering.h"

#include "chrome_authoring.h"
#include "ssg/ChromeRegionShape.h"
#include "ssg/ShellState.h"
#include "ssg/StatusQueue.h"
#include "ssg/WholeScreenAssembly.h"
#include "ssg/Widget.h"
#include "test_helpers.h"

#include <optional>
#include <string>
#include <vector>

namespace {

using namespace ssg;
using ssgtest::composeFooterRegion;
using ssgtest::composeHeaderValidated;

std::vector<AccessibilityNode> lowerHeaderFieldsFromAssembly(
    const std::vector<StatusFieldCatalogEntry>& catalog,
    const ChromeProviderResolver& resolver, const Style& style,
    const std::optional<ValidatedComposition>& override = std::nullopt) {
    UiSchema uiSchema;
    uiSchema.root = assembleWholeScreen(catalog, "help.open", style.dimensions,
                                      override).root;
    auto validated = ValidatedSchema::validate(std::move(uiSchema));
    ASSERT_TRUE(validated.ok());
    const auto validatedSchema = validated.takeSchema();
    const auto* root = std::get_if<UiContainer>(&validatedSchema.schema().root.content);
    ASSERT_TRUE(root != nullptr);
    const UiNode* headerRegion = nullptr;
    for (const auto& child : root->children)
        if (child.id.value() == kHeaderNodeId) headerRegion = &child;
    ASSERT_TRUE(headerRegion != nullptr);
    std::vector<AccessibilityNode> header;
    const auto lowered = lowerUiChromeRegion(
        *headerRegion, {0, 0, 80, 1}, ShellNodeKind::HeaderField,
        SemanticRole::Header, style, resolver, header);
    ASSERT_TRUE(lowered.ok());
    return header;
}

Style defaultStyle() { return Style{}; }

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

WidgetDescriptor statusActions() {
    WidgetDescriptor w;
    w.kind = WidgetKind::StatusActions;
    w.id = "footer.status_actions";
    w.rank = 1;
    return w;
}

StatusViewState twoActionStatus() {
    return StatusViewState{{StatusItemView{
        StatusId{42}, StatusPriority::Information, 7, "status",
        {StatusAction{"first", "First", "ignored.first"},
         StatusAction{"second", "Second", "ignored.second"}}}}, 0};
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

TEST(composedProviderHeaderMatchesBuiltinSpanForSpan) {
    const Style style = defaultStyle();
    const std::vector<StatusFieldCatalogEntry> catalog{
        {"path", "Current path", StatusFieldRegion::Header, 1},
        {"git_branch", "Git branch", StatusFieldRegion::Header, 5}};
    const auto resolver = resolverFrom(
        {{"path", {"~/proj", "Current path",
                   std::optional<std::string>{"panel.show_files"}}},
         {"git_branch", {"main", "Git branch",
                         std::optional<std::string>{"panel.show_git_status"}}}});

    const auto builtin = lowerHeaderFieldsFromAssembly(catalog, resolver, style);
    ASSERT_EQ(builtin.size(), std::size_t{2});

    const auto composed = composeHeaderValidated(
        {providerField("path", "path", 1),
         providerField("git_branch", "git_branch", 5)});
    const auto composedNodes =
        lowerHeaderFieldsFromAssembly(catalog, resolver, style, composed);

    ASSERT_TRUE(composedNodes == builtin);
}

TEST(statusActionItemsKeepTypedInvocationAndPlainFooterWidgetsKeepRegionKind) {
    auto hint = providerField("footer.hint", "footer.hint", 0);
    hint.overflow = Overflow::Truncate;
    auto region = chromeRegion(kFooterNodeId,
        {literal(WidgetKind::Label, "plain_label", "LBL"),
         providerField("plain_field", "field", 0)},
        {hint, statusActions()}, std::nullopt, CenterWidth::Flex, 0, 1);
    Style style = defaultStyle();
    style.dimensions.labelPadding = 5;
    auto status = twoActionStatus();
    std::vector<AccessibilityNode> out;
    auto lowered = lowerUiChromeRegion(
        region, {0, 0, 80, 1}, ShellNodeKind::FooterField,
        SemanticRole::Footer, style,
        resolverFrom({{"footer.hint", {"Help", "Help", std::string{"help.open"}}},
                      {"field", {"FIELD", "Plain field", std::nullopt}}}),
        out, &status);
    ASSERT_TRUE(lowered.ok());
    ASSERT_EQ(out.size(), std::size_t{5});
    ASSERT_EQ(out[0].kind, ShellNodeKind::FooterField);
    ASSERT_EQ(out[0].id, std::string{"plain_label"});
    ASSERT_EQ(out[1].kind, ShellNodeKind::FooterField);
    ASSERT_EQ(out[1].id, std::string{"plain_field"});
    ASSERT_EQ(out[2].kind, ShellNodeKind::FooterHint);
    ASSERT_EQ(out[2].id, std::string{"footer.hint"});
    ASSERT_EQ(out[2].rect.width, 4 + style.dimensions.labelPadding);
    ASSERT_EQ(out[3].kind, ShellNodeKind::FooterAction);
    ASSERT_EQ(out[3].id, std::string{"first"});
    ASSERT_TRUE(out[3].role == SemanticRole::StatusInfo);
    ASSERT_EQ(out[3].statusInvocation,
              (StatusActionInvocation{StatusId{42}, "first", 7}));
    ASSERT_FALSE(out[3].commandId.has_value());
    ASSERT_EQ(out[4].kind, ShellNodeKind::FooterAction);
    ASSERT_EQ(out[4].id, std::string{"second"});
    ASSERT_EQ(out[4].statusInvocation,
              (StatusActionInvocation{StatusId{42}, "second", 7}));
    ASSERT_EQ(out[3].rect.width, 5 + style.dimensions.labelPadding);
}

TEST(statusActionItemsCollapseBeforeHintAndFieldsAtNarrowWidth) {
    auto hint = providerField("footer.hint", "footer.hint", 0);
    hint.overflow = Overflow::Truncate;
    auto region = chromeRegion(kFooterNodeId,
        {literal(WidgetKind::Label, "plain_label", "LBL"),
         providerField("plain_field", "field", 0)},
        {hint, statusActions()}, std::nullopt, CenterWidth::Flex, 0, 1);
    Style style = defaultStyle();
    style.dimensions.labelPadding = 3;
    auto status = twoActionStatus();
    std::vector<AccessibilityNode> out;
    auto lowered = lowerUiChromeRegion(
        region, {0, 0, 12, 1}, ShellNodeKind::FooterField,
        SemanticRole::Footer, style,
        resolverFrom({{"footer.hint", {"Help", "Help", std::string{"help.open"}}},
                      {"field", {"FIELD", "Plain field", std::nullopt}}}),
        out, &status);
    ASSERT_TRUE(lowered.ok());
    ASSERT_EQ(out.size(), std::size_t{2});
    ASSERT_EQ(out[0].kind, ShellNodeKind::FooterAction);
    ASSERT_EQ(out[0].id, std::string{"first"});
    ASSERT_EQ(out[1].kind, ShellNodeKind::FooterAction);
    ASSERT_EQ(out[1].id, std::string{"second"});
}

}  // namespace

int main() {
    RUN(lowersLiteralAndProviderWidgets);
    RUN(lowersCheckboxWithRoleOverride);
    RUN(dropsEmptyProviderWidget);
    RUN(dropsProviderWithEmptyLabel);
    RUN(spacerCreatesGapWithoutANode);
    RUN(composedProviderHeaderMatchesBuiltinSpanForSpan);
    RUN(statusActionItemsKeepTypedInvocationAndPlainFooterWidgetsKeepRegionKind);
    RUN(statusActionItemsCollapseBeforeHintAndFieldsAtNarrowWidth);
    return 0;
}
