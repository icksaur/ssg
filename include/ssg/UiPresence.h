#pragma once

// The published presence section: the authoritative per-node visibility for one
// schema generation, stamped with a PresenceBasis so a client can reconcile an
// optimistically-predicted mutation (phase 7B) against the acknowledgment that
// confirms or rejects it. Presence is a SEPARATE authority from the dynamic value
// state (UiNodeState): value state carries resolved leaf content and regenerates as
// providers change, while presence carries visibility and advances only through a
// mutation patch. Keeping them separate is what lets a client predict a visibility
// change locally without re-resolving values, and gives presence the basis the
// value state has no use for.
//
// One record per schema node, keyed by UiNodeId, in the schema's pre-order. A
// client binds it to the schema of the SAME generation and lays out only present
// nodes (a node lays out iff it and every ancestor are present).

#include <ssg/MutationPatch.h>  // PresenceBasis, PresenceConfig
#include <ssg/UiTree.h>         // UiNodeId, Generation, ValidatedSchema

#include <vector>

namespace ssg {

// One node's authoritative presence.
struct UiPresenceRecord {
    UiNodeId id;
    bool present = true;

    friend bool operator==(const UiPresenceRecord&, const UiPresenceRecord&) =
        default;
};

// The presence section for one schema generation: one record per schema node, at a
// generation and basis.
struct UiPresenceSection {
    Generation generation{0};
    PresenceBasis basis{0};
    std::vector<UiPresenceRecord> nodes;

    friend bool operator==(const UiPresenceSection&, const UiPresenceSection&) =
        default;
};

// Build the presence section for a validated schema against a PresenceConfig: one
// record per schema node in pre-order, carrying the config's generation and basis.
// The config's generation must equal the schema's (a caller pairs them), so the
// section corresponds one-to-one with the schema it names.
[[nodiscard]] UiPresenceSection buildPresenceSection(
    const ValidatedSchema& schema, const PresenceConfig& presence);

}  // namespace ssg
