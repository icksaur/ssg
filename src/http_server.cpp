#include <ssg/http_server.h>
#include <ssg/startup_audit.h>

#include <http.h>

#include <charconv>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace ssg {
namespace {

char hexDigit(unsigned value) {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + value - 10);
}

std::string hexEncode(std::string_view value) {
    std::string result;
    result.reserve(value.size() * 2);
    for (unsigned char byte : value) {
        result.push_back(hexDigit(byte >> 4));
        result.push_back(hexDigit(byte & 0x0f));
    }
    return result;
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<std::string> hexDecode(std::string_view value) {
    if ((value.size() & 1u) != 0) return std::nullopt;
    std::string result;
    result.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        auto const high = hexValue(value[index]);
        auto const low = hexValue(value[index + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        result.push_back(static_cast<char>((high << 4) | low));
    }
    return result;
}

std::vector<std::string_view> split(std::string_view value) {
    std::vector<std::string_view> result;
    while (!value.empty()) {
        auto const separator = value.find(' ');
        result.push_back(value.substr(0, separator));
        if (separator == std::string_view::npos) break;
        value.remove_prefix(separator + 1);
    }
    return result;
}

std::vector<std::uint8_t> bytes(std::string const& value) {
    return {value.begin(), value.end()};
}

}  // namespace

SessionId::SessionId(std::string value) : value_{std::move(value)} {
    if (value_.empty()) throw std::invalid_argument{"session ID cannot be empty"};
}

std::string encodeSessionAttachRequest(SessionAttachRequest const& request) {
    return "SSG1 ATTACH " +
           (request.lastAppliedRevision
                ? std::to_string(request.lastAppliedRevision->value())
                : std::string{"-"}) +
           " " + hexEncode(request.credential);
}

DecodeSessionAttachRequestResult decodeSessionAttachRequest(
    std::string_view message, ProtocolLimits limits) {
    if (message.size() > limits.maxMessageBytes) {
        return {ProtocolError::MessageTooLarge, std::nullopt,
                "attach request exceeds message limit"};
    }
    auto const fields = split(message);
    if (fields.size() != 4 || fields[0] != "SSG1" ||
        fields[1] != "ATTACH") {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "expected SSG1 ATTACH request"};
    }
    std::optional<Revision> revision;
    if (fields[2] != "-") {
        std::uint64_t value = 0;
        auto const parsed = std::from_chars(fields[2].data(),
                                            fields[2].data() + fields[2].size(),
                                            value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != fields[2].data() + fields[2].size()) {
            return {ProtocolError::MalformedMessage, std::nullopt,
                    "invalid last-applied revision"};
        }
        revision.emplace(value);
    }
    auto credential = hexDecode(fields[3]);
    if (!credential) {
        return {ProtocolError::MalformedMessage, std::nullopt,
                "invalid credential encoding"};
    }
    return {ProtocolError::None,
            SessionAttachRequest{std::move(*credential), revision}, {}};
}

struct HttpEditorRoute::Impl {
    struct Outbound {
        std::string payload;
        bool binary;
    };

    struct Connection {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Outbound> queue;
        bool stopping{false};
        bool detached{false};
        std::thread writer;
        std::optional<AuthenticatedSession> binding;
        std::optional<SessionSnapshot> snapshot;
    };

    struct ReplayRecord {
        Revision baseRevision;
        Revision revision;
        std::string payload;
    };

    using ReplayKey = std::pair<std::string, std::uint64_t>;

    Impl(Http::Server& httpServer, EditorSession& editorSession,
         CommandArgumentCodecRegistry commandArgumentCodecs,
         HttpEditorSessionHost& sessionHost,
         HttpEditorRouteConfig routeConfig)
        : session{editorSession},
          argumentCodecs{std::move(commandArgumentCodecs)},
          host{sessionHost},
          config{std::move(routeConfig)},
          server{httpServer} {
        if (config.route.empty() || config.route.front() != '/') {
            throw std::invalid_argument{
                "WebSocket route must start with a slash"};
        }
        if (config.outboundQueueMessages == 0 || config.replayDeltas == 0 ||
            config.writeTimeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument{
                "WebSocket queue, replay, and write timeout must be positive"};
        }
        server.ws(
            config.route,
            {[this](Http::WebSocketHandle handle) { opened(handle); },
             [this](Http::WebSocketHandle handle, Http::WebSocketMessage message) {
                 received(handle, std::move(message));
             },
             [this](Http::WebSocketHandle handle) { closed(handle); }});
    }

    ~Impl() {
        if (server.boundPort().has_value()) std::terminate();
    }

    void opened(Http::WebSocketHandle handle) {
        auto connection = std::make_shared<Connection>();
        {
            std::lock_guard lock{connectionsMutex};
            connections.emplace(handle, connection);
        }
        connection->writer =
            std::thread{[this, handle, connection] { writeLoop(handle, connection); }};
    }

    void received(Http::WebSocketHandle handle,
                  Http::WebSocketMessage message) {
        auto connection = find(handle);
        if (!connection) return;

        std::lock_guard processLock{processingMutex};
        if (!connection->binding) {
            if (message.opcode != 0x1 ||
                !attach(handle, connection, message.data)) {
                close(handle, connection);
            }
            return;
        }
        if (message.opcode != 0x2) {
            close(handle, connection);
            return;
        }

        auto command = decodeCommandRequest(
            message.data, argumentCodecs, config.protocolLimits);
        if (command.accepted()) {
            auto const result = session.dispatch(
                connection->binding->principal.clientId(), *command.command);
            if (!result.accepted()) {
                enqueue(handle, connection,
                        {encodeCommandResult(result), true});
                return;
            }
            publishSession(connection->binding->sessionId);
            return;
        }

        auto clipboard =
            decodeClipboardResponse(message.data, config.protocolLimits);
        if (clipboard.accepted()) {
            try {
                host.clipboardResponse(
                    connection->binding->sessionId,
                    connection->binding->principal.clientId(),
                    *clipboard.response);
                publishSession(connection->binding->sessionId);
            } catch (...) {
                close(handle, connection);
            }
            return;
        }
        auto status = decodeStatusActionInvocation(message.data,
                                                       config.protocolLimits);
        if (status.accepted()) {
            try {
                host.statusAction(connection->binding->sessionId,
                                   connection->binding->principal.clientId(),
                                   *status.invocation);
                publishSession(connection->binding->sessionId);
            } catch (...) {
                close(handle, connection);
            }
            return;
        }
        auto binary = decodeBinaryFrame(message.data, config.protocolLimits);
        if (binary.accepted()) {
            try {
                host.binary(connection->binding->sessionId,
                            connection->binding->principal.clientId(),
                            *binary.frame);
                publishSession(connection->binding->sessionId);
            } catch (...) {
                close(handle, connection);
            }
            return;
        }
        close(handle, connection);
    }

    bool attach(Http::WebSocketHandle handle,
                std::shared_ptr<Connection> const& connection,
                std::string_view payload) {
        auto request =
            decodeSessionAttachRequest(payload, config.protocolLimits);
        if (!request.accepted()) return false;
        auto authenticated = host.authenticate(request.request->credential);
        if (!authenticated) return false;
        if (authenticated->principal.origin() != InvocationOrigin::Websocket) {
            return false;
        }
        auto const clientId = authenticated->principal.clientId();
        auto const attached =
            session.attach(authenticated->principal, authenticated->viewId);
        if (!attached.accepted()) return false;

        connection->binding.emplace(std::move(*authenticated));
        connection->snapshot.emplace(host.snapshot(
            connection->binding->sessionId, clientId));
        auto const currentRevision = connection->snapshot->revision();

        bool replayed = false;
        if (request.request->lastAppliedRevision) {
            auto next = *request.request->lastAppliedRevision;
            if (next == currentRevision) {
                replayed = true;
            } else {
                auto const key = replayKey(*connection->binding);
                auto const found = replay.find(key);
                std::vector<std::string> chain;
                if (found != replay.end()) {
                    for (auto const& record : found->second) {
                        if (record.baseRevision == next) {
                            chain.push_back(record.payload);
                            next = record.revision;
                        }
                    }
                }
                if (next == currentRevision && !chain.empty() &&
                    chain.size() <= config.outboundQueueMessages) {
                    for (auto& encoded : chain) {
                        enqueue(handle, connection,
                                {std::move(encoded), true});
                    }
                    replayed = true;
                }
            }
        }
        if (!replayed) {
            enqueue(handle, connection,
                    {encodeSessionSnapshot(*connection->snapshot), true});
        }
        return true;
    }

    void publishSession(SessionId const& sessionId) {
        std::vector<std::pair<Http::WebSocketHandle,
                              std::shared_ptr<Connection>>>
            targets;
        {
            std::lock_guard lock{connectionsMutex};
            for (auto const& [handle, connection] : connections) {
                std::lock_guard connectionLock{connection->mutex};
                if (connection->binding && !connection->stopping &&
                    connection->binding->sessionId == sessionId) {
                    targets.emplace_back(handle, connection);
                }
            }
        }
        for (auto const& [handle, connection] : targets) {
            auto current = host.snapshot(
                connection->binding->sessionId,
                connection->binding->principal.clientId());
            if (!connection->snapshot ||
                connection->snapshot->revision() == current.revision()) {
                connection->snapshot.emplace(std::move(current));
                continue;
            }
            auto delta = deriveSessionDelta(*connection->snapshot, current);
            auto encoded = encodeSessionDelta(delta);
            auto& history = replay[replayKey(*connection->binding)];
            history.push_back({delta.baseRevision(), delta.revision(), encoded});
            while (history.size() > config.replayDeltas) history.pop_front();
            connection->snapshot.emplace(std::move(current));
            enqueue(handle, connection, {std::move(encoded), true});
        }
    }

    ReplayKey replayKey(AuthenticatedSession const& binding) const {
        return {std::string{binding.sessionId.value()},
                binding.principal.clientId().value()};
    }

    void enqueue(Http::WebSocketHandle handle,
                 std::shared_ptr<Connection> const& connection,
                 Outbound outbound) {
        bool overflow = false;
        {
            std::lock_guard lock{connection->mutex};
            if (connection->stopping) return;
            if (connection->queue.size() >=
                config.outboundQueueMessages) {
                connection->stopping = true;
                overflow = true;
            } else {
                connection->queue.push_back(std::move(outbound));
            }
        }
        connection->ready.notify_one();
        if (overflow) close(handle, connection);
    }

    void writeLoop(Http::WebSocketHandle handle,
                    std::shared_ptr<Connection> const& connection) {
        for (;;) {
            Outbound outbound;
            {
                std::unique_lock lock{connection->mutex};
                connection->ready.wait(lock, [&] {
                    return connection->stopping || !connection->queue.empty();
                });
                if (connection->queue.empty()) return;
                outbound = std::move(connection->queue.front());
                connection->queue.pop_front();
            }
            auto const deadline =
                std::chrono::steady_clock::now() + config.writeTimeout;
            auto const result =
                outbound.binary
                    ? server.send(handle, bytes(outbound.payload), deadline)
                    : server.send(handle, outbound.payload, deadline);
            if (!result) {
                close(handle, connection);
                return;
            }
        }
    }

    void close(Http::WebSocketHandle handle,
               std::shared_ptr<Connection> const& connection) {
        {
            std::lock_guard lock{connection->mutex};
            connection->stopping = true;
        }
        connection->ready.notify_one();
        {
            std::lock_guard processLock{processingMutex};
            if (connection->binding && !connection->detached) {
                (void)session.detach(
                    connection->binding->principal.clientId());
                connection->detached = true;
            }
        }
        server.closeConnection(handle);
    }

    void closed(Http::WebSocketHandle handle) {
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard lock{connectionsMutex};
            auto const found = connections.find(handle);
            if (found == connections.end()) return;
            connection = std::move(found->second);
            connections.erase(found);
        }
        {
            std::lock_guard lock{connection->mutex};
            connection->stopping = true;
        }
        connection->ready.notify_one();
        if (connection->writer.joinable() &&
            connection->writer.get_id() != std::this_thread::get_id()) {
            connection->writer.join();
        }
        std::lock_guard processLock{processingMutex};
        if (connection->binding && !connection->detached) {
            (void)session.detach(connection->binding->principal.clientId());
            connection->detached = true;
        }
    }

    std::shared_ptr<Connection> find(Http::WebSocketHandle handle) {
        std::lock_guard lock{connectionsMutex};
        auto const found = connections.find(handle);
        return found == connections.end() ? nullptr : found->second;
    }

    std::shared_ptr<Connection> find(ClientId clientId) {
        std::lock_guard lock{connectionsMutex};
        for (auto const& [handle, connection] : connections) {
            (void)handle;
            std::lock_guard connectionLock{connection->mutex};
            if (connection->binding &&
                !connection->stopping &&
                connection->binding->principal.clientId() == clientId) {
                return connection;
            }
        }
        return nullptr;
    }

    bool sendTo(ClientId clientId, std::string payload) {
        std::lock_guard processLock{processingMutex};
        auto connection = find(clientId);
        if (!connection) return false;
        Http::WebSocketHandle handle = 0;
        {
            std::lock_guard lock{connectionsMutex};
            for (auto const& entry : connections) {
                if (entry.second == connection) {
                    handle = entry.first;
                    break;
                }
            }
        }
        if (handle == 0) return false;
        enqueue(handle, connection, {std::move(payload), true});
        return true;
    }

    EditorSession& session;
    CommandArgumentCodecRegistry argumentCodecs;
    HttpEditorSessionHost& host;
    HttpEditorRouteConfig config;
    Http::Server& server;
    std::recursive_mutex processingMutex;
    std::mutex connectionsMutex;
    std::map<Http::WebSocketHandle, std::shared_ptr<Connection>> connections;
    std::map<ReplayKey, std::deque<ReplayRecord>> replay;
};

HttpEditorRoute::HttpEditorRoute(
    Http::Server& server, EditorSession& session,
    CommandArgumentCodecRegistry argumentCodecs,
    HttpEditorSessionHost& host, HttpEditorRouteConfig config)
    : impl_{std::make_unique<Impl>(server, session, std::move(argumentCodecs),
                                  host, std::move(config))} {}

HttpEditorRoute::~HttpEditorRoute() = default;

bool HttpEditorRoute::sendClipboardRequest(
    ClientId clientId, ClipboardRequest const& request) {
    return impl_->sendTo(clientId, encodeClipboardRequest(request));
}

bool HttpEditorRoute::sendBinary(ClientId clientId,
                                  BinaryFrame const& frame) {
    return impl_->sendTo(clientId, encodeBinaryFrame(frame));
}

struct HttpEditorServer::Impl {
    Impl(EditorSession& session,
         CommandArgumentCodecRegistry argumentCodecs,
         HttpEditorSessionHost& host, HttpEditorServerConfig config)
        : server{config.port},
          route{server, session, std::move(argumentCodecs), host,
                {std::move(config.route), config.outboundQueueMessages,
                 config.replayDeltas, config.writeTimeout,
                 config.protocolLimits}} {
        noteOptionalConstruction(OptionalSubsystem::Http);
    }

    ~Impl() { stop(); }

    void start() {
        if (started) {
            throw std::logic_error{"HTTP editor server is already started"};
        }
        server.start();
        started = true;
    }

    void stop() {
        if (!started) return;
        server.stop();
        started = false;
    }

    Http::Server server;
    HttpEditorRoute route;
    bool started{false};
};

HttpEditorServer::HttpEditorServer(
    EditorSession& session, CommandArgumentCodecRegistry argumentCodecs,
    HttpEditorSessionHost& host, HttpEditorServerConfig config)
    : impl_{std::make_unique<Impl>(
          session, std::move(argumentCodecs), host, std::move(config))} {}

HttpEditorServer::~HttpEditorServer() = default;

void HttpEditorServer::start() { impl_->start(); }
void HttpEditorServer::stop() { impl_->stop(); }

bool HttpEditorServer::sendClipboardRequest(
    ClientId clientId, ClipboardRequest const& request) {
    return impl_->route.sendClipboardRequest(clientId, request);
}

bool HttpEditorServer::sendBinary(ClientId clientId,
                                   BinaryFrame const& frame) {
    return impl_->route.sendBinary(clientId, frame);
}

}  // namespace ssg
