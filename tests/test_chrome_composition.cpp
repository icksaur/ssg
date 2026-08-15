#include "ssg/ChromeDecode.h"

#include "chrome_authoring.h"
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
    return decodeChrome(root, providers());
}

// The header/footer row reconstructed from a decoded composition, so a decoder
// test asserts on the widget groups without walking the container tree by hand.
ssgtest::RowView headerOf(const ChromeDecodeResult& r) {
    return ssgtest::rowOf(ssgtest::areaByIdRef(r.composition->composition(), ssg::kHeaderNodeId));
}
ssgtest::RowView footerOf(const ChromeDecodeResult& r) {
    return ssgtest::rowOf(ssgtest::areaByIdRef(r.composition->composition(), ssg::kFooterNodeId));
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

    // Reference comparison: expectations are literal here, independent of the
    // decoder, so a decode regression cannot match them.
    const auto h = headerOf(out);
    ASSERT_EQ(h.left.size(), std::size_t{2});
    ASSERT_EQ(h.left[0].id, std::string{"header.left[0]"});
    ASSERT_TRUE(h.left[0].value->isProvider);
    ASSERT_EQ(h.left[0].value->provider, std::string{"path"});
    ASSERT_EQ(h.left[1].value->provider, std::string{"branch"});
    ASSERT_TRUE(h.right.empty());

    const auto f = footerOf(out);
    ASSERT_EQ(f.left.size(), std::size_t{1});
    ASSERT_EQ(f.left[0].value->provider, std::string{"status"});
    ASSERT_EQ(f.right.size(), std::size_t{2});
    ASSERT_EQ(f.right[0].value->literal, std::string{"RO"});
    ASSERT_TRUE(f.right[0].role.has_value());
    ASSERT_EQ(*f.right[0].role, std::string{"footer"});
    ASSERT_EQ(f.right[1].value->literal, std::string{"reload"});
    ASSERT_TRUE(f.right[1].command.has_value());
    ASSERT_EQ(*f.right[1].command, std::string{"config.reload"});
}

// An omitted region keeps its built-in chrome (nullopt), separator defaults 1.
TEST(omittedRegionIsNulloptAndSeparatorDefaultsToOne) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("field")},
                                               {"text", str("x")}})})}})}}));
    ASSERT_TRUE(out.ok());
    ASSERT_FALSE(ssgtest::hasArea(out.composition->composition(), ssg::kHeaderNodeId));
    ASSERT_TRUE(ssgtest::hasArea(out.composition->composition(), ssg::kFooterNodeId));
    const auto f = footerOf(out);
    ASSERT_EQ(f.separator, 1);
}

TEST(explicitSeparatorIsRead) {
    const auto out = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"separator", CV::ofInt(3)}})}}));
    ASSERT_TRUE(out.ok());
    const auto f = footerOf(out);
    ASSERT_EQ(f.separator, 3);
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
    const auto f = footerOf(out);
    ASSERT_EQ(f.left[0].id, std::string{"footer.left[0]"});
    ASSERT_EQ(f.left[1].id, std::string{"mine"});
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
    const auto litRow = footerOf(lit);
    ASSERT_EQ(litRow.left[0].value->isProvider, false);
    ASSERT_EQ(litRow.left[0].value->literal, std::string{"hi"});

    const auto prov = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("field")},
                                               {"provider", str("branch")}})})}})}}));
    ASSERT_TRUE(prov.ok());
    const auto provRow = footerOf(prov);
    ASSERT_EQ(provRow.left[0].value->isProvider, true);
    ASSERT_EQ(provRow.left[0].value->provider,
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
    const auto boolRow = footerOf(boolean);
    ASSERT_EQ(boolRow.left[0].checked->isProvider, false);
    ASSERT_EQ(boolRow.left[0].checked->literal,
              std::string{"true"});

    const auto prov = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"left", CV::ofArray({widget(
                                              {{"kind", str("checkbox")},
                                               {"checked_provider", str("follow")}})})}})}}));
    ASSERT_TRUE(prov.ok());
    const auto provRow = footerOf(prov);
    ASSERT_EQ(provRow.left[0].checked->isProvider, true);
    ASSERT_EQ(provRow.left[0].checked->provider,
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
    const auto f = footerOf(ok);
    ASSERT_EQ(*f.left[0].width, 4);
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
    const auto flexRow = footerOf(flex);
    ASSERT_TRUE(flexRow.center.has_value());
    ASSERT_TRUE(flexRow.centerWidth == CenterWidth::Flex);

    const auto fixed = decode(CV::ofTable(
        {{"footer", CV::ofTable({{"center", widget({{"kind", str("label")},
                                                    {"text", str("m")},
                                                    {"width", CV::ofInt(12)}})}})}}));
    ASSERT_TRUE(fixed.ok());
    const auto fixedRow = footerOf(fixed);
    ASSERT_TRUE(fixedRow.centerWidth == CenterWidth::Fixed);
    ASSERT_EQ(fixedRow.centerFixed, 12);
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
    const auto f = footerOf(out);
    ASSERT_EQ(f.left.size(), std::size_t{64});
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
    const auto f = footerOf(out);
    ASSERT_TRUE(f.left[0].overflow == Overflow::Truncate);
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
