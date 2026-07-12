#pragma once

#include <ssg/protocol.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ssg {

class SessionId {
public:
    explicit SessionId(std::string value);

    [[nodiscard]] std::string_view value() const noexcept { return value_; }
    auto operator<=>(SessionId const&) const = default;

private:
    std::string value_;
};

struct SessionAttachRequest {
    std::string credential;
    std::optional<Revision> last_applied_revision;
};

struct DecodeSessionAttachRequestResult {
    ProtocolError error;
    std::optional<SessionAttachRequest> request;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::none;
    }
};

[[nodiscard]] std::string encode_session_attach_request(
    SessionAttachRequest const& request);
[[nodiscard]] DecodeSessionAttachRequestResult decode_session_attach_request(
    std::string_view message, ProtocolLimits limits = {});

struct AuthenticatedSession {
    SessionId session_id;
    InvocationPrincipal principal;
    ViewId view_id;
};

class HttpEditorSessionHost {
public:
    virtual ~HttpEditorSessionHost() = default;

    [[nodiscard]] virtual std::optional<AuthenticatedSession> authenticate(
        std::string_view credential) = 0;
    [[nodiscard]] virtual SessionSnapshot snapshot(SessionId const& session_id,
                                                   ClientId client_id) = 0;
    virtual void clipboard_response(SessionId const& session_id,
                                    ClientId client_id,
                                    ClipboardResponse const& response) = 0;
    virtual void status_action(SessionId const& session_id, ClientId client_id,
                               StatusActionInvocation const& invocation) = 0;
    virtual void binary(SessionId const& session_id, ClientId client_id,
                        BinaryFrame const& frame) = 0;
};

struct HttpEditorServerConfig {
    std::uint16_t port;
    std::string route{"/session"};
    std::size_t outbound_queue_messages{32};
    std::size_t replay_deltas{64};
    std::chrono::milliseconds write_timeout{1000};
    ProtocolLimits protocol_limits{};
};

class HttpEditorServer {
public:
    HttpEditorServer(EditorSession& session,
                     CommandArgumentCodecRegistry argument_codecs,
                     HttpEditorSessionHost& host,
                     HttpEditorServerConfig config);
    ~HttpEditorServer();

    HttpEditorServer(HttpEditorServer const&) = delete;
    HttpEditorServer& operator=(HttpEditorServer const&) = delete;

    void start();
    void stop();

    [[nodiscard]] bool send_clipboard_request(
        ClientId client_id, ClipboardRequest const& request);
    [[nodiscard]] bool send_binary(ClientId client_id,
                                   BinaryFrame const& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
