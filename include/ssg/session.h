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
    none,
    unknown_client,
    unknown_command,
    stale_revision,
    capability_denied,
    handler_failed,
    revision_exhausted,
};

struct CommandResult {
    CommandError error;
    Revision revision;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == CommandError::none;
    }
};

enum class AttachError : std::uint8_t {
    none,
    duplicate_client,
};

struct AttachResult {
    AttachError error;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == AttachError::none;
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
    explicit EditorSession(CommandRegistry registry);
    ~EditorSession();

    EditorSession(EditorSession const&) = delete;
    EditorSession& operator=(EditorSession const&) = delete;
    EditorSession(EditorSession&&) = delete;
    EditorSession& operator=(EditorSession&&) = delete;

    [[nodiscard]] AttachResult attach(InvocationPrincipal principal,
                                      ViewId view_id);
    [[nodiscard]] bool detach(ClientId client_id);

    [[nodiscard]] CommandResult dispatch(ClientId client_id,
                                         ClientCommand const& command);

    [[nodiscard]] Revision revision() const;
    [[nodiscard]] SessionTopology topology() const;
    [[nodiscard]] std::optional<AttachedClient> attached_client(
        ClientId client_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
