#include "ssg/ChromeComposition.h"
#include "test_helpers.h"

#include <string>
#include <vector>

namespace {

using namespace ssg;
using CV = ssg::ChromeValue;

std::vector<std::string> providers() {
    return {"path", "branch", "status", "follow"};
}

ChromeDecodeResult decode(const CV& root) {
    return decodeChromeComposition(root, providers());
}

CV str(std::string s) { return CV::ofString(std::move(s)); }

// A widget table from key/value pairs, for readable trees.
CV widget(std::vector<std::pair<std::string, CV>> entries) {
    return CV::ofTable(std::move(entries));
}

// --- Happy path: a hand-built tree decodes to a hand-built descriptor tree ---
// (reference comparison: the expected side is constructed independently, so a
// decode regression cannot match it.)
TEST(decodesHeaderLeftAndFooterGroups) {
    const CV root = CV::ofTable(
        {{"header",
          CV::ofTable({{"left", CV::ofArray({widget({{"kind", str("field")},
                                                      {"provider", str("path")}}),
                                             widget({{"kind", str("field")},
                                                     {"provider", str("branch")}})})}})},
         {"footer",
          CV::ofTable(
              {{"left", CV::ofArray({widget({{"kind", str("field")},
                                             {"provider", str("status")}})})},
               {"right", CV::ofArray({widget({{"kind", str("label")},
                                              {"text", str("RO")},
                                              {"role", str("footer")}}),
                                      widget({{"kind", str("field")},
                                              {"text", str("reload")},
                                              {"command", str("config.reload")}})})}})}});

    const auto out = decode(root);
    ASSERT_TRUE(out.ok());
    ASSERT_TRUE(out.composition.has_value());
    if (!out.composition) return;

    ChromeComposition expected;
    RowDescriptor header;
    header.left.push_back(
        {WidgetKind::Field, "header.left[0]", ValueSource{true, "", "path"}, {},
         {}, {}, {}, 0, false, Overflow::None, ""});
    header.left.push_back(
        {WidgetKind::Field, "header.left[1]", ValueSource{true, "", "branch"}, {},
         {}, {}, {}, 0, false, Overflow::None, ""});
    expected.header = header;
    RowDescriptor footer;
    footer.left.push_back(
        {WidgetKind::Field, "footer.left[0]", ValueSource{true, "", "status"}, {},
         {}, {}, {}, 0, false, Overflow::None, ""});
    footer.right.push_back(
        {WidgetKind::Label, "footer.right[0]", ValueSource{false, "RO", ""}, {},
         {}, std::optional<std::string>{"footer"}, {}, 0, false, Overflow::None,
         ""});
    footer.right.push_back(
        {WidgetKind::Field, "footer.right[1]", ValueSource{false, "reload", ""},
         {}, {}, {}, std::optional<std::string>{"config.reload"}, 0, false,
         Overflow::None, ""});
    expected.footer = footer;

    ASSERT_TRUE(*out.composition == expected);
}

// An omitted region keeps its built-in chrome (nullopt), separator defaults 1.
TEST(omittedRegionIsNulloptAndSeparatorDefaultsToOne) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("field")},
                                               {"text", str("x")}})})}})}}));
    ASSERT_TRUE(out.ok());
    ASSERT_FALSE(out.composition->header.has_value());
    ASSERT_TRUE(out.composition->footer.has_value());
    ASSERT_EQ(out.composition->footer->separator, 1);
}

TEST(explicitSeparatorIsRead) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"separator", CV::ofInt(3)}})}}));
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.composition->footer->separator, 3);
}

// An omitted `id` defaults to the decode path (stable/unique); an author id wins.
TEST(idDefaultsToPathAndAuthorIdWins) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({
                                              widget({{"kind", str("field")},
                                                      {"text", str("a")}}),
                                              widget({{"kind", str("field")},
                                                      {"id", str("mine")},
                                                      {"text", str("b")}})})}})}}));
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.composition->footer->left[0].id, std::string{"footer.left[0]"});
    ASSERT_EQ(out.composition->footer->left[1].id, std::string{"mine"});
}

// Negative geometry values are fail-loud, not silently degraded.
TEST(negativeSeparatorAndWidthFailLoud) {
    const auto sep = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"separator", CV::ofInt(-1)}})}}));
    ASSERT_FALSE(sep.ok());
    ASSERT_EQ(*sep.error, std::string{"footer.separator: must not be negative"});

    const auto spacer = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("spacer")},
                                               {"width", CV::ofInt(-4)}})})}})}}));
    ASSERT_FALSE(spacer.ok());
    ASSERT_EQ(*spacer.error, std::string{"footer.left[0].width: must not be negative"});

    const auto center = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"center", widget({{"kind", str("label")},
                                                    {"text", str("m")},
                                                    {"width", CV::ofInt(-2)}})}})}}));
    ASSERT_FALSE(center.ok());
    ASSERT_EQ(*center.error, std::string{"footer.center.width: must not be negative"});
}

// --- Value source ---
TEST(textLiteralAndProviderAreDistinctSources) {
    const auto lit = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("label")},
                                               {"text", str("hi")}})})}})}}));
    ASSERT_TRUE(lit.ok());
    ASSERT_EQ(lit.composition->footer->left[0].value->isProvider, false);
    ASSERT_EQ(lit.composition->footer->left[0].value->literal, std::string{"hi"});

    const auto prov = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("field")},
                                               {"provider", str("branch")}})})}})}}));
    ASSERT_TRUE(prov.ok());
    ASSERT_EQ(prov.composition->footer->left[0].value->isProvider, true);
    ASSERT_EQ(prov.composition->footer->left[0].value->provider,
              std::string{"branch"});
}

TEST(bothTextAndProviderFailsLoud) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("field")},
                                               {"text", str("a")},
                                               {"provider", str("path")}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left[0]: value has both \"text\" and "
                          "\"provider\""});
}

TEST(labelAndFieldRequireAValue) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("label")}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left[0]: kind \"label\" requires \"text\" or "
                          "\"provider\""});
}

TEST(unknownProviderFailsLoud) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("field")},
                                               {"provider", str("clock")}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left[0].provider: unknown provider \"clock\""});
}

// --- Kind gating ---
TEST(unknownKindFailsLoud) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("buton")}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left[0].kind: unknown kind \"buton\""});
}

TEST(textInputIsRejectedInChrome) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("text_input")}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left[0]: \"text_input\" is not allowed in chrome"});
}

TEST(containerIsNotALeafWidget) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("container")}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left[0]: \"container\" is not a leaf widget"});
}

// --- Checkbox ---
TEST(checkboxCheckedFromBoolAndProvider) {
    const auto boolean = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("checkbox")},
                                               {"text", str("case")},
                                               {"checked", CV::ofBool(true)}})})}})}}));
    ASSERT_TRUE(boolean.ok());
    ASSERT_EQ(boolean.composition->footer->left[0].checked->isProvider, false);
    ASSERT_EQ(boolean.composition->footer->left[0].checked->literal,
              std::string{"true"});

    const auto prov = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("checkbox")},
                                               {"checked_provider", str("follow")}})})}})}}));
    ASSERT_TRUE(prov.ok());
    ASSERT_EQ(prov.composition->footer->left[0].checked->isProvider, true);
    ASSERT_EQ(prov.composition->footer->left[0].checked->provider,
              std::string{"follow"});
}

TEST(checkboxRequiresACheckedSource) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("checkbox")},
                                               {"text", str("x")}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left[0]: kind \"checkbox\" requires "
                          "\"checked\" or \"checked_provider\""});
}

TEST(checkedOnANonCheckboxFailsLoud) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("field")},
                                               {"text", str("x")},
                                               {"checked", CV::ofBool(true)}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left[0]: \"checked\" only allowed for kind "
                          "\"checkbox\""});
}

// --- Spacer / width ---
TEST(leftRightSpacerRequiresIntegerWidth) {
    const auto missing = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("spacer")}})})}})}}));
    ASSERT_FALSE(missing.ok());
    ASSERT_EQ(*missing.error,
              std::string{"footer.left[0]: a left/right \"spacer\" requires an "
                          "integer \"width\""});

    const auto ok = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("spacer")},
                                               {"width", CV::ofInt(4)}})})}})}}));
    ASSERT_TRUE(ok.ok());
    ASSERT_EQ(*ok.composition->footer->left[0].width, 4);
}

TEST(widthForbiddenOnLeftRightNonSpacer) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("label")},
                                               {"text", str("x")},
                                               {"width", CV::ofInt(4)}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left[0].width: not allowed for a left/right "
                          "non-spacer widget"});
}

TEST(centerWidthFlexAndFixed) {
    const auto flex = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"center", widget({{"kind", str("label")},
                                                    {"text", str("m")},
                                                    {"width", str("flex")}})}})}}));
    ASSERT_TRUE(flex.ok());
    ASSERT_TRUE(flex.composition->footer->center.has_value());
    ASSERT_TRUE(flex.composition->footer->centerWidth == CenterWidth::Flex);

    const auto fixed = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"center", widget({{"kind", str("label")},
                                                    {"text", str("m")},
                                                    {"width", CV::ofInt(12)}})}})}}));
    ASSERT_TRUE(fixed.ok());
    ASSERT_TRUE(fixed.composition->footer->centerWidth == CenterWidth::Fixed);
    ASSERT_EQ(fixed.composition->footer->centerFixed, 12);
}

// --- Header is left-group only ---
TEST(headerRightIsRejected) {
    const auto out = decode(CV::ofTable(
        {{"header", CV::ofTable({{"right", CV::ofArray({widget(
                                               {{"kind", str("label")},
                                                {"text", str("x")}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"header.right: not allowed (header is left-group only)"});
}

TEST(headerCenterIsRejected) {
    const auto out = decode(CV::ofTable(
        {{"header", CV::ofTable({{"center", widget({{"kind", str("label")},
                                                    {"text", str("x")}})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"header.center: not allowed (header is left-group only)"});
}

// --- command / role gating ---
TEST(commandForbiddenOnLabel) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("label")},
                                               {"text", str("x")},
                                               {"command", str("noop")}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left[0].command: not allowed for kind \"label\""});
}

TEST(roleForbiddenOnSpacer) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("spacer")},
                                               {"width", CV::ofInt(2)},
                                               {"role", str("footer")}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left[0].role: not allowed for kind \"spacer\""});
}

// --- Unknown fields ---
TEST(unknownWidgetFieldFailsLoud) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("field")},
                                               {"text", str("x")},
                                               {"colour", str("red")}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error, std::string{"footer.left[0].colour: unknown field"});
}

TEST(unknownRegionFieldFailsLoud) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"middle", CV::ofArray({})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error, std::string{"footer.middle: unknown field"});
}

TEST(unknownRootFieldFailsLoud) {
    const auto out = decode(CV::ofTable({{"sidebar", CV::ofTable({})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error, std::string{"chrome.sidebar: unknown field"});
}

// --- Caps ---
TEST(overPerSideCapFailsLoud) {
    std::vector<CV> items;
    for (int i = 0; i < kChromeMaxPerSide + 1; ++i)
        items.push_back(widget({{"kind", str("field")}, {"text", str("x")}}));
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray(std::move(items))}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left: exceeds widgets per side (65 > 64)"});
}

// A large but valid composition (per-side under the cap, in every slot) decodes
// successfully -- proving the total-widget ceiling is not spuriously tripped.
// (The 256 total cap is a forward-looking forms ceiling; in chrome the per-side
// cap of 64 over a header side + three footer slots bounds well under it.)
TEST(largeValidCompositionUnderCapsSucceeds) {
    std::vector<CV> full;
    for (int i = 0; i < kChromeMaxPerSide; ++i)
        full.push_back(widget({{"kind", str("field")}, {"text", str("x")}}));
    const CV root = CV::ofTable(
        {{"header", CV::ofTable({{"left", CV::ofArray(full)}})},
         {"footer", CV::ofTable({{"left", CV::ofArray(full)},
                                 {"right", CV::ofArray(full)},
                                 {"center", widget({{"kind", str("spacer")}})}})}});
    const auto out = decode(root);
    ASSERT_TRUE(out.ok());
    ASSERT_EQ(out.composition->footer->left.size(), std::size_t{64});
}

// --- overflow ---
TEST(unknownOverflowFailsLoud) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("field")},
                                               {"text", str("x")},
                                               {"overflow", str("wrap")}})})}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error,
              std::string{"footer.left[0].overflow: unknown overflow \"wrap\""});
}

TEST(overflowTruncateParses) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("field")},
                                               {"text", str("x")},
                                               {"overflow", str("truncate")}})})}})}}));
    ASSERT_TRUE(out.ok());
    ASSERT_TRUE(out.composition->footer->left[0].overflow == Overflow::Truncate);
}

// --- structural ---
TEST(rootMustBeATable) {
    const auto out = decode(CV::ofString("nope"));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error, std::string{"chrome: expected a table"});
}

TEST(sideMustBeAnArray) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", str("nope")}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error, std::string{"footer.left: expected an array"});
}

// A huge integer is rejected rather than silently narrowed to int.
TEST(outOfRangeIntegerFailsLoud) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"separator", CV::ofInt(9999999999LL)}})}}));
    ASSERT_FALSE(out.ok());
    ASSERT_EQ(*out.error, std::string{"footer.separator: out of range"});
}

}  // namespace

int main() {
    RUN(decodesHeaderLeftAndFooterGroups);
    RUN(omittedRegionIsNulloptAndSeparatorDefaultsToOne);
    RUN(explicitSeparatorIsRead);
    RUN(idDefaultsToPathAndAuthorIdWins);
    RUN(negativeSeparatorAndWidthFailLoud);
    RUN(outOfRangeIntegerFailsLoud);
    RUN(textLiteralAndProviderAreDistinctSources);
    RUN(bothTextAndProviderFailsLoud);
    RUN(labelAndFieldRequireAValue);
    RUN(unknownProviderFailsLoud);
    RUN(unknownKindFailsLoud);
    RUN(textInputIsRejectedInChrome);
    RUN(containerIsNotALeafWidget);
    RUN(checkboxCheckedFromBoolAndProvider);
    RUN(checkboxRequiresACheckedSource);
    RUN(checkedOnANonCheckboxFailsLoud);
    RUN(leftRightSpacerRequiresIntegerWidth);
    RUN(widthForbiddenOnLeftRightNonSpacer);
    RUN(centerWidthFlexAndFixed);
    RUN(headerRightIsRejected);
    RUN(headerCenterIsRejected);
    RUN(commandForbiddenOnLabel);
    RUN(roleForbiddenOnSpacer);
    RUN(unknownWidgetFieldFailsLoud);
    RUN(unknownRegionFieldFailsLoud);
    RUN(unknownRootFieldFailsLoud);
    RUN(overPerSideCapFailsLoud);
    RUN(largeValidCompositionUnderCapsSucceeds);
    RUN(unknownOverflowFailsLoud);
    RUN(overflowTruncateParses);
    RUN(rootMustBeATable);
    RUN(sideMustBeAnArray);
    return 0;
}
