#include <ssg/HttpEditorServer.h>
#include <ssg/EditorRuntime.h>
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
                : std::string{"-"});
}

DecodeSessionAttachRequestResult decodeSessionAttachRequest(
    std::string_view message, ProtocolLimits limits) {
    if (message.size() > limits.maxMessageBytes) {
        return {ProtocolError::MessageTooLarge, std::nullopt,
                "attach request exceeds message limit"};
    }
    auto const fields = split(message);
    if (fields.empty() || fields[0] != "SSG1") {
        return {ProtocolError::UnsupportedVersion, std::nullopt,
                "unsupported protocol version"};
    }
    if (fields.size() != 3 || fields[1] != "ATTACH") {
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
    return {ProtocolError::None, SessionAttachRequest{revision}, {}};
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
        std::optional<AttachedSession> binding;
        std::optional<SessionSnapshot> snapshot;
    };

    struct ReplayRecord {
        Revision baseRevision;
        Revision revision;
        std::string payload;
    };

    using ReplayKey = std::pair<std::string, std::uint64_t>;

    Impl(Http::Server& httpServer, EditorRuntime& editorRuntime,
         HttpEditorConnectionPolicy& connectionPolicy,
         HttpEditorRouteConfig routeConfig)
        : runtime{editorRuntime},
          argumentCodecs{runtime.commandCatalog()},
          policy{connectionPolicy},
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

        auto command = ProtocolCodec{}.decodeCommandRequest(
            message.data, argumentCodecs, config.protocolLimits);
        if (command.accepted()) {
            auto const result = runtime.dispatch(
                connection->binding->principal.clientId(), *command.command);
            if (!result.accepted()) {
                enqueue(handle, connection,
                        {ProtocolCodec{}.encodeCommandResult(result), true});
                return;
            }
            publishSession(connection->binding->sessionId);
            return;
        }

        auto status = ProtocolCodec{}.decodeStatusActionInvocation(message.data,
                                                       config.protocolLimits);
        if (status.accepted()) {
            try {
                auto const result = runtime.dispatch(
                    connection->binding->principal.clientId(),
                    {"status.invoke_action", runtime.revision(),
                     *status.invocation});
                if (!result.accepted()) {
                    enqueue(handle, connection,
                            {ProtocolCodec{}.encodeCommandResult(result), true});
                    return;
                }
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
        auto attached = policy.attach();
        if (!attached) return false;
        if (attached->principal.origin() != InvocationOrigin::Websocket) {
            return false;
        }
        auto const clientId = attached->principal.clientId();
        auto const attachResult =
            runtime.attach(attached->principal, attached->viewId);
        if (!attachResult.accepted()) return false;

        connection->binding.emplace(std::move(*attached));
        connection->snapshot = runtime.snapshot(clientId);
        if (!connection->snapshot) {
            (void)runtime.detach(clientId);
            connection->binding.reset();
            return false;
        }
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
                    {ProtocolCodec{}.encodeSessionSnapshot(*connection->snapshot), true});
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
            auto current =
                runtime.snapshot(connection->binding->principal.clientId());
            if (!current) {
                close(handle, connection);
                continue;
            }
            if (!connection->snapshot ||
                connection->snapshot->revision() == current->revision()) {
                connection->snapshot = std::move(current);
                continue;
            }
            // The delta is only an optimization. When a transition cannot be
            // expressed as one (a document appearing where the previous snapshot
            // had none, a presentation-mode change), re-sync this connection with a
            // full snapshot rather than letting the exception escape and terminate
            // the host. Recording no replay record leaves a revision gap the
            // reconnect path already covers by sending a full snapshot.
            std::string encoded;
            try {
                auto delta =
                    SessionSnapshotCodec{}.deriveDelta(*connection->snapshot,
                                                       *current);
                encoded = ProtocolCodec{}.encodeSessionDelta(delta);
                auto& history = replay[replayKey(*connection->binding)];
                history.push_back({delta.baseRevision(), delta.revision(), encoded});
                while (history.size() > config.replayDeltas) history.pop_front();
            } catch (std::exception const&) {
                encoded = ProtocolCodec{}.encodeSessionSnapshot(*current);
            }
            connection->snapshot = std::move(current);
            enqueue(handle, connection, {std::move(encoded), true});
        }
    }

    ReplayKey replayKey(AttachedSession const& binding) const {
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
                (void)runtime.detach(
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
            (void)runtime.detach(connection->binding->principal.clientId());
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

    EditorRuntime& runtime;
    // Derived from the session's catalog rather than handed in as a value: a
    // command registered while the server is running must be decodable at once,
    // and a snapshot taken at construction could not be (R8).
    CommandArgumentCodecRegistry argumentCodecs;
    HttpEditorConnectionPolicy& policy;
    HttpEditorRouteConfig config;
    Http::Server& server;
    std::recursive_mutex processingMutex;
    std::mutex connectionsMutex;
    std::map<Http::WebSocketHandle, std::shared_ptr<Connection>> connections;
    std::map<ReplayKey, std::deque<ReplayRecord>> replay;
};

HttpEditorRoute::HttpEditorRoute(
    Http::Server& server, EditorRuntime& runtime,
    HttpEditorConnectionPolicy& policy, HttpEditorRouteConfig config)
    : impl_{std::make_unique<Impl>(server, runtime, policy,
                                   std::move(config))} {}

HttpEditorRoute::~HttpEditorRoute() = default;

struct HttpEditorServer::Impl {
    Impl(EditorRuntime& runtime,
         HttpEditorConnectionPolicy& policy, HttpEditorServerConfig config)
        : server{config.port},
          route{server, runtime, policy,
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
    EditorRuntime& runtime,
    HttpEditorConnectionPolicy& policy, HttpEditorServerConfig config)
    : impl_{std::make_unique<Impl>(
          runtime, policy, std::move(config))} {}

HttpEditorServer::~HttpEditorServer() = default;

void HttpEditorServer::start() { impl_->start(); }
void HttpEditorServer::stop() { impl_->stop(); }

}  // namespace ssg
