#include <ssg/UiTreeProtocol.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace ssg {

namespace {

// --- scalar helpers -------------------------------------------------------

template <typename Enum>
ProtocolValue enumValue(Enum value) {
    return ProtocolValue::makeUint(static_cast<std::uint64_t>(
        static_cast<std::underlying_type_t<Enum>>(value)));
}

std::optional<std::uint64_t> uintField(const ProtocolValue& object,
                                       std::string_view key) {
    const ProtocolValue* field = object.field(key);
    if (!field) return std::nullopt;
    return field->asUint();
}

std::optional<std::string> textField(const ProtocolValue& object,
                                     std::string_view key) {
    const ProtocolValue* field = object.field(key);
    if (!field || !field->asText()) return std::nullopt;
    return *field->asText();
}

std::optional<bool> boolField(const ProtocolValue& object,
                              std::string_view key) {
    const ProtocolValue* field = object.field(key);
    if (!field) return std::nullopt;
    return field->asBool();
}

// A nonnegative wire integer as int, or nullopt if it is out of int range. The
// tree's dimension types (Size/Inset/Gap) THROW on a negative/oversized value, so
// the decoder must reject such a value rather than let the throw escape -- a
// malformed wire message decodes to nullopt, never an exception.
std::optional<int> boundedInt(std::optional<std::uint64_t> raw) {
    if (!raw) return std::nullopt;
    if (*raw > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(*raw);
}

// --- Size / Inset / Gap ---------------------------------------------------

ProtocolValue encodeSize(const Size& size) {
    return ProtocolValue::makeObject(
        {{"kind", enumValue(size.kind())},
         {"extent", ProtocolValue::makeUint(
                        static_cast<std::uint64_t>(size.extent()))}});
}

// Decode an enum value validated against a closed set of allowed enumerators,
// returning nullopt for an unknown value (never a corrupt enum in the model).
template <typename Enum, std::size_t N>
std::optional<Enum> decodeEnumIn(std::optional<std::uint64_t> raw,
                                 const std::array<Enum, N>& allowed) {
    if (!raw) return std::nullopt;
    for (const Enum candidate : allowed) {
        if (static_cast<std::uint64_t>(
                static_cast<std::underlying_type_t<Enum>>(candidate)) == *raw) {
            return candidate;
        }
    }
    return std::nullopt;
}

constexpr std::array kAllAxes{Axis::Row, Axis::Column};
constexpr std::array kAllSizeKinds{SizeKind::Exact, SizeKind::Flex,
                                   SizeKind::Auto};
constexpr std::array kAllOverflows{Overflow::None, Overflow::Truncate,
                                   Overflow::ScrollTail};

std::optional<Size> decodeSize(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const auto kind = decodeEnumIn(uintField(value, "kind"), kAllSizeKinds);
    const auto extent = boundedInt(uintField(value, "extent"));
    if (!kind || !extent) return std::nullopt;
    switch (*kind) {
    case SizeKind::Flex:
        return Size::flex();
    case SizeKind::Auto:
        return Size::autoSize();
    case SizeKind::Exact:
        return Size::exact(*extent);
    }
    return std::nullopt;  // unreachable: decodeEnumIn already bounded the domain
}

ProtocolValue encodeInset(const Inset& inset) {
    const auto u = [](int v) {
        return ProtocolValue::makeUint(static_cast<std::uint64_t>(v));
    };
    return ProtocolValue::makeObject({{"left", u(inset.left())},
                                      {"right", u(inset.right())},
                                      {"top", u(inset.top())},
                                      {"bottom", u(inset.bottom())}});
}

std::optional<Inset> decodeInset(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const auto l = boundedInt(uintField(value, "left"));
    const auto r = boundedInt(uintField(value, "right"));
    const auto t = boundedInt(uintField(value, "top"));
    const auto b = boundedInt(uintField(value, "bottom"));
    if (!l || !r || !t || !b) return std::nullopt;
    return Inset::of(*l, *r, *t, *b);
}

// --- WidgetDescriptor -----------------------------------------------------

ProtocolValue encodeValueSource(const ValueSource& source) {
    return ProtocolValue::makeObject(
        {{"is_provider", ProtocolValue::makeBool(source.isProvider)},
         {"literal", ProtocolValue::makeText(source.literal)},
         {"provider", ProtocolValue::makeText(source.provider)}});
}

std::optional<ValueSource> decodeValueSource(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const auto isProvider = boolField(value, "is_provider");
    const auto literal = textField(value, "literal");
    const auto provider = textField(value, "provider");
    if (!isProvider || !literal || !provider) return std::nullopt;
    return ValueSource{*isProvider, *literal, *provider};
}

// An absent optional encodes as null; present as its value.
ProtocolValue encodeOptionalValueSource(const std::optional<ValueSource>& v) {
    return v ? encodeValueSource(*v) : ProtocolValue::makeNull();
}
ProtocolValue encodeOptionalText(const std::optional<std::string>& v) {
    return v ? ProtocolValue::makeText(*v) : ProtocolValue::makeNull();
}
ProtocolValue encodeOptionalInt(const std::optional<int>& v) {
    return v ? ProtocolValue::makeInt(*v) : ProtocolValue::makeNull();
}

bool isNull(const ProtocolValue* field) {
    return !field || field->kind() == ProtocolValue::Kind::NullValue;
}

ProtocolValue encodeWidget(const WidgetDescriptor& w) {
    return ProtocolValue::makeObject(
        {{"kind", enumValue(w.kind)},
         {"id", ProtocolValue::makeText(w.id)},
         {"value", encodeOptionalValueSource(w.value)},
         {"checked", encodeOptionalValueSource(w.checked)},
         {"width", encodeOptionalInt(w.width)},
         {"role", encodeOptionalText(w.role)},
         {"command", encodeOptionalText(w.command)},
         {"surface",
          w.surface ? enumValue(*w.surface) : ProtocolValue::makeNull()},
         {"rank", ProtocolValue::makeInt(w.rank)},
         {"keep", ProtocolValue::makeBool(w.keep)},
         {"overflow", enumValue(w.overflow)},
         {"sigil", ProtocolValue::makeText(w.sigil)}});
}

std::optional<WidgetDescriptor> decodeWidget(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const auto kind = decodeEnumIn(uintField(value, "kind"), kAllWidgetKinds);
    const auto id = textField(value, "id");
    const auto rank = value.field("rank") ? value.field("rank")->asInt()
                                          : std::nullopt;
    const auto keep = boolField(value, "keep");
    const auto overflow =
        decodeEnumIn(uintField(value, "overflow"), kAllOverflows);
    const auto sigil = textField(value, "sigil");
    if (!kind || !id || !rank || !keep || !overflow || !sigil) {
        return std::nullopt;
    }

    WidgetDescriptor w;
    w.kind = *kind;
    w.id = *id;
    if (*rank > std::numeric_limits<int>::max() ||
        *rank < std::numeric_limits<int>::min()) {
        return std::nullopt;
    }
    w.rank = static_cast<int>(*rank);
    w.keep = *keep;
    w.overflow = *overflow;
    w.sigil = *sigil;

    if (!isNull(value.field("value"))) {
        auto source = decodeValueSource(*value.field("value"));
        if (!source) return std::nullopt;
        w.value = *source;
    }
    if (!isNull(value.field("checked"))) {
        auto source = decodeValueSource(*value.field("checked"));
        if (!source) return std::nullopt;
        w.checked = *source;
    }
    if (!isNull(value.field("width"))) {
        auto width = value.field("width")->asInt();
        if (!width || *width > std::numeric_limits<int>::max() ||
            *width < std::numeric_limits<int>::min()) {
            return std::nullopt;
        }
        w.width = static_cast<int>(*width);
    }
    if (!isNull(value.field("role"))) {
        if (!value.field("role")->asText()) return std::nullopt;
        w.role = *value.field("role")->asText();
    }
    if (!isNull(value.field("command"))) {
        if (!value.field("command")->asText()) return std::nullopt;
        w.command = *value.field("command")->asText();
    }
    if (!isNull(value.field("surface"))) {
        const auto surface =
            decodeEnumIn(uintField(value, "surface"), kAllViewSurfaces);
        if (!surface) return std::nullopt;
        w.surface = *surface;
    }
    return w;
}

// --- UiNode (recursive) ---------------------------------------------------

ProtocolValue encodeNode(const UiNode& node);

ProtocolValue encodeContainer(const UiContainer& container) {
    std::vector<ProtocolValue> children;
    children.reserve(container.children.size());
    for (const auto& child : container.children) {
        children.push_back(encodeNode(child));
    }
    return ProtocolValue::makeObject(
        {{"axis", enumValue(container.axis)},
         {"inset", encodeInset(container.inset)},
         {"gap", ProtocolValue::makeUint(
                     static_cast<std::uint64_t>(container.gap.extent()))},
         {"children", ProtocolValue::makeArray(std::move(children))}});
}

ProtocolValue encodeNode(const UiNode& node) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", ProtocolValue::makeText(node.id.value()));
    fields.emplace_back("size", encodeSize(node.size));
    if (const auto* container = std::get_if<UiContainer>(&node.content)) {
        fields.emplace_back("container", encodeContainer(*container));
    } else {
        fields.emplace_back(
            "leaf", encodeWidget(std::get<UiLeaf>(node.content).widget));
    }
    return ProtocolValue::makeObject(std::move(fields));
}

std::optional<UiNode> decodeNode(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const auto id = textField(value, "id");
    const ProtocolValue* sizeField = value.field("size");
    if (!id || !sizeField) return std::nullopt;
    const auto size = decodeSize(*sizeField);
    if (!size) return std::nullopt;

    UiNode node;
    node.id = UiNodeId{*id};
    node.size = *size;

    const ProtocolValue* containerField = value.field("container");
    const ProtocolValue* leafField = value.field("leaf");
    const bool hasContainer = containerField && !isNull(containerField);
    const bool hasLeaf = leafField && !isNull(leafField);
    // A node is EXACTLY one of container or leaf; both (or neither) is malformed.
    if (hasContainer == hasLeaf) return std::nullopt;
    if (hasContainer) {
        if (!containerField->asObject()) return std::nullopt;
        const auto axis = decodeEnumIn(uintField(*containerField, "axis"),
                                       kAllAxes);
        const auto gap = boundedInt(uintField(*containerField, "gap"));
        const ProtocolValue* insetField = containerField->field("inset");
        const ProtocolValue* childrenField = containerField->field("children");
        if (!axis || !gap || !insetField || !childrenField ||
            !childrenField->asArray()) {
            return std::nullopt;
        }
        const auto inset = decodeInset(*insetField);
        if (!inset) return std::nullopt;
        UiContainer container;
        container.axis = *axis;
        container.inset = *inset;
        container.gap = Gap::of(*gap);
        for (const auto& childValue : *childrenField->asArray()) {
            auto child = decodeNode(childValue);
            if (!child) return std::nullopt;
            container.children.push_back(std::move(*child));
        }
        node.content = std::move(container);
    } else {
        auto widget = decodeWidget(*leafField);
        if (!widget) return std::nullopt;
        node.content = UiLeaf{std::move(*widget)};
    }
    return node;
}

}  // namespace

ProtocolValue encodeUiSchema(const UiSchema& schema) {
    return ProtocolValue::makeObject(
        {{"generation", ProtocolValue::makeUint(schema.generation.value())},
         {"root", encodeNode(schema.root)}});
}

std::optional<UiSchema> decodeUiSchema(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    const auto generation = uintField(value, "generation");
    const ProtocolValue* rootField = value.field("root");
    if (!generation || !rootField) return std::nullopt;

    auto root = decodeNode(*rootField);
    if (!root) return std::nullopt;

    UiSchema schema;
    schema.generation = Generation{*generation};
    schema.root = std::move(*root);
    // A decoded schema must satisfy the same structural rules as one built
    // in-process (unique non-empty node ids, per-leaf shape); malformed wire can
    // never enter the semantic channel as a plausible schema.
    if (!validateUiSchema(schema).ok()) return std::nullopt;
    return schema;
}

}  // namespace ssg
