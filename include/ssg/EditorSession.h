#pragma once

#include <ssg/CommandRegistry.h>

#include <any>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ssg {

class CommandCatalog;

struct ClientCommand {
    // The command to invoke, named however the caller most cheaply can: a name
    // at the protocol, Lua and palette boundaries, a handle on the keystroke
    // path.  One field, so a dispatch cannot carry two different commands.
    CommandRef id;
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

struct CommandResult {
    CommandError error;
    // Default-constructed to the null sentinel.  Without the initializer,
    // `CommandResult{}` aggregate-initializes this member from `{}`, which
    // reaches Revision's EXPLICIT constructor -- legal but warned about, and the
    // warning is the honest one: an implicit conversion is being performed
    // through a constructor written to forbid exactly that.
    Revision revision{};
    std::string message;

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

class EditorSession {
public:
    // Why a handler's dispatch is refused, and what to do instead.
    //
    // Defined once because two guards report it: this class, and
    // EditorRuntime::dispatch, whose wrapper touches the session before
    // dispatching and so must refuse earlier.  A caller meeting this needs the
    // alternative, not just the prohibition -- composing commands is a
    // supported thing to want (doc/spec-reentrant-dispatch.md).
    static constexpr std::string_view kNestedDispatchRefusal =
        "a command handler may not dispatch another command directly; ask for "
        "it instead, so each command still advances the revision exactly once";

    explicit EditorSession(std::shared_ptr<CommandCatalog> catalog,
                           CommandServices* services = nullptr);
    ~EditorSession();

    EditorSession(EditorSession const&) = delete;
    EditorSession& operator=(EditorSession const&) = delete;
    EditorSession(EditorSession&&) = delete;
    EditorSession& operator=(EditorSession&&) = delete;

    // The catalog this session dispatches from.  Held rather than copied, so a
    // command registered later is visible here with no propagation step.
    [[nodiscard]] std::shared_ptr<CommandCatalog> const& catalog() const;

    [[nodiscard]] AttachResult attach(InvocationPrincipal principal,
                                      ViewId viewId);
    [[nodiscard]] bool detach(ClientId clientId);

    // The revision of the dispatch in progress on THIS thread, if there is
    // one.  A handler runs with the session locked, so anything it calls that
    // would take that lock has to ask this first rather than block on a lock
    // its own call already holds.
    [[nodiscard]] std::optional<Revision> activeDispatchRevision()
        const noexcept;

    [[nodiscard]] CommandResult dispatch(ClientId clientId,
                                         ClientCommand const& command);

    [[nodiscard]] Revision revision() const;
    // Advance the session revision for a library-internal, out-of-band
    // authoritative state change that does not flow through dispatch (M10
    // deferred enrichment: the tree scan and syntax highlighting run by
    // prime_deferred after the first frame).  Client commands still advance the
    // revision only through dispatch; this is the runtime's seam for its own
    // authoritative mutations so delta clients observe them.  Throws on overflow.
    Revision advanceRevision();
    [[nodiscard]] SessionTopology topology() const;
    [[nodiscard]] std::optional<AttachedClient> attachedClient(
        ClientId clientId) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
