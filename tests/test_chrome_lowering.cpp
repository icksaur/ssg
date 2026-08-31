#include "ssg/ChromeLowering.h"

#include "chrome_authoring.h"
#include "ssg/ChromeRegionShape.h"
#include "ssg/ShellState.h"
#include "ssg/StatusQueue.h"
#include "ssg/WholeScreenAssembly.h"
#include "ssg/Widget.h"
#include "test_helpers.h"

#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ssg;
using ssgtest::composeFooterRegion;
using ssgtest::composeHeaderValidated;

UiChromeLowerResult lowerUiChromeRegion(
    const UiNode& region, Rect rect, ShellNodeKind kind,
    SemanticRole role, const Style& style,
    const ChromeProviderResolver& resolver,
    std::vector<AccessibilityNode>& out,
    const StatusViewState* status = nullptr,
    const PromptInputProjection* input = nullptr) {
    SolvedChromeSurface solved;
    auto result = ssg::lowerUiChromeRegion(
        region, rect, role, style, resolver, solved, status, input);
    for (const auto& item : solved.items) {
        auto itemKind = item.statusInvocation
                            ? ShellNodeKind::FooterAction
                            : item.id == "footer.hint"
                                  ? ShellNodeKind::FooterHint
                                  : kind;
        out.push_back({itemKind, item.id, item.label, item.rect, item.role,
                       item.content, item.command, item.statusInvocation});
    }
    if (solved.input) {
        out.push_back({kind, "input_line.query", "Input line",
                       solved.input->query, SemanticRole::Prompt,
                       solved.input->queryText});
        if (solved.input->ghost) {
            out.push_back({kind, "input_line.ghost",
                           "Input line completion", *solved.input->ghost,
                           SemanticRole::LineNumber,
                           solved.input->ghostText});
        }
    }
    return result;
}

std::vector<AccessibilityNode> lowerHeaderFieldsFromAssembly(
    const std::vector<StatusFieldCatalogEntry>& catalog,
    const ChromeProviderResolver& resolver, const Style& style,
    const std::optional<ValidatedComposition>& override = std::nullopt) {
    UiSchema uiSchema;
    uiSchema.root = assembleWholeScreen(catalog, "help.open", style.dimensions,
                                       style.inputLineSigil, override).root;
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

TEST(gridDisplayUsesProviderIdentityRatherThanDecodedWidgetId) {
    const auto region = composeFooterRegion(
        {providerField("header.left.field.0", "path", 0)});
    Style style;
    style.cwdPrefix = "cwd: ";
    SolvedChromeSurface solved;
    const auto lowered = ssg::lowerUiChromeRegion(
        region, {0, 0, 80, 1}, SemanticRole::Header, style,
        resolverFrom(
            {{"path", {"~/project", "Current path", std::nullopt}}}),
        solved);
    ASSERT_TRUE(lowered.ok());
    ASSERT_EQ(solved.items.size(), std::size_t{1});
    if (!solved.items.empty()) {
        ASSERT_EQ(solved.items.front().content,
                  std::string{"cwd: ~/project"});
    }
}

TEST(labelItemsNeverCarryCommands) {
    auto label = literal(WidgetKind::Label, "label", "Label");
    auto region = composeFooterRegion({label});
    auto& left = std::get<UiContainer>(region.content).children.front();
    auto& leaf = std::get<UiContainer>(left.content).children.front();
    std::get<UiLeaf>(leaf.content).widget.command = "must.not.dispatch";
    SolvedChromeSurface solved;
    const auto lowered = ssg::lowerUiChromeRegion(
        region, {0, 0, 80, 1}, SemanticRole::Footer,
        defaultStyle(), resolverFrom({}), solved);
    ASSERT_TRUE(lowered.ok());
    ASSERT_EQ(solved.items.size(), std::size_t{1});
    if (!solved.items.empty()) {
        ASSERT_FALSE(solved.items.front().command.has_value());
    }
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

TEST(emptyStatusViewProducesNoActionItems) {
    auto region =
        chromeRegion(kFooterNodeId, {}, {statusActions()}, std::nullopt,
                     CenterWidth::Flex, 0, 1);
    StatusViewState empty;
    SolvedChromeSurface solved;
    const auto lowered = ssg::lowerUiChromeRegion(
        region, {0, 0, 80, 1}, SemanticRole::Footer,
        defaultStyle(), resolverFrom({}), solved, &empty);
    ASSERT_TRUE(lowered.ok());
    ASSERT_TRUE(solved.items.empty());
}

TEST(semanticStateChromeSolveValidatesCorrespondenceAndBuildsInput) {
    UiSchema schema{
        Generation{7},
        assembleWholeScreen({}, "help.open", Style{}.dimensions,
                            Style{}.inputLineSigil, std::nullopt)
            .root};
    auto validated = ValidatedSchema::validate(schema);
    ASSERT_TRUE(validated.ok());
    if (!validated.ok()) return;
    const auto state =
        resolveUiState(validated.schema(), resolverFrom({}));
    const auto* root =
        std::get_if<UiContainer>(&validated.schema().schema().root.content);
    ASSERT_TRUE(root != nullptr);
    if (!root) return;
    const UiNode* header = nullptr;
    for (const auto& child : root->children) {
        if (child.id.value() == kHeaderNodeId) header = &child;
    }
    ASSERT_TRUE(header != nullptr);
    if (!header) return;

    PromptInputProjection input{true, "sa", "ve"};
    SolvedChromeSurface solved;
    const auto accepted = solveUiChromeRegion(
        *header, {0, 0, 80, 1}, SemanticRole::Header, Style{},
        Generation{7}, state, solved, nullptr, &input);
    ASSERT_TRUE(accepted.ok());
    ASSERT_TRUE(solved.input.has_value());
    if (solved.input) {
        ASSERT_TRUE(solved.input->queryText.find("sa") !=
                    std::string::npos);
        ASSERT_EQ(solved.input->ghostText, std::string{"ve"});
    }

    auto wrongGeneration = state;
    wrongGeneration.generation = Generation{8};
    const auto generationRejected = solveUiChromeRegion(
        *header, {0, 0, 80, 1}, SemanticRole::Header, Style{},
        Generation{7}, wrongGeneration, solved, nullptr, &input);
    ASSERT_FALSE(generationRejected.ok());

    auto missingNode = state;
    const auto inputState = std::ranges::find(
        missingNode.nodes,
        UiNodeId{std::string{kHeaderPromptInputNodeId}},
        &UiNodeState::id);
    ASSERT_TRUE(inputState != missingNode.nodes.end());
    if (inputState != missingNode.nodes.end()) {
        missingNode.nodes.erase(inputState);
        const auto nodeRejected = solveUiChromeRegion(
            *header, {0, 0, 80, 1}, SemanticRole::Header, Style{},
            Generation{7}, missingNode, solved, nullptr, &input);
        ASSERT_FALSE(nodeRejected.ok());
        ASSERT_TRUE(nodeRejected.error->find(kHeaderPromptInputNodeId) !=
                    std::string::npos);
    }
}

// Lower a real assembled header at `width` with a prompt-input projection, returning
// the emitted nodes. The header carries the always-present trailing TextInput leaf.
std::vector<AccessibilityNode> lowerHeaderWithPromptInput(int width, std::string query,
                                                          std::string ghost,
                                                          bool visible,
                                                          const Style& style) {
    UiSchema uiSchema;
    uiSchema.root = assembleWholeScreen(
                        {{"active_command", "Active command", StatusFieldRegion::Header, 0},
                         {"current_path", "Current path", StatusFieldRegion::Header, 1}},
                        "help.open", style.dimensions, style.inputLineSigil,
                        std::nullopt)
                        .root;
    auto validated = ValidatedSchema::validate(std::move(uiSchema));
    ASSERT_TRUE(validated.ok());
    const auto schema = validated.takeSchema();
    const auto* root = std::get_if<UiContainer>(&schema.schema().root.content);
    ASSERT_TRUE(root != nullptr);
    const UiNode* headerRegion = nullptr;
    for (const auto& child : root->children)
        if (child.id.value() == kHeaderNodeId) headerRegion = &child;
    ASSERT_TRUE(headerRegion != nullptr);
    const auto resolver = resolverFrom(
        {{"active_command", {"INSERT", "Active command", std::nullopt}},
         {"current_path", {"src/main.cpp", "Current path", std::nullopt}}});
    const PromptInputProjection input{visible, std::move(query), std::move(ghost)};
    std::vector<AccessibilityNode> out;
    const auto lowered = lowerUiChromeRegion(
        *headerRegion, {0, 0, width, 1}, ShellNodeKind::HeaderField,
        SemanticRole::Header, style, resolver, out, nullptr, &input);
    ASSERT_TRUE(lowered.ok());
    return out;
}

const AccessibilityNode* nodeById(const std::vector<AccessibilityNode>& nodes,
                                  std::string_view id) {
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

std::vector<AccessibilityNode> groupNodesOnly(std::vector<AccessibilityNode> v) {
    std::erase_if(v, [](const AccessibilityNode& n) {
        return n.id.rfind("input_line", 0) == 0;
    });
    return v;
}

// Kind: seam. The prompt input lowers by the reserve/expand rule: its fixed
// reservation is a floor subtracted before the groups pack, so the status fields
// never reflow as the query grows; the input then expands across the header's
// remaining width, scrolls a long query's tail under the pinned sigil, and clamps
// its ghost to the leftover cells. When hidden it emits nothing and the groups fill
// the whole width. Verified at a wide and a narrow width.
TEST(thePromptInputFloorsTheFieldsGrowsAndScrollsItsTailAtEveryWidth) {
    const Style style = defaultStyle();
    const std::string longQuery = std::string(300, 'x') + "TAIL";

    for (const int width : {80, 40}) {
        const auto shortQ = lowerHeaderWithPromptInput(width, "ab", "", true, style);
        const auto longQ = lowerHeaderWithPromptInput(width, longQuery, "", true, style);

        // Field stability: the fields lower identically regardless of the query,
        // because the reservation floor is subtracted before they pack.
        ASSERT_TRUE(groupNodesOnly(shortQ) == groupNodesOnly(longQ));

        // The query node stays inside the header and pins the sigil.
        const auto* shortNode = nodeById(shortQ, "input_line.query");
        ASSERT_TRUE(shortNode != nullptr);
        if (shortNode) {
            ASSERT_TRUE(shortNode->role == SemanticRole::Prompt);
            ASSERT_TRUE(shortNode->rect.right() <= width);
            ASSERT_TRUE(shortNode->content.rfind(style.inputLineSigil, 0) == 0);
        }

        // Long-tail scroll: the visible query shows its END (ends with the tail)
        // and is far shorter than the whole query, proving the head scrolled off.
        const auto* longNode = nodeById(longQ, "input_line.query");
        ASSERT_TRUE(longNode != nullptr);
        if (longNode) {
            ASSERT_TRUE(longNode->content.size() >= 4);
            ASSERT_TRUE(longNode->content.compare(longNode->content.size() - 4, 4,
                                                  "TAIL") == 0);
            ASSERT_TRUE(longNode->content.size() < longQuery.size());
            ASSERT_TRUE(longNode->rect.right() <= width);
        }
    }

    // Ghost clamp + exact adjacency: the ghost sits immediately after the query,
    // clamped to its own display cells, and carries the LineNumber role.
    const auto withGhost = lowerHeaderWithPromptInput(80, "ab", "cdef", true, style);
    const auto* query = nodeById(withGhost, "input_line.query");
    const auto* ghost = nodeById(withGhost, "input_line.ghost");
    ASSERT_TRUE(query != nullptr && ghost != nullptr);
    if (query && ghost) {
        ASSERT_TRUE(ghost->role == SemanticRole::LineNumber);
        ASSERT_EQ(ghost->rect.width, 4);
        ASSERT_EQ(ghost->rect.x, query->rect.right());
        ASSERT_TRUE(ghost->rect.right() <= 80);
    }

    // A query filling the row leaves only the held-back caret column, so the ghost
    // clamps to that single cell -- far under its own display width.
    const auto filled =
        lowerHeaderWithPromptInput(40, std::string(300, 'x'), "ghost", true, style);
    const auto* filledGhost = nodeById(filled, "input_line.ghost");
    ASSERT_TRUE(filledGhost != nullptr);
    if (filledGhost) ASSERT_EQ(filledGhost->rect.width, 1);

    // Hidden: no input nodes at all, and the groups fill the width unreserved.
    const auto hidden = lowerHeaderWithPromptInput(80, "ab", "cd", false, style);
    ASSERT_TRUE(nodeById(hidden, "input_line.query") == nullptr);
    ASSERT_TRUE(nodeById(hidden, "input_line.ghost") == nullptr);
}

TEST(aVisiblePromptProjectionWithoutTheCanonicalNodeEmitsNoInputNodes) {
    const auto region = composeFooterRegion({literal(WidgetKind::Field, "a", "A")});
    const PromptInputProjection input{true, "query", "ghost"};
    std::vector<AccessibilityNode> out;
    const auto lowered =
        lowerUiChromeRegion(region, {0, 0, 80, 1}, ShellNodeKind::HeaderField,
                            SemanticRole::Header, defaultStyle(), resolverFrom({}),
                            out, nullptr, &input);
    ASSERT_TRUE(lowered.ok());
    ASSERT_TRUE(nodeById(out, "input_line.query") == nullptr);
    ASSERT_TRUE(nodeById(out, "input_line.ghost") == nullptr);
}

TEST(aTrailingTextInputWithTheWrongIdIsRejected) {
    auto region = composeFooterRegion({literal(WidgetKind::Field, "a", "A")});
    WidgetDescriptor widget;
    widget.kind = WidgetKind::TextInput;
    widget.id = "not_input_line";
    UiNode input{UiNodeId{"not_input_line"}, Size::autoSize(), UiLeaf{widget}};
    std::get<UiContainer>(region.content).children.push_back(std::move(input));

    const PromptInputProjection projection{true, "query", ""};
    std::vector<AccessibilityNode> out;
    const auto lowered =
        lowerUiChromeRegion(region, {0, 0, 80, 1}, ShellNodeKind::HeaderField,
                            SemanticRole::Header, defaultStyle(), resolverFrom({}),
                            out, nullptr, &projection);
    ASSERT_FALSE(lowered.ok());
}

}  // namespace

int main() {
    RUN(lowersLiteralAndProviderWidgets);
    RUN(lowersCheckboxWithRoleOverride);
    RUN(dropsEmptyProviderWidget);
    RUN(dropsProviderWithEmptyLabel);
    RUN(gridDisplayUsesProviderIdentityRatherThanDecodedWidgetId);
    RUN(labelItemsNeverCarryCommands);
    RUN(spacerCreatesGapWithoutANode);
    RUN(composedProviderHeaderMatchesBuiltinSpanForSpan);
    RUN(statusActionItemsKeepTypedInvocationAndPlainFooterWidgetsKeepRegionKind);
    RUN(statusActionItemsCollapseBeforeHintAndFieldsAtNarrowWidth);
    RUN(emptyStatusViewProducesNoActionItems);
    RUN(semanticStateChromeSolveValidatesCorrespondenceAndBuildsInput);
    RUN(thePromptInputFloorsTheFieldsGrowsAndScrollsItsTailAtEveryWidth);
    RUN(aVisiblePromptProjectionWithoutTheCanonicalNodeEmitsNoInputNodes);
    RUN(aTrailingTextInputWithTheWrongIdIsRejected);
    return 0;
}
