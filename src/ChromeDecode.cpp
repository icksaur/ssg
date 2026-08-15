#include <ssg/ChromeDecode.h>

#include <ssg/RegionRoot.h>  // regionRoleName

#include <array>
#include <limits>
#include <string>

namespace ssg {

const ChromeValue* ChromeValue::find(std::string_view key) const {
    if (kind != Kind::Table) return nullptr;
    for (const auto& [name, value] : table)
        if (name == key) return &value;
    return nullptr;
}

namespace {

// The full set of keys a widget table may carry; anything else is an unknown
// field. Whether a recognized key is ALLOWED is then decided by the per-kind
// matrix (a recognized-but-forbidden key is a clearer error than "unknown").
constexpr std::array<std::string_view, 13> kWidgetKeys{
    "kind", "id", "text", "provider", "checked", "checked_provider", "width",
    "role", "command", "rank", "keep", "overflow", "sigil"};

constexpr std::array<std::string_view, 4> kRegionKeys{"left", "right", "center",
                                                      "separator"};

bool isWidgetKey(std::string_view key) {
    for (auto k : kWidgetKeys)
        if (k == key) return true;
    return false;
}

std::optional<WidgetKind> parseKind(std::string_view s) {
    if (s == "label") return WidgetKind::Label;
    if (s == "field") return WidgetKind::Field;
    if (s == "checkbox") return WidgetKind::Checkbox;
    if (s == "text_input") return WidgetKind::TextInput;
    if (s == "spacer") return WidgetKind::Spacer;
    if (s == "container") return WidgetKind::Container;
    return std::nullopt;
}

std::optional<Overflow> parseOverflow(std::string_view s) {
    if (s == "none") return Overflow::None;
    if (s == "truncate") return Overflow::Truncate;
    if (s == "scroll_tail") return Overflow::ScrollTail;
    return std::nullopt;
}

enum class Slot { Left, Right, Center };

// The decoder's per-region scratch: the widget groups plus the center width
// policy and separator. Not exported -- it exists only between decoding a region
// table and assembling its UiRegion tree. The public output is a UiComposition of
// region trees; this shape never crosses a module boundary.
struct DecodedRow {
    std::vector<WidgetDescriptor> left;
    std::vector<WidgetDescriptor> right;
    std::optional<WidgetDescriptor> center;
    CenterWidth centerWidth = CenterWidth::Flex;
    int centerFixed = 0;
    int separator = 1;
};

UiNode leafFor(const WidgetDescriptor& widget, std::string id, Size size) {
    return UiNode{UiNodeId{std::move(id)}, size, UiLeaf{widget}};
}

// A group container holding the given widgets as leaves. Its Size is Auto
// (content-sized: it never grows to fill), so the left group renders flush at the
// start and the right group flush at the end, with the flex middle between them
// absorbing the slack. `gap` is the separator between items.
UiNode groupFor(std::string id, const std::vector<WidgetDescriptor>& widgets,
                int gap) {
    UiContainer container;
    container.axis = Axis::Row;
    container.gap = Gap::of(gap);
    for (std::size_t i = 0; i < widgets.size(); ++i) {
        container.children.push_back(
            leafFor(widgets[i], id + "." + std::to_string(i), Size::autoSize()));
    }
    return UiNode{UiNodeId{std::move(id)}, Size::autoSize(), std::move(container)};
}

// Assemble a decoded row into a chrome subtree rooted at `base` (a well-known node
// id such as "header"/"footer"):
// Row = [ left(Auto), middle(Flex), right(Auto) ]. The Auto end groups size to
// content and the Flex middle absorbs the slack, so the packing (left flush,
// right flush) is encoded in the SIZING, not positional convention. The center
// widget, if any, sits at the start of the flex middle; its own Size carries the
// width policy (Flex fills; Exact is a fixed center right after the left group).
UiNode assembleRegion(const DecodedRow& row, std::string_view base) {
    const std::string baseId{base};
    UiNode left = groupFor(baseId + ".left", row.left, row.separator);
    UiNode right = groupFor(baseId + ".right", row.right, 0);

    UiContainer middleContainer;
    middleContainer.axis = Axis::Row;
    if (row.center) {
        const Size centerSize = row.centerWidth == CenterWidth::Fixed
                                    ? Size::exact(row.centerFixed)
                                    : Size::flex();
        middleContainer.children.push_back(
            leafFor(*row.center, baseId + ".middle.0", centerSize));
    }
    UiNode middle{UiNodeId{baseId + ".middle"}, Size::flex(),
                  std::move(middleContainer)};

    UiContainer rootContainer;
    rootContainer.axis = Axis::Row;
    rootContainer.children.push_back(std::move(left));
    rootContainer.children.push_back(std::move(middle));
    rootContainer.children.push_back(std::move(right));

    return UiNode{UiNodeId{baseId}, Size::flex(), std::move(rootContainer)};
}

// Fail-loud recursive decoder. The first error short-circuits; every message is
// path-qualified so a Lua author can locate the offending field.
class Decoder {
public:
    explicit Decoder(const std::vector<std::string>& validProviders)
        : validProviders_{validProviders} {}

    std::optional<UiComposition> decode(const ChromeValue& root) {
        if (root.kind != ChromeValue::Kind::Table)
            return failC("chrome", "expected a table");
        for (const auto& [key, _] : root.table) {
            if (key != "header" && key != "footer")
                return failC("chrome." + key, "unknown field");
        }
        // The composed chrome is two subtrees (header, footer) under one root
        // Column; placement is their order in the root, not a region enum. Panel
        // and content join the root in a later sub-step.
        UiContainer rootContainer;
        rootContainer.axis = Axis::Column;
        if (const auto* h = root.find("header")) {
            DecodedRow row;
            if (!decodeRegion(*h, "header", /*isHeader=*/true, row))
                return std::nullopt;
            rootContainer.children.push_back(assembleRegion(row, kHeaderNodeId));
        }
        if (const auto* f = root.find("footer")) {
            DecodedRow row;
            if (!decodeRegion(*f, "footer", /*isHeader=*/false, row))
                return std::nullopt;
            rootContainer.children.push_back(assembleRegion(row, kFooterNodeId));
        }
        UiComposition out;
        out.root = UiNode{UiNodeId{std::string{kRootNodeId}}, Size::flex(),
                          std::move(rootContainer)};
        return out;
    }

    [[nodiscard]] const std::optional<std::string>& error() const {
        return error_;
    }

private:
    std::optional<UiComposition> failC(const std::string& path,
                                       const std::string& why) {
        error_ = path + ": " + why;
        return std::nullopt;
    }
    bool failB(const std::string& path, const std::string& why) {
        error_ = path + ": " + why;
        return false;
    }

    bool decodeRegion(const ChromeValue& value, const std::string& path,
                      bool isHeader, DecodedRow& out) {
        if (value.kind != ChromeValue::Kind::Table)
            return failB(path, "expected a table");
        for (const auto& [key, _] : value.table) {
            bool known = false;
            for (auto k : kRegionKeys) known = known || k == key;
            if (!known) return failB(path + "." + key, "unknown field");
        }

        if (const auto* left = value.find("left")) {
            if (!decodeSide(*left, path + ".left", Slot::Left, isHeader, out.left))
                return false;
        }
        if (const auto* right = value.find("right")) {
            // The header is left-group only while the picker input line owns the
            // trailing cells; any
            // `right` key in a header is rejected on presence.
            if (isHeader)
                return failB(path + ".right",
                             "not allowed (header is left-group only)");
            if (!decodeSide(*right, path + ".right", Slot::Right, isHeader,
                            out.right))
                return false;
        }
        if (const auto* center = value.find("center")) {
            if (isHeader)
                return failB(path + ".center",
                             "not allowed (header is left-group only)");
            WidgetDescriptor widget;
            if (!decodeWidget(*center, path + ".center", Slot::Center, isHeader,
                              widget, out.centerWidth, out.centerFixed))
                return false;
            if (++totalWidgets_ > kChromeMaxTotalWidgets)
                return failB(path + ".center",
                             overCap("total widgets", totalWidgets_,
                                     kChromeMaxTotalWidgets));
            out.center = std::move(widget);
        }
        if (const auto* sep = value.find("separator")) {
            if (sep->kind != ChromeValue::Kind::Integer)
                return failB(path + ".separator", "expected an integer");
            if (!readInt(sep->integer, path + ".separator", /*allowNegative=*/false,
                         out.separator))
                return false;
        }
        return true;
    }

    bool decodeSide(const ChromeValue& value, const std::string& path, Slot slot,
                    bool isHeader, std::vector<WidgetDescriptor>& out) {
        if (value.kind != ChromeValue::Kind::Array)
            return failB(path, "expected an array");
        if (static_cast<int>(value.array.size()) > kChromeMaxPerSide)
            return failB(path, overCap("widgets per side",
                                       static_cast<int>(value.array.size()),
                                       kChromeMaxPerSide));
        for (std::size_t i = 0; i < value.array.size(); ++i) {
            const std::string itemPath = path + "[" + std::to_string(i) + "]";
            WidgetDescriptor widget;
            CenterWidth unusedWidth = CenterWidth::Flex;
            int unusedFixed = 0;
            if (!decodeWidget(value.array[i], itemPath, slot, isHeader,
                              widget, unusedWidth, unusedFixed))
                return false;
            if (++totalWidgets_ > kChromeMaxTotalWidgets)
                return failB(itemPath,
                             overCap("total widgets", totalWidgets_,
                                     kChromeMaxTotalWidgets));
            out.push_back(std::move(widget));
        }
        return true;
    }

    // `centerWidth`/`centerFixed` are written only for the Center slot.
    bool decodeWidget(const ChromeValue& value, const std::string& path,
                      Slot slot, bool isHeader, WidgetDescriptor& out,
                      CenterWidth& centerWidth, int& centerFixed) {
        if (value.kind != ChromeValue::Kind::Table)
            return failB(path, "expected a widget table");
        // A generated slot id (the decode path) makes the projected node id
        // stable and unique when the author omits `id`.
        out.id = path;

        const auto* kindValue = value.find("kind");
        if (!kindValue || kindValue->kind != ChromeValue::Kind::String)
            return failB(path + ".kind", "required string field");
        const auto kind = parseKind(kindValue->string);
        if (!kind) return failB(path + ".kind", "unknown kind \"" +
                                                    kindValue->string + "\"");
        if (*kind == WidgetKind::TextInput)
            return failB(path, "\"text_input\" is not allowed in chrome");
        if (*kind == WidgetKind::Container)
            return failB(path, "\"container\" is not a leaf widget");
        out.kind = *kind;

        for (const auto& [key, _] : value.table)
            if (!isWidgetKey(key)) return failB(path + "." + key, "unknown field");

        // --- value: text XOR provider ---
        const bool hasText = value.find("text") != nullptr;
        const bool hasProvider = value.find("provider") != nullptr;
        if (hasText && hasProvider)
            return failB(path, "value has both \"text\" and \"provider\"");
        const bool valueAllowed =
            *kind == WidgetKind::Label || *kind == WidgetKind::Field ||
            *kind == WidgetKind::Checkbox;
        const bool valueRequired =
            *kind == WidgetKind::Label || *kind == WidgetKind::Field;
        if (hasText || hasProvider) {
            if (!valueAllowed)
                return failB(path, "\"text\"/\"provider\" not allowed for kind \"" +
                                       kindValue->string + "\"");
            ValueSource src;
            if (!readValueSource(value, path, hasProvider, src)) return false;
            out.value = std::move(src);
        } else if (valueRequired) {
            return failB(path, "kind \"" + kindValue->string +
                                   "\" requires \"text\" or \"provider\"");
        }

        // --- checked: bool XOR checked_provider (Checkbox only) ---
        const bool hasChecked = value.find("checked") != nullptr;
        const bool hasCheckedProvider = value.find("checked_provider") != nullptr;
        if (hasChecked && hasCheckedProvider)
            return failB(path, "\"checked\" and \"checked_provider\" are exclusive");
        if (hasChecked || hasCheckedProvider) {
            if (*kind != WidgetKind::Checkbox)
                return failB(path, "\"checked\" only allowed for kind \"checkbox\"");
            ValueSource src;
            if (hasCheckedProvider) {
                if (!readProvider(value, path, "checked_provider", src))
                    return false;
            } else {
                const auto* c = value.find("checked");
                if (c->kind != ChromeValue::Kind::Boolean)
                    return failB(path + ".checked", "expected a boolean");
                src.literal = c->boolean ? "true" : "false";
            }
            out.checked = std::move(src);
        } else if (*kind == WidgetKind::Checkbox) {
            return failB(path,
                         "kind \"checkbox\" requires \"checked\" or "
                         "\"checked_provider\"");
        }

        // --- width: center sizing, or a left/right Spacer's blank width ---
        if (!decodeWidth(value, path, slot, *kind, out, centerWidth, centerFixed))
            return false;

        // --- role (forbidden on Spacer) ---
        if (const auto* role = value.find("role")) {
            if (*kind == WidgetKind::Spacer)
                return failB(path + ".role", "not allowed for kind \"spacer\"");
            if (role->kind != ChromeValue::Kind::String)
                return failB(path + ".role", "expected a string");
            out.role = role->string;
        }

        // --- command (allowed on Field/Checkbox) ---
        if (const auto* command = value.find("command")) {
            const bool commandAllowed =
                *kind == WidgetKind::Field || *kind == WidgetKind::Checkbox;
            if (!commandAllowed)
                return failB(path + ".command",
                             "not allowed for kind \"" + kindValue->string + "\"");
            if (command->kind != ChromeValue::Kind::String)
                return failB(path + ".command", "expected a string");
            out.command = command->string;
        }

        // --- general geometry knobs, allowed for any lowerable kind ---
        if (const auto* id = value.find("id")) {
            if (id->kind != ChromeValue::Kind::String)
                return failB(path + ".id", "expected a string");
            out.id = id->string;
        }
        if (const auto* rank = value.find("rank")) {
            if (rank->kind != ChromeValue::Kind::Integer)
                return failB(path + ".rank", "expected an integer");
            if (!readInt(rank->integer, path + ".rank", /*allowNegative=*/true,
                         out.rank))
                return false;
        }
        if (const auto* keep = value.find("keep")) {
            if (keep->kind != ChromeValue::Kind::Boolean)
                return failB(path + ".keep", "expected a boolean");
            out.keep = keep->boolean;
        }
        if (const auto* overflow = value.find("overflow")) {
            if (overflow->kind != ChromeValue::Kind::String)
                return failB(path + ".overflow", "expected a string");
            const auto parsed = parseOverflow(overflow->string);
            if (!parsed)
                return failB(path + ".overflow",
                             "unknown overflow \"" + overflow->string + "\"");
            out.overflow = *parsed;
        }
        if (const auto* sigil = value.find("sigil")) {
            if (sigil->kind != ChromeValue::Kind::String)
                return failB(path + ".sigil", "expected a string");
            out.sigil = sigil->string;
        }
        return true;
    }

    bool decodeWidth(const ChromeValue& value, const std::string& path, Slot slot,
                     WidgetKind kind, WidgetDescriptor& out,
                     CenterWidth& centerWidth, int& centerFixed) {
        const auto* width = value.find("width");
        if (slot == Slot::Center) {
            // Center sizing: "flex" or an integer. A center Spacer is always a
            // flex gap (width forbidden), matching the lowering rule.
            if (kind == WidgetKind::Spacer) {
                if (width) return failB(path + ".width",
                                        "not allowed for a center \"spacer\"");
                centerWidth = CenterWidth::Flex;
                return true;
            }
            if (!width) {
                centerWidth = CenterWidth::Flex;  // default
                return true;
            }
            if (width->kind == ChromeValue::Kind::String) {
                if (width->string != "flex")
                    return failB(path + ".width",
                                 "expected \"flex\" or an integer");
                centerWidth = CenterWidth::Flex;
            } else if (width->kind == ChromeValue::Kind::Integer) {
                centerWidth = CenterWidth::Fixed;
                if (!readInt(width->integer, path + ".width",
                             /*allowNegative=*/false, centerFixed))
                    return false;
            } else {
                return failB(path + ".width", "expected \"flex\" or an integer");
            }
            return true;
        }
        // Left/right: width is the fixed blank of a Spacer, forbidden otherwise.
        if (kind == WidgetKind::Spacer) {
            if (!width) return failB(path, "a left/right \"spacer\" requires "
                                           "an integer \"width\"");
            if (width->kind != ChromeValue::Kind::Integer)
                return failB(path + ".width", "expected an integer");
            int w = 0;
            if (!readInt(width->integer, path + ".width", /*allowNegative=*/false,
                         w))
                return false;
            out.width = w;
            return true;
        }
        if (width)
            return failB(path + ".width",
                         "not allowed for a left/right non-spacer widget");
        return true;
    }

    bool readValueSource(const ChromeValue& value, const std::string& path,
                         bool isProvider, ValueSource& out) {
        if (isProvider) return readProvider(value, path, "provider", out);
        const auto* text = value.find("text");
        if (text->kind != ChromeValue::Kind::String)
            return failB(path + ".text", "expected a string");
        out.isProvider = false;
        out.literal = text->string;
        return true;
    }

    bool readProvider(const ChromeValue& value, const std::string& path,
                      std::string_view key, ValueSource& out) {
        const auto* p = value.find(key);
        if (p->kind != ChromeValue::Kind::String)
            return failB(path + "." + std::string{key}, "expected a string");
        bool known = false;
        for (const auto& id : validProviders_) known = known || id == p->string;
        if (!known)
            return failB(path + "." + std::string{key},
                         "unknown provider \"" + p->string + "\"");
        out.isProvider = true;
        out.provider = p->string;
        return true;
    }

    std::string overCap(const std::string& name, int n, int max) {
        return "exceeds " + name + " (" + std::to_string(n) + " > " +
               std::to_string(max) + ")";
    }

    // Narrow a Lua integer to `int` fail-loud: reject negatives (unless allowed)
    // and any value outside the `int` range, so a huge value cannot overflow the
    // cast and silently corrupt geometry.
    bool readInt(long long v, const std::string& path, bool allowNegative,
                 int& out) {
        if (!allowNegative && v < 0)
            return failB(path, "must not be negative");
        if (v > std::numeric_limits<int>::max() ||
            v < std::numeric_limits<int>::min())
            return failB(path, "out of range");
        out = static_cast<int>(v);
        return true;
    }

    const std::vector<std::string>& validProviders_;
    int totalWidgets_ = 0;
    std::optional<std::string> error_;
};

}  // namespace

ChromeDecodeResult decodeChrome(
    const ChromeValue& root, const std::vector<std::string>& validProviders) {
    Decoder decoder{validProviders};
    auto composition = decoder.decode(root);
    if (!composition) return {decoder.error(), std::nullopt};
    return {std::nullopt, ValidatedComposition{std::move(*composition)}};
}

}  // namespace ssg
