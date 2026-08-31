#include <ssg/UiTreeProtocol.h>
#include <ssg/Style.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
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
    ProtocolValue::Object fields{
        {"kind", enumValue(size.kind())},
        // Responsive reuses the legacy extent slot as its preferred size.
        {"extent", ProtocolValue::makeUint(
                       static_cast<std::uint64_t>(size.extent()))}};
    if (size.kind() == SizeKind::Responsive) {
        fields.emplace_back(
            "minimum",
            ProtocolValue::makeUint(
                static_cast<std::uint64_t>(size.minimum())));
        fields.emplace_back(
            "growth",
            ProtocolValue::makeUint(
                static_cast<std::uint64_t>(size.growth())));
        fields.emplace_back("optional",
                            ProtocolValue::makeBool(size.optional()));
    }
    return ProtocolValue::makeObject(std::move(fields));
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
constexpr std::array kAllScrollAxes{ScrollAxis::None, ScrollAxis::Vertical};
constexpr std::array kAllSizeKinds{SizeKind::Exact, SizeKind::Flex,
                                   SizeKind::Auto, SizeKind::Responsive};
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
    case SizeKind::Responsive: {
        const auto minimum = boundedInt(uintField(value, "minimum"));
        const auto growth = boundedInt(uintField(value, "growth"));
        const auto optional = boolField(value, "optional");
        if (!minimum || !growth || !optional) return std::nullopt;
        if (*optional && *growth == 0 && *extent > 0 &&
            *minimum <= *extent) {
            return Size::optionalPreferred(*extent, *minimum);
        }
        if (!*optional && *growth == 1 && *extent == *minimum) {
            return Size::minimumFlex(*minimum);
        }
        return std::nullopt;
    }
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

ProtocolValue encodeStyle(const UiNodeStyle& style) {
    ProtocolValue::Object fields;
    if (style.foreground) {
        fields.emplace_back("foreground", enumValue(*style.foreground));
    }
    if (style.background) {
        fields.emplace_back("background", enumValue(*style.background));
    }
    return ProtocolValue::makeObject(std::move(fields));
}

std::optional<UiNodeStyle> decodeStyle(const ProtocolValue& value) {
    if (!value.asObject()) return std::nullopt;
    UiNodeStyle style;
    if (value.field("foreground")) {
        style.foreground = decodeEnumIn(uintField(value, "foreground"),
                                        kAllSemanticRoles);
        if (!style.foreground) return std::nullopt;
    }
    if (value.field("background")) {
        style.background = decodeEnumIn(uintField(value, "background"),
                                        kAllSemanticRoles);
        if (!style.background) return std::nullopt;
    }
    return style;
}

ProtocolValue encodeContainer(const UiContainer& container) {
    std::vector<ProtocolValue> children;
    children.reserve(container.children.size());
    for (const auto& child : container.children) {
        children.push_back(encodeNode(child));
    }
    ProtocolValue::Object fields{
        {"axis", enumValue(container.axis)},
        {"inset", encodeInset(container.inset)},
        {"gap", ProtocolValue::makeUint(
                    static_cast<std::uint64_t>(container.gap.extent()))},
        {"children", ProtocolValue::makeArray(std::move(children))}};
    // Additive: emit `scroll` only when the container is a viewport, so a tree
    // whose scroll flags are all None encodes byte-identically to before.
    if (container.scroll != ScrollAxis::None) {
        fields.emplace_back("scroll", enumValue(container.scroll));
    }
    return ProtocolValue::makeObject(std::move(fields));
}

ProtocolValue encodeNode(const UiNode& node) {
    std::vector<ProtocolValue::Field> fields;
    fields.emplace_back("id", ProtocolValue::makeText(node.id.value()));
    fields.emplace_back("size", encodeSize(node.size));
    if (node.style.foreground || node.style.background) {
        fields.emplace_back("style", encodeStyle(node.style));
    }
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
    if (const ProtocolValue* styleField = value.field("style")) {
        auto style = decodeStyle(*styleField);
        if (!style) return std::nullopt;
        node.style = std::move(*style);
    }
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
        // Additive + forward-compatible: an ABSENT scroll field is None, and an
        // unrecognized numeric ordinal degrades to None (a future axis renders as
        // "not a viewport" rather than rejecting the frame). A PRESENT field of the
        // wrong TYPE (not a uint) is malformed and fails the decode -- only absence
        // and unknown ordinals may degrade.
        if (containerField->field("scroll")) {
            const auto raw = uintField(*containerField, "scroll");
            if (!raw) return std::nullopt;
            container.scroll =
                decodeEnumIn(raw, kAllScrollAxes).value_or(ScrollAxis::None);
        }
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

bool hasChildren(UiNode const& node,
                 std::initializer_list<std::string_view> ids) {
    const auto* container = std::get_if<UiContainer>(&node.content);
    if (!container || container->children.size() != ids.size()) return false;
    std::size_t index = 0;
    for (const auto id : ids) {
        if (container->children[index++].id.value() != id) return false;
    }
    return true;
}

bool isViewLeaf(const UiNode& node, std::string_view id,
                ViewSurface surface) {
    const auto* leaf = std::get_if<UiLeaf>(&node.content);
    return node.id.value() == id && leaf != nullptr &&
           leaf->widget.kind == WidgetKind::View &&
           leaf->widget.surface == surface;
}

// The presentation-bearing legacy delta is frozen until Plan 6, so its schema
// cannot be regenerated when the canonical whole-screen topology moves.
bool normalizePrecedingWholeScreenTopology(UiSchema& schema) {
    if (!hasChildren(schema.root,
                     {kHeaderNodeId, kNoticeNodeId, kExternalModNodeId,
                      kBodyNodeId, kFooterPromptNodeId, kFooterNodeId})) {
        return false;
    }
    auto& root = std::get<UiContainer>(schema.root.content);
    auto& body = root.children[3];
    if (!hasChildren(body, {kPanelNodeId, kContentNodeId})) return false;
    auto& bodyContainer = std::get<UiContainer>(body.content);
    auto& panel = bodyContainer.children[0];
    auto& content = bodyContainer.children[1];
    const StyleDimensions legacyDimensions;
    if (panel.size != Size::exact(legacyDimensions.panelTargetWidth) ||
        content.size != Size::flex()) {
        return false;
    }
    if (!hasChildren(panel,
                     {kFileTreeNodeId, kGitStatusNodeId, kSymbolsNodeId}) ||
        !hasChildren(content, {kEditorNodeId, kFindResultsViewportNodeId})) {
        return false;
    }
    auto& contentContainer = std::get<UiContainer>(content.content);
    auto& editor = contentContainer.children[0];
    auto& findResultsViewport = contentContainer.children[1];
    if (!hasChildren(editor, {kTabBarNodeId, kDocumentViewportNodeId}) ||
        !hasChildren(findResultsViewport, {kFindResultsNodeId})) {
        return false;
    }
    auto& editorContainer = std::get<UiContainer>(editor.content);
    auto& documentViewport = editorContainer.children[1];
    if (!hasChildren(documentViewport, {kDocumentNodeId})) return false;

    UiNode header = std::move(root.children[0]);
    UiNode notice = std::move(root.children[1]);
    UiNode externalModification = std::move(root.children[2]);
    UiNode movedBody = std::move(root.children[3]);
    UiNode footerPrompt = std::move(root.children[4]);
    UiNode footer = std::move(root.children[5]);

    auto& movedBodyContainer = std::get<UiContainer>(movedBody.content);
    movedBodyContainer.children[0].size = Size::optionalPreferred(
        legacyDimensions.panelTargetWidth,
        legacyDimensions.panelMinimumWidth);
    movedBodyContainer.children[1].size =
        Size::minimumFlex(legacyDimensions.editorMinimumWidth);
    auto& movedContent =
        std::get<UiContainer>(movedBodyContainer.children[1].content);
    UiNode movedEditor = std::move(movedContent.children[0]);
    UiNode movedFindResults = std::move(movedContent.children[1]);
    auto& movedEditorContainer =
        std::get<UiContainer>(movedEditor.content);
    UiNode tabBar = std::move(movedEditorContainer.children[0]);
    UiNode movedDocumentViewport =
        std::move(movedEditorContainer.children[1]);
    movedEditorContainer.children = {std::move(movedDocumentViewport)};
    movedContent.children = {
        std::move(tabBar), std::move(notice),
        std::move(externalModification), std::move(movedEditor),
        std::move(movedFindResults)};
    root.children = {std::move(header), std::move(movedBody),
                     std::move(footerPrompt), std::move(footer)};
    return true;
}

bool normalizePrecedingTreeSurface(UiSchema& schema) {
    if (!hasChildren(schema.root, {kHeaderNodeId, kBodyNodeId,
                                   kFooterPromptNodeId, kFooterNodeId})) {
        return false;
    }
    auto& root = std::get<UiContainer>(schema.root.content);
    auto& body = root.children[1];
    if (!hasChildren(body, {kPanelNodeId, kContentNodeId})) return false;
    auto& panel = std::get<UiContainer>(body.content).children[0];
    if (!hasChildren(panel,
                     {kFileTreeNodeId, kGitStatusNodeId, kSymbolsNodeId})) {
        return false;
    }
    auto& children = std::get<UiContainer>(panel.content).children;
    if (!isViewLeaf(children[0], kFileTreeNodeId, ViewSurface::FileTree) ||
        !isViewLeaf(children[1], kGitStatusNodeId, ViewSurface::GitStatus) ||
        !isViewLeaf(children[2], kSymbolsNodeId, ViewSurface::Symbols)) {
        return false;
    }
    children[0].id = UiNodeId{std::string{kTreeNodeId}};
    auto& widget = std::get<UiLeaf>(children[0].content).widget;
    widget.id = std::string{kTreeNodeId};
    widget.surface = ViewSurface::Tree;
    children.erase(children.begin() + 1, children.end());
    return true;
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
    // in-process (unique non-empty node ids, per-leaf shape) AND the whole-screen
    // well-known-area contract; malformed wire can never enter the semantic channel
    // as a plausible schema.
    if (!validateUiSchema(schema).ok()) return std::nullopt;
    if (!validateWellKnownAreas(schema).ok()) {
        const bool topologyNormalized =
            normalizePrecedingWholeScreenTopology(schema);
        const bool treeNormalized = normalizePrecedingTreeSurface(schema);
        if ((!topologyNormalized && !treeNormalized) ||
            !validateWellKnownAreas(schema).ok()) {
            return std::nullopt;
        }
    }
    return schema;
}

}  // namespace ssg
