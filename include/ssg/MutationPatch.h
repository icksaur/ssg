#pragma once

// The atomic mutation-patch model: how server-authoritative presence changes.
//
// A trigger dispatches a command; the command produces a MutationPatch -- a
// single, atomic, ordered set of presence operations over named nodes. The patch,
// not a loose operation, is the unit a consumer applies, and it applies WHOLE or
// not at all against a known pre-state at a generation. The rules pinned here
// (validated by applyMutationPatch, the reference interpreter) are what make every
// client compute the identical post-state:
//   - generation match: a patch names the generation it targets.
//   - no dangling target: every op names a node in the schema.
//   - conflict rejection: two ops on one node are rejected, not order-resolved.
//   - parent/child: hiding a container hides its subtree; showing a node implies
//     showing its ancestors; hiding an ancestor while showing a descendant is a
//     contradiction rejected at validation.
//   - determinism/replay: the same patch from the same pre-state yields the same
//     post-state (a pure function; the shown/hidden sets are disjoint after the
//     contradiction check, so application is order-independent).
//
// Reconciled patches (applied optimistically by a client before the library
// confirms) carry an ApplicationId so a later acknowledgment names the prediction
// it confirms -- a schema generation identifies structure, not which predicted
// patch is being confirmed.

#include <ssg/UiTree.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ssg {

enum class MutationOpKind : std::uint8_t { Show, Hide, Toggle };

struct MutationOp {
    MutationOpKind kind = MutationOpKind::Show;
    UiNodeId target;

    friend bool operator==(const MutationOp&, const MutationOp&) = default;
};

// Identifies one optimistically-applied patch so the library's acknowledgment can
// name the prediction it confirms. Zero is the anonymous/non-reconciled patch.
struct ApplicationId {
    explicit constexpr ApplicationId(std::uint64_t value = 0) noexcept
        : value_{value} {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }
    constexpr auto operator<=>(ApplicationId const&) const noexcept = default;

private:
    std::uint64_t value_;
};

struct MutationPatch {
    Generation generation{0};
    ApplicationId applicationId{0};
    std::vector<MutationOp> ops;

    friend bool operator==(const MutationPatch&, const MutationPatch&) = default;
};

// The server-authoritative present flag per node, at a generation. A node absent
// from the map reads as not present; the library seeds this from the schema's
// per-node defaults.
class PresenceConfig {
public:
    PresenceConfig() = default;

    // Every node in the schema present. A convenient base; real defaults may hide
    // some nodes and are set explicitly.
    [[nodiscard]] static PresenceConfig allPresent(const UiSchema& schema);

    [[nodiscard]] bool isPresent(const UiNodeId& id) const;
    void set(const UiNodeId& id, bool present);

    friend bool operator==(const PresenceConfig&, const PresenceConfig&) =
        default;

private:
    std::map<UiNodeId, bool> present_;
};

struct PatchResult {
    // A message on rejection (generation mismatch, unknown node, conflicting ops,
    // parent/child contradiction); nullopt on success.
    std::optional<std::string> error;
    std::optional<PresenceConfig> post;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

// Apply `patch` to `pre` against `schema`, atomically. Returns the post-state or
// a rejection; never a half-applied state. The reference interpreter for the
// rules above.
[[nodiscard]] PatchResult applyMutationPatch(const UiSchema& schema,
                                             const PresenceConfig& pre,
                                             const MutationPatch& patch);

}  // namespace ssg
