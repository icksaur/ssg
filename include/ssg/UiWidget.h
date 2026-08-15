#pragma once

// The UI-VM leaf vocabulary: the widget descriptor a UiTree leaf carries and the
// value/provider types it composes from. Split out of the chrome decoder so
// UiTree.h depends only on this vocabulary, never on the decoder (which in turn
// depends on UiTree for its UiComposition output) -- the split breaks that cycle.

#include <ssg/Widget.h>  // WidgetKind, Overflow, CenterWidth

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ssg {

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
// `surface` names the client-rendered surface of a `View` leaf (required for a
// View, forbidden otherwise), validated at schema validation.
struct WidgetDescriptor {
    WidgetKind kind = WidgetKind::Label;
    std::string id;
    std::optional<ValueSource> value;
    std::optional<ValueSource> checked;
    std::optional<int> width;
    std::optional<std::string> role;
    std::optional<std::string> command;
    std::optional<ViewSurface> surface;
    int rank = 0;
    bool keep = false;
    Overflow overflow = Overflow::None;
    std::string sigil;

    friend bool operator==(const WidgetDescriptor&, const WidgetDescriptor&) =
        default;
};

// What a live provider resolves to for a composed widget: the displayed value,
// its accessible label, and any inherited click command -- mirroring a built-in
// status field's (value, accessibleLabel, commandId). A provider with no value
// returns nullopt, and the widget is dropped (as the built-in drops an
// empty-value field). Lua-free: the resolver is supplied by the server-side
// snapshot, keeping the decoder/lowering runtime-agnostic.
struct ResolvedProvider {
    std::string value;
    std::string accessibleLabel;
    std::optional<std::string> commandId;
};

using ChromeProviderResolver =
    std::function<std::optional<ResolvedProvider>(std::string_view id)>;

}  // namespace ssg
