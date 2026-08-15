// Round-trip oracle for the UI-VM tree wire codec (spec phase 4: publish the
// medium-agnostic model on the channel). The knowable answer: decoding an
// encoded schema recovers it exactly, and a malformed value decodes to nullopt
// (never a partial). The corpus reuses the chrome bridge so the published shape
// is the real one.

#include "chrome_authoring.h"
#include "ssg/UiTree.h"
#include "ssg/UiTreeProtocol.h"
#include "test_helpers.h"

#include <optional>
#include <string>
#include <vector>

namespace {

using ssg::decodeUiSchema;
using ssg::encodeUiSchema;
using ssg::Generation;
using ssg::ProtocolValue;
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
        const auto comp = ssgtest::composeHeaderAndFooter(
            {path, dirty, sp}, {msg}, {label("enc", "utf-8")}, label("title", "SSG"),
            ssg::CenterWidth::Fixed, 8, /*headerSeparator=*/2);
        all.push_back(UiSchema{Generation{7}, comp.root});
    }
    {  // empty composition -> empty schema (a valid empty root)
        all.push_back(UiSchema{
            Generation{1},
            ssg::UiNode{ssg::UiNodeId{"root"}, ssg::Size::flex(),
                        ssg::UiContainer{ssg::Axis::Column, {}, {}, {}}}});
    }
    {  // a View leaf carries its surface across the wire
        WidgetDescriptor view;
        view.kind = WidgetKind::View;
        view.id = "content.tabview";
        view.surface = ssg::ViewSurface::TabView;
        ssg::UiContainer body;
        body.axis = ssg::Axis::Row;
        body.children.push_back(
            ssg::UiNode{ssg::UiNodeId{"content.tabview"}, ssg::Size::flex(),
                        ssg::UiLeaf{view}});
        ssg::UiNode root{ssg::UiNodeId{"body"}, ssg::Size::flex(),
                         std::move(body)};
        all.push_back(UiSchema{Generation{3}, std::move(root)});
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
    RUN(malformedWireDecodesToNullopt);
    RUN(malformedSurfaceDecodesToNullopt);
    return failed == 0 ? 0 : 1;
}
