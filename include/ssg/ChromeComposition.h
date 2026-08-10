#pragma once

// The init.lua chrome/widget composition descriptors and their decoder
//. `init.lua` composes the header
// and footer from a tree of widget descriptors; this header defines the pure
// value types that tree decodes to, plus the decoder that turns a Lua-agnostic
// value tree into a validated `ChromeComposition` (or a path-qualified error).
//
// The decoder is Lua-FREE on purpose: it consumes `ChromeValue`, a minimal
// mirror of a Lua value, so it is a pure, hand-testable function. The phase-4
// `lua_State` walker fills a `ChromeValue`; nothing here links Lua.

#include <ssg/Widget.h>  // WidgetKind, Overflow, CenterWidth

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssg {

// A Lua-agnostic mirror of one Lua value the decoder reads. Kept deliberately
// small: the chrome schema needs booleans, integers, strings, arrays (Lua list
// tables, e.g. a `left` group) and keyed tables (a region or a widget). Factory
// helpers keep test trees readable.
struct ChromeValue {
    enum class Kind : std::uint8_t { Boolean, Integer, String, Array, Table };

    Kind kind = Kind::Table;
    bool boolean = false;
    long long integer = 0;
    std::string string;
    std::vector<ChromeValue> array;
    std::vector<std::pair<std::string, ChromeValue>> table;

    static ChromeValue ofBool(bool value) {
        ChromeValue v;
        v.kind = Kind::Boolean;
        v.boolean = value;
        return v;
    }
    static ChromeValue ofInt(long long value) {
        ChromeValue v;
        v.kind = Kind::Integer;
        v.integer = value;
        return v;
    }
    static ChromeValue ofString(std::string value) {
        ChromeValue v;
        v.kind = Kind::String;
        v.string = std::move(value);
        return v;
    }
    static ChromeValue ofArray(std::vector<ChromeValue> items) {
        ChromeValue v;
        v.kind = Kind::Array;
        v.array = std::move(items);
        return v;
    }
    static ChromeValue ofTable(
        std::vector<std::pair<std::string, ChromeValue>> entries) {
        ChromeValue v;
        v.kind = Kind::Table;
        v.table = std::move(entries);
        return v;
    }

    // The value for `key` in a table, or nullptr (not a table, or absent).
    [[nodiscard]] const ChromeValue* find(std::string_view key) const;
};

// One widget's value source: a literal XOR a named live provider. `literal`
// holds the text (or "true"/"false" for a boolean `checked` source); `provider`
// holds the provider id when `isProvider`.
struct ValueSource {
    bool isProvider = false;
    std::string literal;   // used when !isProvider
    std::string provider;  // used when isProvider

    friend bool operator==(const ValueSource&, const ValueSource&) = default;
};

// One composed widget. The SUPERSET type that also serves forms; the per-kind
// CHROME-context field matrix governs which
// fields are required/allowed/forbidden here, enforced by the decoder. `value`
// is the text/caption source, `checked` the checkbox state source, `width` a
// left/right `Spacer`'s blank width, `command` the click target (validated at
// dispatch, not here), `role` a SemanticRole name (validated at lowering).
struct WidgetDescriptor {
    WidgetKind kind = WidgetKind::Label;
    std::string id;
    std::optional<ValueSource> value;
    std::optional<ValueSource> checked;
    std::optional<int> width;
    std::optional<std::string> role;
    std::optional<std::string> command;
    int rank = 0;
    bool keep = false;
    Overflow overflow = Overflow::None;
    std::string sigil;

    friend bool operator==(const WidgetDescriptor&, const WidgetDescriptor&) =
        default;
};

// One row: a stack of widgets packed left/right with an optional single centre.
// The reused unit -- a header is one row, a footer is one, and a form (deferred)
// is a Column of rows. `separator` cells sit between adjacent left items.
struct RowDescriptor {
    std::vector<WidgetDescriptor> left;
    std::vector<WidgetDescriptor> right;
    std::optional<WidgetDescriptor> center;
    CenterWidth centerWidth = CenterWidth::Flex;
    int centerFixed = 0;
    int separator = 1;

    friend bool operator==(const RowDescriptor&, const RowDescriptor&) = default;
};

// What one `ssg.chrome` call stages: an optional composed header and/or footer.
// An absent region keeps its built-in chrome.
struct ChromeComposition {
    std::optional<RowDescriptor> header;
    std::optional<RowDescriptor> footer;

    friend bool operator==(const ChromeComposition&, const ChromeComposition&) =
        default;
};

// What a live provider resolves to for a composed widget: the displayed value,
// its accessible label, and any inherited click command -- mirroring a built-in
// status field's (value, accessibleLabel, commandId). A provider with no value
// returns nullopt, and the widget is dropped (as the built-in drops an
// empty-value field). Lua-free: the resolver is supplied by the server-side
// snapshot, keeping this header (and the decoder/lowering) runtime-agnostic.
struct ResolvedProvider {
    std::string value;
    std::string accessibleLabel;
    std::optional<std::string> commandId;
};

using ChromeProviderResolver =
    std::function<std::optional<ResolvedProvider>(std::string_view id)>;

struct ChromeDecodeResult {    // A path-qualified message on failure (e.g. `header.left[2]: unknown kind
    // "buton"`); nullopt on success.
    std::optional<std::string> error;
    std::optional<ChromeComposition> composition;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

// Decode-time caps (mechanism, tunable). Exceeding either is a fail-loud,
// path-qualified error. In
// CHROME the per-side cap is what bites (a header's one side + a footer's three
// slots is bounded well under the total); the total cap is a forward-looking
// ceiling for the multi-row FORM reuse of this decoder. (A nesting-depth cap
// lands with the nested/forms decoder, where a Container can recurse; the
// phase-1 chrome schema is flat, so depth is bounded by the fixed shape.)
inline constexpr int kChromeMaxPerSide = 64;
inline constexpr int kChromeMaxTotalWidgets = 256;

// Decode a `ssg.chrome{...}` argument tree into a validated `ChromeComposition`.
// `validProviders` is the set of live provider ids a widget may reference
// (injected so the decoder does not couple to the status-field registry);
// unknown providers, unknown kinds, per-kind matrix violations, header
// right/centre, `TextInput` in chrome, over-cap trees, and structural errors all
// fail loud with a path-qualified message. Pure: no Lua, no runtime state.
[[nodiscard]] ChromeDecodeResult decodeChromeComposition(
    const ChromeValue& root, const std::vector<std::string>& validProviders);

}  // namespace ssg
