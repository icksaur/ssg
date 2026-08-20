#pragma once

#include <ssg/CommandInvocation.h>

#include <any>
#include <cstdint>
#include <optional>
#include <string>

namespace ssg {

struct ClientCommand {
    // The command to invoke, named however the caller most cheaply can: a name
    // at the protocol, Lua and palette boundaries, a handle on the keystroke
    // path.  One field, so a dispatch cannot carry two different commands.
    CommandName id;
    Revision baseRevision;
    std::any payload;
};

enum class CommandError : std::uint8_t {
    None,
    UnknownClient,
    UnknownCommand,
    StaleRevision,
    CapabilityDenied,
    HandlerFailed,
    RevisionExhausted,
};

// The routing/geometry consequences of a dispatch, so a host can decide whether
// a buffered follow-on event needs a fresh snapshot before it is handled
// (Lever 3 per-drain snapshot coalescing). Separate axes because a key/paste
// consumes routing state while a pointer/wheel consumes geometry, and a pure
// cursor move changes geometry without changing routing.
struct DispatchEffects {
    bool routingChanged = false;
    bool geometryChanged = false;

    void merge(DispatchEffects other) noexcept {
        routingChanged |= other.routingChanged;
        geometryChanged |= other.geometryChanged;
    }
};

struct CommandResult {
    CommandError error;
    // Default-constructed to the null sentinel.  Without the initializer,
    // `CommandResult{}` aggregate-initializes this member from `{}`, which
    // reaches Revision's EXPLICIT constructor -- legal but warned about, and the
    // warning is the honest one: an implicit conversion is being performed
    // through a constructor written to forbid exactly that.
    Revision revision{};
    std::string message;
    // What this dispatch changed, for a host that coalesces per-drain snapshots
    // (Lever 3). `routingChanged` is true when any interaction-routing input the
    // host reads to interpret the NEXT key changed (focus, prompt kind/value,
    // picker, keymap, catalog, clipboard); `geometryChanged` is the conservative
    // "any semantic revision advanced" gate a pointer/wheel hit-test consumes.
    // Both are unioned across every nested and deferred dispatch this call runs.
    DispatchEffects effects{};

    [[nodiscard]] bool accepted() const noexcept {
        return error == CommandError::None;
    }
};

enum class AttachError : std::uint8_t {
    None,
    DuplicateClient,
};

struct AttachResult {
    AttachError error;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == AttachError::None;
    }
};

struct AttachedClient {
    InvocationPrincipal principal;
    ViewId viewId;
};

struct SessionTopology {
    std::optional<WorkspaceId> activeWorkspace;
    std::optional<ViewId> activeView;

    bool operator==(SessionTopology const&) const = default;
};

}  // namespace ssg
