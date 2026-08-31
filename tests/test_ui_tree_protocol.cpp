// Round-trip oracle for the UI-VM tree wire codec (spec phase 4: publish the
// medium-agnostic model on the channel). The knowable answer: decoding an
// encoded schema recovers it exactly, and a malformed value decodes to nullopt
// (never a partial). The corpus reuses the chrome bridge so the published shape
// is the real one.

#include "chrome_authoring.h"
#include "ssg/UiTree.h"
#include "ssg/UiTreeProtocol.h"
#include "ssg/WholeScreenAssembly.h"
#include "test_helpers.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using ssg::decodeUiSchema;
using ssg::encodeUiSchema;
using ssg::Generation;
using ssg::ProtocolValue;
using ssg::ScrollAxis;
using ssg::UiNode;
using ssg::UiSchema;
using ssg::ValueSource;
using ssg::WidgetDescriptor;
using ssg::WidgetKind;

WidgetDescriptor label(std::string id, std::string text) {
    WidgetDescriptor w;
    w.kind = WidgetKind::Label;
    w.id = std::move(id);
    ValueSource v;
    v.literal = std::move(text);
    w.value = v;
    return w;
}

std::vector<UiSchema> corpus() {
    std::vector<UiSchema> all;

    {  // header + footer, providers, checkbox, spacer, overflow, center, ranks
        WidgetDescriptor path;
        path.kind = WidgetKind::Field;
        path.id = "path";
        ValueSource p;
        p.isProvider = true;
        p.provider = "file.path";
        path.value = p;
        path.command = "file.reveal";
        path.rank = 2;
        path.keep = true;
        WidgetDescriptor dirty;
        dirty.kind = WidgetKind::Checkbox;
        dirty.id = "dirty";
        dirty.value = ValueSource{false, "modified", ""};
        dirty.checked = ValueSource{false, "true", ""};
        dirty.role = "status_warning";
        WidgetDescriptor sp;
        sp.kind = WidgetKind::Spacer;
        sp.id = "sp";
        sp.width = 3;
        WidgetDescriptor msg;
        msg.kind = WidgetKind::Field;
        msg.id = "msg";
        msg.value = ValueSource{false, "scrolling status", ""};
        msg.overflow = ssg::Overflow::ScrollTail;
        msg.sigil = ">";
        const auto comp = ssgtest::composeHeaderAndFooterValidated(
            {path, dirty, sp}, {msg}, {label("enc", "utf-8")});
        all.push_back(UiSchema{
            Generation{7},
            ssg::assembleWholeScreen({}, "help.open", ssg::StyleDimensions{},
                                     ssg::Style{}.inputLineSigil, comp)
                .root});
    }
    {  // the built-in whole-screen tree without a composed chrome override
        all.push_back(UiSchema{
            Generation{1},
            ssg::assembleWholeScreen({}, "help.open", ssg::StyleDimensions{},
                                     ssg::Style{}.inputLineSigil,
                                     std::nullopt)
                .root});
    }
    return all;
}

// The core property: encode then decode recovers the schema exactly.
TEST(uiSchemaRoundTripsThroughTheWire) {
    for (const auto& schema : corpus()) {
        const ProtocolValue encoded = encodeUiSchema(schema);
        const auto decoded = decodeUiSchema(encoded);
        ASSERT_TRUE(decoded.has_value());
        if (decoded) ASSERT_TRUE(*decoded == schema);
    }
}

TEST(focusHostContextRoundTripsThroughTheWire) {
    const auto schema = corpus().back();
    const auto decoded = decodeUiSchema(encodeUiSchema(schema));
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(decoded->root.focusContext == std::nullopt);

    const std::array expected{
        std::pair{ssg::kEditorNodeId, ssg::FocusTarget::Editor},
        std::pair{ssg::kPanelNodeId, ssg::FocusTarget::Panel},
        std::pair{ssg::kHeaderPromptInputNodeId, ssg::FocusTarget::Prompt},
        std::pair{ssg::kFooterPromptNodeId, ssg::FocusTarget::Prompt},
        std::pair{ssg::kExternalModNodeId,
                  ssg::FocusTarget::ExternalModification},
    };
    for (const auto& [id, context] : expected) {
        const UiNode* node =
            ssg::findUiNode(*decoded, ssg::UiNodeId{std::string{id}});
        ASSERT_TRUE(node != nullptr);
        ASSERT_TRUE(node->focusContext == context);
    }

    std::size_t declared = 0;
    const auto countDeclared = [&](const auto& self, const UiNode& node) -> void {
        if (node.focusContext) ++declared;
        if (const auto* container =
                std::get_if<ssg::UiContainer>(&node.content)) {
            for (const auto& child : container->children) self(self, child);
        }
    };
    countDeclared(countDeclared, decoded->root);
    ASSERT_EQ(declared, expected.size());
}

TEST(containerAccessibilityLabelRoundTripsThroughTheWire) {
    for (const std::string label : {"Find and replace", ""}) {
        auto schema = corpus().back();
        auto* prompt = const_cast<UiNode*>(ssg::findUiNode(
            schema, ssg::UiNodeId{std::string{ssg::kFooterPromptNodeId}}));
        ASSERT_TRUE(prompt != nullptr);
        prompt->accessibleLabel = label;
        const auto decoded = decodeUiSchema(encodeUiSchema(schema));
        ASSERT_TRUE(decoded.has_value());
        const UiNode* roundTripped = decoded
            ? ssg::findUiNode(
                  *decoded,
                  ssg::UiNodeId{std::string{ssg::kFooterPromptNodeId}})
            : nullptr;
        ASSERT_TRUE(roundTripped != nullptr);
        ASSERT_EQ(roundTripped->accessibleLabel,
                  std::optional<std::string>{label});
    }
}

TEST(precedingCanonicalTopologyDecodesToTheCurrentArrangement) {
    UiSchema current{
        Generation{1},
        ssg::assembleWholeScreen({}, "help.open", ssg::StyleDimensions{},
                                 ssg::Style{}.inputLineSigil, std::nullopt)
            .root};
    UiSchema preceding = current;
    auto& root = std::get<ssg::UiContainer>(preceding.root.content);
    UiNode header = std::move(root.children[0]);
    UiNode body = std::move(root.children[1]);
    UiNode footerPrompt = std::move(root.children[2]);
    UiNode footer = std::move(root.children[3]);
    auto& bodyContainer = std::get<ssg::UiContainer>(body.content);
    auto& panel =
        std::get<ssg::UiContainer>(bodyContainer.children[0].content);
    const UiNode genericTree = panel.children.front();
    constexpr auto retiredFileTree = static_cast<ssg::ViewSurface>(1);
    constexpr auto retiredGitStatus = static_cast<ssg::ViewSurface>(2);
    constexpr auto retiredSymbols = static_cast<ssg::ViewSurface>(4);
    const auto legacyTree = [&](std::string_view id, ssg::ViewSurface surface) {
        UiNode node = genericTree;
        node.id = ssg::UiNodeId{std::string{id}};
        std::get<ssg::UiLeaf>(node.content).widget.surface = surface;
        return node;
    };
    panel.children = {
        legacyTree(ssg::kFileTreeNodeId, retiredFileTree),
        legacyTree(ssg::kGitStatusNodeId, retiredGitStatus),
        legacyTree(ssg::kSymbolsNodeId, retiredSymbols)};
    bodyContainer.children[0].size =
        ssg::Size::exact(ssg::StyleDimensions{}.panelTargetWidth);
    bodyContainer.children[1].size = ssg::Size::flex();
    auto& content =
        std::get<ssg::UiContainer>(bodyContainer.children[1].content);
    UiNode tabBar = std::move(content.children[0]);
    UiNode notice = std::move(content.children[1]);
    UiNode externalModification = std::move(content.children[2]);
    UiNode editor = std::move(content.children[3]);
    UiNode findResults = std::move(content.children[4]);
    auto& editorContainer = std::get<ssg::UiContainer>(editor.content);
    editorContainer.children.insert(editorContainer.children.begin(),
                                    std::move(tabBar));
    content.children = {std::move(editor), std::move(findResults)};
    root.children = {std::move(header), std::move(notice),
                     std::move(externalModification), std::move(body),
                     std::move(footerPrompt), std::move(footer)};

    const auto decoded = decodeUiSchema(encodeUiSchema(preceding));
    ASSERT_TRUE(decoded.has_value());
    if (decoded) ASSERT_EQ(*decoded, current);

    UiSchema precedingTree = current;
    auto& currentRoot = std::get<ssg::UiContainer>(precedingTree.root.content);
    auto& currentBody = std::get<ssg::UiContainer>(
        currentRoot.children[1].content);
    auto& currentPanel =
        std::get<ssg::UiContainer>(currentBody.children[0].content);
    const UiNode currentGeneric = currentPanel.children.front();
    const auto precedingLeaf =
        [&](std::string_view id, ssg::ViewSurface surface) {
            UiNode node = currentGeneric;
            node.id = ssg::UiNodeId{std::string{id}};
            std::get<ssg::UiLeaf>(node.content).widget.surface = surface;
            return node;
        };
    currentPanel.children = {
        precedingLeaf(ssg::kFileTreeNodeId, retiredFileTree),
        precedingLeaf(ssg::kGitStatusNodeId, retiredGitStatus),
        precedingLeaf(ssg::kSymbolsNodeId, retiredSymbols)};
    const auto treeDecoded = decodeUiSchema(encodeUiSchema(precedingTree));
    ASSERT_TRUE(treeDecoded.has_value());
    if (treeDecoded) ASSERT_EQ(*treeDecoded, current);

    const auto panelChildren = [](UiSchema& candidate)
        -> std::vector<UiNode>& {
        auto& candidateRoot =
            std::get<ssg::UiContainer>(candidate.root.content);
        auto& candidateBody =
            std::get<ssg::UiContainer>(candidateRoot.children[1].content);
        return std::get<ssg::UiContainer>(
                   candidateBody.children[0].content)
            .children;
    };
    UiSchema extraTreeChild = precedingTree;
    panelChildren(extraTreeChild).push_back(
        panelChildren(extraTreeChild).front());
    ASSERT_FALSE(decodeUiSchema(encodeUiSchema(extraTreeChild)).has_value());

    UiSchema missingTreeChild = precedingTree;
    panelChildren(missingTreeChild).pop_back();
    ASSERT_FALSE(decodeUiSchema(encodeUiSchema(missingTreeChild)).has_value());

    UiSchema reorderedTreeChildren = precedingTree;
    std::swap(panelChildren(reorderedTreeChildren)[0],
              panelChildren(reorderedTreeChildren)[1]);
    ASSERT_FALSE(
        decodeUiSchema(encodeUiSchema(reorderedTreeChildren)).has_value());

    UiSchema wrongTreeSurface = precedingTree;
    std::get<ssg::UiLeaf>(panelChildren(wrongTreeSurface)[0].content)
        .widget.surface = ssg::ViewSurface::Tree;
    ASSERT_FALSE(decodeUiSchema(encodeUiSchema(wrongTreeSurface)).has_value());

    UiSchema malformedInterior = preceding;
    auto& malformedRoot =
        std::get<ssg::UiContainer>(malformedInterior.root.content);
    malformedRoot.children[1].content =
        ssg::UiContainer{ssg::Axis::Column, {}, {}, {}};
    ASSERT_FALSE(
        decodeUiSchema(encodeUiSchema(malformedInterior)).has_value());

    UiSchema wrongLegacySize = preceding;
    auto& wrongRoot =
        std::get<ssg::UiContainer>(wrongLegacySize.root.content);
    auto& wrongBody =
        std::get<ssg::UiContainer>(wrongRoot.children[3].content);
    wrongBody.children[0].size =
        ssg::Size::exact(ssg::StyleDimensions{}.panelTargetWidth + 1);
    ASSERT_FALSE(
        decodeUiSchema(encodeUiSchema(wrongLegacySize)).has_value());

    auto& precedingRoot =
        std::get<ssg::UiContainer>(preceding.root.content);
    std::swap(precedingRoot.children[1], precedingRoot.children[2]);
    ASSERT_FALSE(decodeUiSchema(encodeUiSchema(preceding)).has_value());
}

TEST(precedingFooterPromptSurfaceDecodesOnlyInItsExactCanonicalShape) {
    UiSchema current{
        Generation{1},
        ssg::assembleWholeScreen({}, "help.open", ssg::StyleDimensions{},
                                 ssg::Style{}.inputLineSigil, std::nullopt)
            .root};
    auto preceding = current;
    auto& root = std::get<ssg::UiContainer>(preceding.root.content);
    WidgetDescriptor retired;
    retired.kind = WidgetKind::View;
    retired.id = std::string{ssg::kFooterPromptNodeId};
    retired.surface = static_cast<ssg::ViewSurface>(5);
    root.children[2] =
        UiNode{ssg::UiNodeId{std::string{ssg::kFooterPromptNodeId}},
               ssg::Size::autoSize(), ssg::UiLeaf{retired},
               root.children[2].style};

    ASSERT_EQ(decodeUiSchema(encodeUiSchema(preceding)),
              std::optional<UiSchema>{current});

    auto wrongId = preceding;
    std::get<ssg::UiContainer>(wrongId.root.content).children[2].id =
        ssg::UiNodeId{"other"};
    ASSERT_FALSE(decodeUiSchema(encodeUiSchema(wrongId)).has_value());

    auto wrongSize = preceding;
    std::get<ssg::UiContainer>(wrongSize.root.content).children[2].size =
        ssg::Size::exact(1);
    ASSERT_FALSE(decodeUiSchema(encodeUiSchema(wrongSize)).has_value());

    auto declaredFocus = preceding;
    std::get<ssg::UiContainer>(declaredFocus.root.content)
        .children[2]
        .focusContext = ssg::FocusTarget::Prompt;
    ASSERT_FALSE(decodeUiSchema(encodeUiSchema(declaredFocus)).has_value());

    auto stray = current;
    auto& strayRoot = std::get<ssg::UiContainer>(stray.root.content);
    auto& tree = std::get<ssg::UiContainer>(
        std::get<ssg::UiContainer>(strayRoot.children[1].content)
            .children[0]
            .content)
                     .children[0];
    std::get<ssg::UiLeaf>(tree.content).widget.surface =
        static_cast<ssg::ViewSurface>(5);
    ASSERT_FALSE(decodeUiSchema(encodeUiSchema(stray)).has_value());
}

ProtocolValue withRootField(const ProtocolValue& encoded, std::string key,
                            ProtocolValue replacement) {
    ProtocolValue::Object top;
    for (const auto& [topKey, topValue] : *encoded.asObject()) {
        if (topKey != "root") {
            top.emplace_back(topKey, topValue);
            continue;
        }
        ProtocolValue::Object root;
        for (const auto& [nodeKey, nodeValue] : *topValue.asObject()) {
            if (nodeKey != key) root.emplace_back(nodeKey, nodeValue);
        }
        root.emplace_back(std::move(key), std::move(replacement));
        top.emplace_back(topKey, ProtocolValue::makeObject(std::move(root)));
    }
    return ProtocolValue::makeObject(std::move(top));
}

TEST(nodeStyleRoundTripsAndFieldAdditionRemainsCompatible) {
    UiSchema schema = corpus().back();
    schema.root.style.foreground = ssg::SemanticRole::Text;
    schema.root.style.background = ssg::SemanticRole::Canvas;

    const ProtocolValue encoded = encodeUiSchema(schema);
    const auto decoded = decodeUiSchema(encoded);
    ASSERT_TRUE(decoded.has_value());
    if (decoded) ASSERT_TRUE(*decoded == schema);

    const ProtocolValue future =
        withRootField(encoded, "future_node_property",
                      ProtocolValue::makeText("ignored"));
    const auto futureDecoded = decodeUiSchema(future);
    ASSERT_TRUE(futureDecoded.has_value());
    if (futureDecoded) ASSERT_TRUE(*futureDecoded == schema);

    UiSchema unstyled = schema;
    unstyled.root.style = {};
    const ProtocolValue unstyledEncoded = encodeUiSchema(unstyled);
    ASSERT_TRUE(unstyledEncoded.field("root")->field("style") == nullptr);
    ASSERT_TRUE(decodeUiSchema(unstyledEncoded) == unstyled);
}

TEST(responsiveSizeRoundTripsAndMalformedFormsAreRejected) {
    UiSchema schema = corpus().back();
    schema.generation = Generation{9};
    schema.root.size = ssg::Size::optionalPreferred(24, 12);
    const auto encoded = encodeUiSchema(schema);
    const auto* size = encoded.field("root")->field("size");
    ASSERT_TRUE(size != nullptr);
    if (!size) return;
    ASSERT_EQ(size->field("kind")->asUint(),
              std::optional<std::uint64_t>{
                  static_cast<std::uint64_t>(ssg::SizeKind::Responsive)});
    ASSERT_EQ(size->field("extent")->asUint(),
              std::optional<std::uint64_t>{24});
    ASSERT_EQ(size->field("minimum")->asUint(),
              std::optional<std::uint64_t>{12});
    ASSERT_EQ(size->field("growth")->asUint(),
              std::optional<std::uint64_t>{0});
    ASSERT_EQ(size->field("optional")->asBool(), std::optional<bool>{true});
    ASSERT_EQ(decodeUiSchema(encoded), std::optional<UiSchema>{schema});

    UiSchema legacy = schema;
    legacy.root.size = ssg::Size::exact(24);
    const auto encodedLegacy = encodeUiSchema(legacy);
    const auto* legacySize = encodedLegacy.field("root")->field("size");
    ASSERT_TRUE(legacySize != nullptr);
    if (legacySize) {
        ASSERT_EQ(legacySize->asObject()->size(), std::size_t{2});
        ASSERT_TRUE(legacySize->field("minimum") == nullptr);
        ASSERT_TRUE(legacySize->field("growth") == nullptr);
        ASSERT_TRUE(legacySize->field("optional") == nullptr);
    }

    const auto malformed = ProtocolValue::makeObject({
        {"kind", ProtocolValue::makeUint(
                     static_cast<std::uint64_t>(
                         ssg::SizeKind::Responsive))},
        {"extent", ProtocolValue::makeUint(24)},
        {"minimum", ProtocolValue::makeUint(12)},
        {"growth", ProtocolValue::makeUint(1)},
        {"optional", ProtocolValue::makeBool(true)},
    });
    ASSERT_FALSE(
        decodeUiSchema(withRootField(encoded, "size", malformed)).has_value());
    ASSERT_FALSE(decodeUiSchema(withRootField(
                                   encoded, "size",
                                   ProtocolValue::makeObject({
                                       {"kind", ProtocolValue::makeUint(255)},
                                       {"extent", ProtocolValue::makeUint(0)},
                                   })))
                     .has_value());
}

TEST(malformedOrUnknownNodeStyleRejectsTheWholeSchema) {
    UiSchema schema = corpus().back();
    const ProtocolValue encoded = encodeUiSchema(schema);
    const auto style = [](ProtocolValue foreground) {
        return ProtocolValue::makeObject(
            {{"foreground", std::move(foreground)}});
    };

    ASSERT_FALSE(decodeUiSchema(
        withRootField(encoded, "style",
                      style(ProtocolValue::makeText("text"))))
                     .has_value());
    ASSERT_FALSE(decodeUiSchema(
        withRootField(encoded, "style",
                      style(ProtocolValue::makeUint(255))))
                     .has_value());
}

// The scroll viewport property round-trips (proven by the corpus above, whose
// assembled tree carries Vertical on panel+content), and a wire value the client
// does not recognize -- an absent field or a future axis -- decodes to None so an
// old client degrades a new axis to "not a viewport" rather than rejecting the
// frame. Absent->None is already exercised by every non-viewport node in the
// corpus round-trip; this pins the UNKNOWN->None degrade on a real published tree.
// Read the ScrollAxis of a node's container (None for a leaf, since only a
// container can be a viewport).
ScrollAxis containerScroll(const UiNode& node) {
    const auto* c = std::get_if<ssg::UiContainer>(&node.content);
    return c ? c->scroll : ScrollAxis::None;
}

const UiNode* findNode(const UiNode& node, std::string_view id) {
    if (node.id.value() == id) return &node;
    if (const auto* c = std::get_if<ssg::UiContainer>(&node.content)) {
        for (const auto& child : c->children) {
            if (const auto* found = findNode(child, id)) return found;
        }
    }
    return nullptr;
}

// Rebuild one wire node, setting (or dropping) the `scroll` field INSIDE the
// container object of the node whose id matches, recursing through children.
// `scrollValue` absent drops the field; present sets it (a uint for a valid/
// unknown ordinal, or a non-uint to exercise the malformed-type rejection).
ProtocolValue rewriteScroll(const ProtocolValue& node, std::string_view targetId,
                            std::optional<ProtocolValue> scrollValue) {
    std::string thisId;
    for (const auto& [k, v] : *node.asObject()) {
        if (k == "id") { if (const auto* t = v.asText()) thisId = *t; }
    }
    const bool isTarget = thisId == targetId;
    ProtocolValue::Object out;
    for (const auto& [k, v] : *node.asObject()) {
        if (k == "container") {
            ProtocolValue::Object cont;
            for (const auto& [ck, cv] : *v.asObject()) {
                if (ck == "scroll" && isTarget) continue;  // dropped; re-added below
                if (ck == "children") {
                    ProtocolValue::Array kids;
                    for (const auto& child : *cv.asArray()) {
                        kids.push_back(rewriteScroll(child, targetId, scrollValue));
                    }
                    cont.emplace_back(ck, ProtocolValue::makeArray(std::move(kids)));
                } else {
                    cont.emplace_back(ck, cv);
                }
            }
            if (isTarget && scrollValue) {
                cont.emplace_back("scroll", *scrollValue);
            }
            out.emplace_back(k, ProtocolValue::makeObject(std::move(cont)));
        } else {
            out.emplace_back(k, v);
        }
    }
    return ProtocolValue::makeObject(std::move(out));
}

TEST(scrollAxisRoundTripsAndCanonicalViewportRejectsUnknownOrAbsentValues) {
    const UiSchema schema{
        Generation{1},
        ssg::assembleWholeScreen({}, "help.open", ssg::StyleDimensions{},
                                 ssg::Style{}.inputLineSigil, std::nullopt)
            .root};
    const ProtocolValue encoded = encodeUiSchema(schema);

    const auto direct = decodeUiSchema(encoded);
    ASSERT_TRUE(direct.has_value());
    if (direct) {
        const UiNode* viewport =
            findNode(direct->root, ssg::kDocumentViewportNodeId);
        ASSERT_TRUE(viewport != nullptr);
        if (viewport)
            ASSERT_TRUE(containerScroll(*viewport) == ScrollAxis::Vertical);
    }

    const auto rebuild = [&](std::optional<ProtocolValue> scroll) {
        ProtocolValue::Object top;
        for (const auto& [key, value] : *encoded.asObject()) {
            if (key == "root") {
                top.emplace_back(
                    key, rewriteScroll(value, ssg::kDocumentViewportNodeId,
                                       std::move(scroll)));
            } else {
                top.emplace_back(key, value);
            }
        }
        return decodeUiSchema(ProtocolValue::makeObject(std::move(top)));
    };

    const auto absent = rebuild(std::nullopt);
    ASSERT_FALSE(absent.has_value());

    const auto unknown = rebuild(ProtocolValue::makeUint(99));
    ASSERT_FALSE(unknown.has_value());

    // A PRESENT scroll field of the wrong TYPE (not a uint) is malformed: it must
    // fail the decode, never silently degrade to None.
    const auto malformed = rebuild(ProtocolValue::makeText("vertical"));
    ASSERT_TRUE(!malformed.has_value());
}

// A malformed value decodes to nullopt, never a partial schema, and never throws
// on an out-of-range or unknown-enum wire value.
TEST(malformedWireDecodesToNullopt) {
    ASSERT_TRUE(!decodeUiSchema(ProtocolValue::makeUint(5)).has_value());
    ASSERT_TRUE(!decodeUiSchema(ProtocolValue::makeObject({})).has_value());
    // A schema whose root is not an object.
    const ProtocolValue badEntry = ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(1)},
         {"root", ProtocolValue::makeUint(3)}});
    ASSERT_TRUE(!decodeUiSchema(badEntry).has_value());

    // A node with BOTH a container and a leaf is malformed (exactly one).
    const ProtocolValue leafObj = ProtocolValue::makeObject(
        {{"kind", ProtocolValue::makeUint(1)},
         {"id", ProtocolValue::makeText("x")},
         {"value", ProtocolValue::makeNull()},
         {"checked", ProtocolValue::makeNull()},
         {"width", ProtocolValue::makeNull()},
         {"role", ProtocolValue::makeNull()},
         {"command", ProtocolValue::makeNull()},
         {"rank", ProtocolValue::makeInt(0)},
         {"keep", ProtocolValue::makeBool(false)},
         {"overflow", ProtocolValue::makeUint(0)},
         {"sigil", ProtocolValue::makeText("")}});
    const ProtocolValue containerObj = ProtocolValue::makeObject(
        {{"axis", ProtocolValue::makeUint(0)},
         {"inset", ProtocolValue::makeObject(
                       {{"left", ProtocolValue::makeUint(0)},
                        {"right", ProtocolValue::makeUint(0)},
                        {"top", ProtocolValue::makeUint(0)},
                        {"bottom", ProtocolValue::makeUint(0)}})},
         {"gap", ProtocolValue::makeUint(0)},
         {"children", ProtocolValue::makeArray({})}});
    const ProtocolValue bothNode = ProtocolValue::makeObject(
        {{"id", ProtocolValue::makeText("n")},
         {"size", ProtocolValue::makeObject(
                      {{"kind", ProtocolValue::makeUint(1)},
                       {"extent", ProtocolValue::makeUint(0)}})},
         {"container", containerObj},
         {"leaf", leafObj}});
    const ProtocolValue bothSchema = ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(1)}, {"root", bothNode}});
    ASSERT_TRUE(!decodeUiSchema(bothSchema).has_value());
}

// An out-of-range surface ordinal on a leaf decodes to nullopt, never a guess.
TEST(malformedSurfaceDecodesToNullopt) {
    const ProtocolValue leafObj = ProtocolValue::makeObject(
        {{"kind", ProtocolValue::makeUint(6)},  // View
         {"id", ProtocolValue::makeText("v")},
         {"value", ProtocolValue::makeNull()},
         {"checked", ProtocolValue::makeNull()},
         {"width", ProtocolValue::makeNull()},
         {"role", ProtocolValue::makeNull()},
         {"command", ProtocolValue::makeNull()},
         {"surface", ProtocolValue::makeUint(99)},  // no such surface
         {"rank", ProtocolValue::makeInt(0)},
         {"keep", ProtocolValue::makeBool(false)},
         {"overflow", ProtocolValue::makeUint(0)},
         {"sigil", ProtocolValue::makeText("")}});
    const ProtocolValue node = ProtocolValue::makeObject(
        {{"id", ProtocolValue::makeText("v")},
         {"size", ProtocolValue::makeObject(
                      {{"kind", ProtocolValue::makeUint(1)},
                       {"extent", ProtocolValue::makeUint(0)}})},
         {"leaf", leafObj}});
    const ProtocolValue schema = ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(1)}, {"root", node}});
    ASSERT_TRUE(!decodeUiSchema(schema).has_value());
}

}  // namespace

int main() {
    RUN(uiSchemaRoundTripsThroughTheWire);
    RUN(focusHostContextRoundTripsThroughTheWire);
    RUN(containerAccessibilityLabelRoundTripsThroughTheWire);
    RUN(precedingCanonicalTopologyDecodesToTheCurrentArrangement);
    RUN(precedingFooterPromptSurfaceDecodesOnlyInItsExactCanonicalShape);
    RUN(nodeStyleRoundTripsAndFieldAdditionRemainsCompatible);
    RUN(responsiveSizeRoundTripsAndMalformedFormsAreRejected);
    RUN(malformedOrUnknownNodeStyleRejectsTheWholeSchema);
    RUN(scrollAxisRoundTripsAndCanonicalViewportRejectsUnknownOrAbsentValues);
    RUN(malformedWireDecodesToNullopt);
    RUN(malformedSurfaceDecodesToNullopt);
    return failed == 0 ? 0 : 1;
}
