#pragma once

// The generation-scoped dynamic node state: the resolved, geometry-independent
// state a client needs to PRESENT a schema node it cannot resolve itself. The
// published `ui` schema carries value SOURCES (a literal or a provider id); a
// non-grid client (the web) has no status-field registry and cannot resolve a
// provider id. So the runtime resolves each leaf's source here and publishes the
// result, in exact correspondence with the schema it names: one record per schema
// node, keyed by the node's UiNodeId, at the schema's generation.
//
// Presence and renderability are distinct. Every node carries an authoritative
// `present` flag; a leaf additionally carries optional semantic leaf state. A
// Label/Field whose resolution is empty is a still-present node with no leaf state
// (the semantic form of the built-in/TUI drop); a checkbox always carries leaf
// state; a spacer and a container carry none. The content is SEMANTIC -- a
// checkbox's `checked` bool and bare caption, never the TUI's composed glyph.

#include <ssg/UiTree.h>  // UiNodeId, Generation

#include <optional>
#include <string>
#include <vector>

namespace ssg {

// The resolved semantic state of a leaf a client renders. `value` is the caption
// or field text; `label` the accessible label; `command` the click target;
// `checked` is present only for a checkbox.
struct UiLeafState {
    std::string value;
    std::string label;
    std::optional<std::string> command;
    std::optional<bool> checked;

    friend bool operator==(const UiLeafState&, const UiLeafState&) = default;
};

// One schema node's dynamic state: its presence and, for a renderable leaf, its
// resolved leaf state. A container, a spacer, and an empty-resolved Label/Field
// carry no leaf state.
struct UiNodeState {
    UiNodeId id;
    bool present = true;
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

    friend bool operator==(const UiStateSection&, const UiStateSection&) = default;
};

}  // namespace ssg
