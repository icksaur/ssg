#pragma once

// The generation-scoped dynamic node state: the resolved, geometry-independent
// state a client needs to PRESENT and focus a schema node it cannot resolve itself. The
// published `ui` schema carries value SOURCES (a literal or a provider id); a
// non-grid client (the web) has no status-field registry and cannot resolve a
// provider id. So the runtime resolves each leaf's source here and publishes the
// result, in exact correspondence with the schema it names: one record per schema
// node plus the authoritative ordered focus path, all keyed to the schema's
// generation.
//
// Presence and renderability are distinct AND separately published. This section
// carries only a leaf's resolved semantic state; a node's authoritative presence is
// the basis-stamped UiPresenceSection (UiPresence.h), so this section regenerates as
// providers change while presence advances only through a mutation patch. A
// Label/Field whose resolution is empty is a node with no leaf state (the semantic
// form of the built-in/TUI drop); a checkbox always carries leaf state; a spacer and
// a container carry none. A provider-backed TextInput carries authoritative value
// and active state; a state-free TextInput carries none. The content is SEMANTIC --
// a checkbox's `checked` bool and bare caption, never the TUI's composed glyph.

#include <ssg/Theme.h>   // SemanticRole
#include <ssg/UiTree.h>  // UiNodeId, Generation

#include <optional>
#include <string>
#include <vector>

namespace ssg {

// The resolved semantic state of a leaf a client renders. `value` is the caption
// or field text; `label` the accessible label; `command` the click target;
// `checked` is present only for a checkbox; `role` is the effective SemanticRole
// the library resolved (the widget's own role, or the region's default), so a
// client colors the widget by a semantic role ordinal and never re-derives role
// names. `active` is present only for a stateful TextInput; its absence keeps
// browser-local inputs, such as the header picker query, entirely client-owned.
struct UiLeafState {
    std::string value;
    std::string label;
    std::optional<std::string> command;
    std::optional<bool> checked;
    SemanticRole role = SemanticRole::Text;
    std::optional<bool> active;

    friend bool operator==(const UiLeafState&, const UiLeafState&) = default;
};

// One schema node's dynamic state: for a renderable leaf, its resolved leaf state.
// A container, spacer, local TextInput, and empty-resolved Label/Field carry no
// leaf state.
// Presence is NOT here -- it is the separate basis-stamped UiPresenceSection.
struct UiNodeState {
    UiNodeId id;
    std::optional<UiLeafState> leaf;

    friend bool operator==(const UiNodeState&, const UiNodeState&) = default;
};

// The dynamic state for one schema generation: one record per schema node. A
// client binds it to the schema of the SAME generation, and reconciles a mismatch
// (different generation, or a node-id set that does not match the schema) by
// waiting for a consistent frame rather than binding to the wrong tree.
struct UiStateSection {
    Generation generation{0};
    std::vector<UiNodeState> nodes;
    // The authoritative base-to-top keyboard focus path for this generation.
    // Absent only when decoding a frame from a host that predates this field.
    std::optional<std::vector<UiNodeId>> focusPath;

    friend bool operator==(const UiStateSection&, const UiStateSection&) = default;
};

}  // namespace ssg
