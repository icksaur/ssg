// Round-trip oracle for the UI-VM tree wire codec (spec phase 4: publish the
// medium-agnostic model on the channel). The knowable answer: decoding an
// encoded schema recovers it exactly, and a malformed value decodes to nullopt
// (never a partial). The corpus reuses the chrome bridge so the published shape
// is the real one.

#include "ssg/UiChromeBridge.h"
#include "ssg/UiTree.h"
#include "ssg/UiTreeProtocol.h"
#include "test_helpers.h"

#include <optional>
#include <string>
#include <vector>

namespace {

using ssg::ChromeComposition;
using ssg::decodeUiSchema;
using ssg::encodeUiSchema;
using ssg::Generation;
using ssg::ProtocolValue;
using ssg::RowDescriptor;
using ssg::uiSchemaFromChrome;
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
        ChromeComposition c;
        RowDescriptor header;
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
        header.left = {path, dirty, sp};
        header.center = label("title", "SSG");
        header.right = {label("enc", "utf-8")};
        header.centerWidth = ssg::CenterWidth::Fixed;
        header.centerFixed = 8;
        header.separator = 2;
        WidgetDescriptor msg;
        msg.kind = WidgetKind::Field;
        msg.id = "msg";
        msg.value = ValueSource{false, "scrolling status", ""};
        msg.overflow = ssg::Overflow::ScrollTail;
        msg.sigil = ">";
        RowDescriptor footer;
        footer.left = {msg};
        c.header = header;
        c.footer = footer;
        all.push_back(uiSchemaFromChrome(c, Generation{7}));
    }
    {  // empty composition -> empty schema
        all.push_back(uiSchemaFromChrome(ChromeComposition{}, Generation{1}));
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
    // A regions array whose entry is not an object.
    const ProtocolValue badEntry = ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(1)},
         {"regions", ProtocolValue::makeArray({ProtocolValue::makeUint(3)})}});
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
        {{"generation", ProtocolValue::makeUint(1)},
         {"regions", ProtocolValue::makeArray({ProtocolValue::makeObject(
             {{"role", ProtocolValue::makeUint(0)}, {"root", bothNode}})})}});
    ASSERT_TRUE(!decodeUiSchema(bothSchema).has_value());
}

}  // namespace

int main() {
    RUN(uiSchemaRoundTripsThroughTheWire);
    RUN(malformedWireDecodesToNullopt);
    return failed == 0 ? 0 : 1;
}
