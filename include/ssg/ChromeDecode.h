#pragma once

// The init.lua chrome decoder: it turns a Lua-agnostic value tree into a
// validated, generationless `UiComposition` (a set of region trees) or a
// path-qualified error. `init.lua` composes the header and footer from a tree of
// widget descriptors; this header defines the value mirror the decoder reads and
// the decode entry point. The leaf vocabulary the tree decodes to lives in
// UiWidget.h; the region tree it produces lives in UiTree.h.
//
// The decoder is Lua-FREE on purpose: it consumes `ChromeValue`, a minimal
// mirror of a Lua value, so it is a pure, hand-testable function. The
// `lua_State` walker fills a `ChromeValue`; nothing here links Lua.

#include <ssg/UiTree.h>    // UiComposition, UiRegion
#include <ssg/UiWidget.h>  // WidgetDescriptor, ValueSource

#include <cstdint>
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

struct ChromeDecodeResult {
    // A path-qualified message on failure (e.g. `header.left[2]: unknown kind
    // "buton"`); nullopt on success.
    std::optional<std::string> error;
    std::optional<UiComposition> composition;

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

// Decode a `ssg.chrome{...}` argument tree into a validated `UiComposition` (Top
// region for header, Bottom for footer). `validProviders` is the set of live
// provider ids a widget may reference (injected so the decoder does not couple to
// the status-field registry); unknown providers, unknown kinds, per-kind matrix
// violations, header right/centre, `TextInput` in chrome, over-cap trees, and
// structural errors all fail loud with a path-qualified message. The result is
// generationless; the runtime stamps a generation when it publishes. Pure: no
// Lua, no runtime state.
[[nodiscard]] ChromeDecodeResult decodeChrome(
    const ChromeValue& root, const std::vector<std::string>& validProviders);

}  // namespace ssg
