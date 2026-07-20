#pragma once

#include <ssg/command_registry.h>

#include <any>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ssg {

struct ClientCommand {
    std::string id;
    Revision base_revision;
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
    Revision revision;
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
    ViewId view_id;
};

struct SessionTopology {
    std::optional<WorkspaceId> active_workspace;
    std::optional<ViewId> active_view;

    bool operator==(SessionTopology const&) const = default;
};

class EditorSession {
public:
    explicit EditorSession(CommandRegistry registry,
                           CommandServices* services = nullptr);
    ~EditorSession();

    EditorSession(EditorSession const&) = delete;
    EditorSession& operator=(EditorSession const&) = delete;
    EditorSession(EditorSession&&) = delete;
    EditorSession& operator=(EditorSession&&) = delete;

    [[nodiscard]] AttachResult attach(InvocationPrincipal principal,
                                      ViewId viewId);
    [[nodiscard]] bool detach(ClientId clientId);

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
