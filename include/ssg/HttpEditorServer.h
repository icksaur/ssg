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

class HttpEditorSessionHost {
public:
    virtual ~HttpEditorSessionHost() = default;

    // Bind a new connection to a session. The host is the per-connection
    // principal factory: it assigns the session id, principal (client identity,
    // origin, and any host-granted capabilities), and view by its OWN policy,
    // never from client-supplied input. It may decline (return nullopt) to cap
    // sessions or reject a connection.
    //
    // There is no credential and no authentication: SSG is not an authentication
    // boundary; a host that needs one enforces it in its own transport layer
    // before the connection reaches here. Locality-based grants (e.g.
    // `local_file_drop` for a trusted local UI) follow the host's deployment and
    // bind choice, not a client assertion (doc/spec.md: local-only capabilities
    // are granted by host policy, never by a client assertion). The host
    // correlates later snapshot()/binary() callbacks by the SessionId/ClientId it
    // returned here.
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

class HttpEditorRoute {
public:
    // The referenced server must outlive this route and must be stopped before
    // route destruction so no registered callback can outlive its state.
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
