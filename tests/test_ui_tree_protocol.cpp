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
        ASSERT_TRUE(*decoded == schema);
    }
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

TEST(scrollAxisRoundTripsAndUnknownOrAbsentDecodesToNone) {
    const UiSchema schema{
        Generation{1},
        ssg::assembleWholeScreen({}, "help.open", ssg::StyleDimensions{},
                                 ssg::Style{}.inputLineSigil, std::nullopt)
            .root};
    const ProtocolValue encoded = encodeUiSchema(schema);

    const auto direct = decodeUiSchema(encoded);
    ASSERT_TRUE(direct.has_value());
    if (direct) {
        const UiNode* content = findNode(direct->root, ssg::kContentNodeId);
        ASSERT_TRUE(content != nullptr);
        if (content) ASSERT_TRUE(containerScroll(*content) == ScrollAxis::Vertical);
    }

    const auto rebuild = [&](std::optional<ProtocolValue> scroll) {
        ProtocolValue::Object top;
        for (const auto& [key, value] : *encoded.asObject()) {
            if (key == "root") {
                top.emplace_back(
                    key, rewriteScroll(value, ssg::kContentNodeId, std::move(scroll)));
            } else {
                top.emplace_back(key, value);
            }
        }
        return decodeUiSchema(ProtocolValue::makeObject(std::move(top)));
    };

    const auto absent = rebuild(std::nullopt);
    ASSERT_TRUE(absent.has_value());
    if (absent) {
        const UiNode* content = findNode(absent->root, ssg::kContentNodeId);
        if (content) ASSERT_TRUE(containerScroll(*content) == ScrollAxis::None);
    }

    const auto unknown = rebuild(ProtocolValue::makeUint(99));
    ASSERT_TRUE(unknown.has_value());
    if (unknown) {
        const UiNode* content = findNode(unknown->root, ssg::kContentNodeId);
        if (content) ASSERT_TRUE(containerScroll(*content) == ScrollAxis::None);
    }

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
    RUN(scrollAxisRoundTripsAndUnknownOrAbsentDecodesToNone);
    RUN(malformedWireDecodesToNullopt);
    RUN(malformedSurfaceDecodesToNullopt);
    return failed == 0 ? 0 : 1;
}
