// Differential oracle for the UI-VM chrome bridge + generic lowering (spec phase
// 4: dual-path publish + byte-for-byte faithfulness). The strong property: the
// medium-agnostic tree lowered DIRECTLY through lowerUiChromeRegion produces the
// identical AccessibilityNodes as lowering the legacy RowDescriptor through
// lowerChromeRow -- two independent lowerings, so parity is a real proof, not a
// round-trip tautology. Plus: the built tree validates, and a malformed tree
// fails loud.

#include "ssg/ChromeComposition.h"
#include "ssg/ChromeLowering.h"
#include "ssg/Style.h"
#include "ssg/UiChromeBridge.h"
#include "ssg/UiTree.h"
#include "test_helpers.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

using ssg::AccessibilityNode;
using ssg::CenterWidth;
using ssg::ChromeComposition;
using ssg::ChromeProviderResolver;
using ssg::Generation;
using ssg::lowerChromeRow;
using ssg::lowerUiChromeRegion;
using ssg::Overflow;
using ssg::Rect;
using ssg::RegionRole;
using ssg::ResolvedProvider;
using ssg::RowDescriptor;
using ssg::SemanticRole;
using ssg::ShellNodeKind;
using ssg::Style;
using ssg::uiChromeRegionFromRow;
using ssg::uiSchemaFromChrome;
using ssg::UiContainer;
using ssg::UiNode;
using ssg::UiNodeId;
using ssg::UiRegion;
using ssg::ValueSource;
using ssg::WidgetDescriptor;
using ssg::WidgetKind;

ValueSource literal(std::string text) {
    ValueSource v;
    v.isProvider = false;
    v.literal = std::move(text);
    return v;
}
ValueSource provider(std::string id) {
    ValueSource v;
    v.isProvider = true;
    v.provider = std::move(id);
    return v;
}

WidgetDescriptor label(std::string id, std::string text) {
    WidgetDescriptor w;
    w.kind = WidgetKind::Label;
    w.id = std::move(id);
    w.value = literal(std::move(text));
    return w;
}

// A corpus of compositions spanning the behavior-affecting fields the lowering
// reads: providers, checkboxes, spacers, overflow (ScrollTail), keep, rank,
// sigil, custom role, and center width policies.
std::vector<ChromeComposition> corpus() {
    std::vector<ChromeComposition> all;

    {  // header: left providers + literal, right literal, ranks + keep
        ChromeComposition c;
        RowDescriptor row;
        WidgetDescriptor path;
        path.kind = WidgetKind::Field;
        path.id = "path";
        path.value = provider("file.path");
        path.command = "file.reveal";
        path.rank = 2;
        WidgetDescriptor branch;
        branch.kind = WidgetKind::Field;
        branch.id = "branch";
        branch.value = provider("git.branch");
        branch.rank = 1;
        branch.keep = true;
        WidgetDescriptor dirty;
        dirty.kind = WidgetKind::Label;
        dirty.id = "dirty";
        dirty.value = literal("*");
        dirty.role = "status_warning";
        dirty.rank = 0;
        row.left = {path, branch, dirty};
        row.right = {label("enc", "utf-8")};
        c.header = row;
        all.push_back(c);
    }
    {  // footer: checkbox, spacer, ScrollTail field, fixed center
        ChromeComposition c;
        RowDescriptor row;
        WidgetDescriptor wrap;
        wrap.kind = WidgetKind::Checkbox;
        wrap.id = "wrap";
        wrap.value = literal("wrap");
        wrap.checked = literal("true");
        WidgetDescriptor sp;
        sp.kind = WidgetKind::Spacer;
        sp.id = "sp";
        sp.width = 3;
        WidgetDescriptor msg;
        msg.kind = WidgetKind::Field;
        msg.id = "msg";
        msg.value = literal("a very long status message that scrolls");
        msg.overflow = Overflow::ScrollTail;
        msg.sigil = ">";
        row.left = {wrap, sp};
        row.center = label("mode", "NORMAL");
        row.right = {msg};
        row.centerWidth = CenterWidth::Fixed;
        row.centerFixed = 10;
        row.separator = 2;
        c.footer = row;
        all.push_back(c);
    }
    {  // both regions, flex center
        ChromeComposition c;
        RowDescriptor header;
        header.left = {label("a", "alpha")};
        header.center = label("t", "title");
        header.right = {label("b", "beta")};
        RowDescriptor footer;
        footer.left = {label("c", "gamma")};
        c.header = header;
        c.footer = footer;
        all.push_back(c);
    }
    {  // empty (no regions)
        all.push_back(ChromeComposition{});
    }
    return all;
}

// A resolver returning a value for known provider ids, so provider widgets are
// actually exercised through BOTH lowering paths identically.
ChromeProviderResolver providers() {
    return [](std::string_view id) -> std::optional<ResolvedProvider> {
        if (id == "file.path") return ResolvedProvider{"src/main.cpp", "path", std::nullopt};
        if (id == "git.branch") return ResolvedProvider{"main", "branch", std::nullopt};
        return std::nullopt;
    };
}

std::vector<AccessibilityNode> lowerLegacy(const RowDescriptor& row, int width) {
    std::vector<AccessibilityNode> out;
    lowerChromeRow(row, Rect{0, 0, width, 1}, ShellNodeKind::HeaderField,
                   SemanticRole::Header, Style{}, providers(), out);
    return out;
}

std::vector<AccessibilityNode> lowerTree(const RowDescriptor& row, int width) {
    const UiRegion region = uiChromeRegionFromRow(row, RegionRole::Top);
    std::vector<AccessibilityNode> out;
    const auto result =
        lowerUiChromeRegion(region, Rect{0, 0, width, 1},
                            ShellNodeKind::HeaderField, SemanticRole::Header,
                            Style{}, providers(), out);
    ASSERT_TRUE(result.ok());
    return out;
}

// THE proof: lowering the tree directly is byte-for-byte identical to lowering
// the legacy row, across the corpus and across several widths (so rank-collapse,
// right truncation, and center clamping are all exercised).
TEST(loweringTheTreeMatchesLegacyByteForByte) {
    for (const auto& c : corpus()) {
        for (const int width : {80, 40, 24, 12, 6}) {
            if (c.header) {
                ASSERT_TRUE(lowerTree(*c.header, width) ==
                            lowerLegacy(*c.header, width));
            }
            if (c.footer) {
                ASSERT_TRUE(lowerTree(*c.footer, width) ==
                            lowerLegacy(*c.footer, width));
            }
        }
    }
}

// The built schema is well-formed (unique node ids etc.).
TEST(bridgeProducesAValidSchema) {
    for (const auto& c : corpus()) {
        const auto schema = uiSchemaFromChrome(c, Generation{1});
        ASSERT_TRUE(ssg::validateUiSchema(schema).ok());
    }
}

// A malformed region tree fails loud with a named error and emits nothing.
TEST(malformedRegionFailsLoud) {
    auto lowerBad = [](const UiRegion& bad) {
        std::vector<AccessibilityNode> out;
        const auto result = lowerUiChromeRegion(
            bad, Rect{0, 0, 40, 1}, ShellNodeKind::HeaderField,
            SemanticRole::Header, Style{},
            [](std::string_view) { return std::optional<ResolvedProvider>{}; },
            out);
        return std::pair{result.ok(), out.size()};
    };

    // A root that is a leaf, not a container of three groups.
    WidgetDescriptor w = label("x", "x");
    UiRegion leafRoot{RegionRole::Top,
                      UiNode{UiNodeId{"top"}, ssg::Size::flex(), ssg::UiLeaf{w}}};
    auto [ok1, n1] = lowerBad(leafRoot);
    ASSERT_TRUE(!ok1);
    ASSERT_EQ(n1, std::size_t{0});

    // A canonical region whose end groups are Flex instead of Auto: the packing
    // sizing is wrong, so a generic client would render it differently -> reject.
    RowDescriptor row;
    row.left = {label("a", "alpha")};
    row.right = {label("b", "beta")};
    UiRegion wrongSize = uiChromeRegionFromRow(row, RegionRole::Top);
    auto& rootC = std::get<ssg::UiContainer>(wrongSize.root.content);
    rootC.children[0].size = ssg::Size::flex();  // left should be Auto
    auto [ok2, n2] = lowerBad(wrongSize);
    ASSERT_TRUE(!ok2);
    ASSERT_EQ(n2, std::size_t{0});
}

// A negative gap is unrepresentable: Gap::of throws, so it cannot reach a tree.
TEST(negativeGapIsUnrepresentable) {
    bool threw = false;
    try {
        (void)ssg::Gap::of(-1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

}  // namespace

int main() {
    RUN(loweringTheTreeMatchesLegacyByteForByte);
    RUN(bridgeProducesAValidSchema);
    RUN(malformedRegionFailsLoud);
    RUN(negativeGapIsUnrepresentable);
    return failed == 0 ? 0 : 1;
}
