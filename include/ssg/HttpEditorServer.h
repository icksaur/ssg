#pragma once

#include <ssg/Protocol.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Http {
class Server;
}

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
    std::optional<Revision> lastAppliedRevision;
};

struct DecodeSessionAttachRequestResult {
    ProtocolError error;
    std::optional<SessionAttachRequest> request;
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return error == ProtocolError::None;
    }
};

[[nodiscard]] std::string encodeSessionAttachRequest(
    SessionAttachRequest const& request);
[[nodiscard]] DecodeSessionAttachRequestResult decodeSessionAttachRequest(
    std::string_view message, ProtocolLimits limits = {});

struct AttachedSession {
    SessionId sessionId;
    InvocationPrincipal principal;
    ViewId viewId;
};

// CONTRACT
// HttpEditorSessionHost: the host is the sole authority for a connection's
//   session id, principal, and capabilities, deriving them from its own policy
//   and never from client-supplied input. SSG is not an authentication
//   boundary; a credential-less attach is a deliberate refusal, not an
//   unimplemented feature, and a host that needs authentication enforces it in
//   its transport before a connection reaches attach.
// CONTRACT
// HttpEditorSessionHost: snapshot() returns the library's aggregate view for
//   the attached client unchanged; the transport reconstructs no feature state
//   and reads no capability from a client payload. Capabilities come only from
//   the principal fixed at attach, so the direct and WebSocket paths cannot
//   diverge into two editors.
class HttpEditorSessionHost {
public:
    virtual ~HttpEditorSessionHost() = default;

    // Bind a new connection to a session. The host assigns the session id,
    // principal, and view by its own policy, or may decline (return nullopt) to
    // cap sessions or reject a connection. Locality-based grants (e.g.
    // `local_file_drop` for a trusted local UI) follow the host's deployment and
    // bind choice. The host correlates later snapshot()/binary() callbacks by the
    // SessionId/ClientId it returned here.
    [[nodiscard]] virtual std::optional<AttachedSession> attach() = 0;
    [[nodiscard]] virtual SessionSnapshot snapshot(SessionId const& sessionId,
                                                   ClientId clientId) = 0;
    virtual void statusAction(SessionId const& sessionId, ClientId clientId,
                               StatusActionInvocation const& invocation) = 0;
    virtual void binary(SessionId const& sessionId, ClientId clientId,
                        BinaryFrame const& frame) = 0;
};

struct HttpEditorRouteConfig {
    std::string route{"/session"};
    std::size_t outboundQueueMessages{32};
    std::size_t replayDeltas{64};
    std::chrono::milliseconds writeTimeout{1000};
    ProtocolLimits protocolLimits{};
};

struct HttpEditorServerConfig {
    std::uint16_t port;
    std::string route{"/session"};
    std::size_t outboundQueueMessages{32};
    std::size_t replayDeltas{64};
    std::chrono::milliseconds writeTimeout{1000};
    ProtocolLimits protocolLimits{};
};

// CONTRACT
// HttpEditorRoute: the referenced EditorSession and Http::Server must outlive
//   the route, and the server must be stopped before the route is destroyed, so
//   no registered connection callback can run against freed route state. A route
//   destroyed while its server is still bound aborts deliberately rather than
//   softening into a best-effort teardown, which would leave a live callback
//   over freed state.
class HttpEditorRoute {
public:
    HttpEditorRoute(Http::Server& server, EditorSession& session,
                    HttpEditorSessionHost& host,
                    HttpEditorRouteConfig config = {});
    ~HttpEditorRoute();

    HttpEditorRoute(HttpEditorRoute const&) = delete;
    HttpEditorRoute& operator=(HttpEditorRoute const&) = delete;

    [[nodiscard]] bool sendBinary(ClientId clientId,
                                   BinaryFrame const& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class HttpEditorServer {
public:
    HttpEditorServer(EditorSession& session,
                      HttpEditorSessionHost& host,
                     HttpEditorServerConfig config);
    ~HttpEditorServer();

    HttpEditorServer(HttpEditorServer const&) = delete;
    HttpEditorServer& operator=(HttpEditorServer const&) = delete;

    void start();
    void stop();

    [[nodiscard]] bool sendBinary(ClientId clientId,
                                   BinaryFrame const& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
