#include <ssg/HttpEditorServer.h>
#include <ssg/EditorSession.h>
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
        // Serializes messages and close callbacks for this connection only.
        // Runtime-wide command ordering belongs to EditorSession.
        std::recursive_mutex receiveMutex;
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
    std::optional<Revision> lastPublishedRevision;

    Impl(Http::Server& httpServer, EditorSession& editorRuntime,
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
        if (config.outboundQueueMessages < 2 || config.replayDeltas == 0 ||
            config.writeTimeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument{
                "WebSocket queue must admit a response pair; replay and write "
                "timeout must be positive"};
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

        std::lock_guard receiveLock{connection->receiveMutex};
        std::optional<ClientId> clientId;
        std::optional<SessionId> sessionId;
        {
            std::lock_guard stateLock{connection->mutex};
            if (connection->binding) {
                clientId = connection->binding->principal.clientId();
                sessionId = connection->binding->sessionId;
            }
        }
        if (!clientId) {
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
            auto const revisionBefore = runtime.revision();
            auto const result = runtime.dispatch(
                *clientId, *command.command);
            std::vector<Outbound> response;
            if (result.accepted()) {
                (void)runtime.pump();
            }
            if (result.accepted() || result.revision != revisionBefore) {
                if (auto state = publishSession(*sessionId, handle)) {
                    response.push_back(std::move(*state));
                }
            }
            response.push_back(
                {ProtocolCodec{}.encodeCommandResult(result), true});
            enqueueBatch(handle, connection, std::move(response));
            return;
        }

        auto input = ProtocolCodec{}.decodeClientInput(
            message.data, config.protocolLimits);
        if (input.accepted()) {
            auto const revisionBefore = runtime.revision();
            auto const result = runtime.input(*clientId, *input.input);
            std::vector<Outbound> response;
            if (result.command && result.command->accepted()) {
                (void)runtime.pump();
            }
            if (result.command &&
                (result.command->accepted() ||
                 result.command->revision != revisionBefore)) {
                if (auto state = publishSession(*sessionId, handle)) {
                    response.push_back(std::move(*state));
                }
            }
            response.push_back(
                {ProtocolCodec{}.encodeClientInputResult(result), true});
            enqueueBatch(handle, connection, std::move(response));
            return;
        }

        auto status = ProtocolCodec{}.decodeStatusActionInvocation(message.data,
                                                       config.protocolLimits);
        if (status.accepted()) {
            try {
                auto const revisionBefore = runtime.revision();
                auto const result = runtime.dispatch(
                    *clientId,
                    {"status.invoke_action", runtime.revision(),
                     *status.invocation});
                std::vector<Outbound> response;
                if (result.accepted()) {
                    (void)runtime.pump();
                }
                if (result.accepted() || result.revision != revisionBefore) {
                    if (auto state = publishSession(*sessionId, handle)) {
                        response.push_back(std::move(*state));
                    }
                }
                response.push_back(
                    {ProtocolCodec{}.encodeCommandResult(result), true});
                enqueueBatch(handle, connection, std::move(response));
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
        auto attachResult =
            runtime.attach(attached->principal, attached->viewId);
        if (attachResult.error == AttachError::DuplicateClient) {
            // A reconnect can arrive before the server thread has observed the
            // prior socket's close. The host-authenticated client identity is
            // authoritative, so retire that stale binding before retrying.
            // The new connection has no binding yet, so findClient cannot expose
            // it as another reconnect's "previous" connection: receive-mutex
            // acquisition is one-way from an unbound connection to its old bound
            // connection and cannot form an old-to-new cycle.
            if (auto previous = findClient(clientId)) {
                close(previous->first, previous->second);
                attachResult =
                    runtime.attach(attached->principal, attached->viewId);
            }
        }
        if (!attachResult.accepted()) return false;

        std::lock_guard publishLock{publishMutex};
        (void)runtime.pump();
        auto snapshot = runtime.snapshot(clientId);
        if (!snapshot) {
            (void)runtime.detach(clientId);
            return false;
        }
        auto const currentRevision = snapshot->revision();

        bool replayed = false;
        std::vector<std::string> outbound;
        if (request.request->lastAppliedRevision) {
            auto next = *request.request->lastAppliedRevision;
            if (next == currentRevision) {
                replayed = true;
            } else {
                auto const key = replayKey(*attached);
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
                        outbound.push_back(std::move(encoded));
                    }
                    replayed = true;
                }
            }
        }
        if (!replayed) {
            outbound.push_back(
                ProtocolCodec{}.encodeSessionSnapshot(*snapshot));
        }
        {
            std::lock_guard stateLock{connection->mutex};
            connection->binding.emplace(std::move(*attached));
            connection->snapshot.emplace(std::move(*snapshot));
        }
        for (auto& encoded : outbound) {
            enqueue(handle, connection, {std::move(encoded), true});
        }
        return true;
    }

    std::optional<Outbound> publishSession(
        SessionId const& sessionId,
        std::optional<Http::WebSocketHandle> deferHandle = std::nullopt) {
        std::vector<Http::WebSocketHandle> closeAfterPublish;
        std::optional<Outbound> deferred;
        std::unique_lock publishLock{publishMutex};
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
            std::unique_lock stateLock{connection->mutex};
            if (!connection->binding) continue;
            auto current = runtime.snapshot(
                connection->binding->principal.clientId());
            if (!current) {
                stateLock.unlock();
                closeAfterPublish.push_back(handle);
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
            stateLock.unlock();
            if (deferHandle && handle == *deferHandle) {
                deferred.emplace(Outbound{std::move(encoded), true});
            } else {
                enqueue(handle, connection, {std::move(encoded), true});
            }
        }
        publishLock.unlock();
        for (auto handle : closeAfterPublish) {
            server.closeConnection(handle);
        }
        return deferred;
    }

    void publish() {
        const auto revision = runtime.revision();
        if (lastPublishedRevision == revision) return;
        std::vector<SessionId> sessions;
        {
            std::lock_guard lock{connectionsMutex};
            for (auto const& [_, connection] : connections) {
                std::lock_guard connectionLock{connection->mutex};
                if (!connection->binding || connection->stopping) continue;
                auto const& id = connection->binding->sessionId;
                if (std::find(sessions.begin(), sessions.end(), id) ==
                    sessions.end()) {
                    sessions.push_back(id);
                }
            }
        }
        for (auto const& session : sessions) {
            publishSession(session);
        }
        lastPublishedRevision = revision;
    }

    ReplayKey replayKey(AttachedSession const& binding) const {
        return {std::string{binding.sessionId.value()},
                binding.principal.clientId().value()};
    }

    void enqueue(Http::WebSocketHandle handle,
                 std::shared_ptr<Connection> const& connection,
                 Outbound outbound) {
        std::vector<Outbound> batch;
        batch.push_back(std::move(outbound));
        enqueueBatch(handle, connection, std::move(batch));
    }

    void enqueueBatch(Http::WebSocketHandle handle,
                      std::shared_ptr<Connection> const& connection,
                      std::vector<Outbound> batch) {
        if (batch.empty()) return;
        bool overflow = false;
        {
            std::lock_guard lock{connection->mutex};
            if (connection->stopping) return;
            if (connection->queue.size() >
                    config.outboundQueueMessages ||
                batch.size() > config.outboundQueueMessages -
                                   connection->queue.size()) {
                connection->stopping = true;
                overflow = true;
            } else {
                for (auto& outbound : batch) {
                    connection->queue.push_back(std::move(outbound));
                }
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
        std::lock_guard receiveLock{connection->receiveMutex};
        std::optional<ClientId> detachClient;
        {
            std::lock_guard stateLock{connection->mutex};
            connection->stopping = true;
            if (connection->binding && !connection->detached) {
                detachClient =
                    connection->binding->principal.clientId();
                connection->detached = true;
            }
        }
        connection->ready.notify_one();
        if (detachClient) (void)runtime.detach(*detachClient);
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
            std::lock_guard stateLock{connection->mutex};
            connection->stopping = true;
        }
        connection->ready.notify_one();
        if (connection->writer.joinable() &&
            connection->writer.get_id() != std::this_thread::get_id()) {
            connection->writer.join();
        }
        std::lock_guard receiveLock{connection->receiveMutex};
        std::optional<ClientId> detachClient;
        {
            std::lock_guard stateLock{connection->mutex};
            if (connection->binding && !connection->detached) {
                detachClient =
                    connection->binding->principal.clientId();
                connection->detached = true;
            }
        }
        if (detachClient) (void)runtime.detach(*detachClient);
    }

    std::shared_ptr<Connection> find(Http::WebSocketHandle handle) {
        std::lock_guard lock{connectionsMutex};
        auto const found = connections.find(handle);
        return found == connections.end() ? nullptr : found->second;
    }

    std::optional<std::pair<Http::WebSocketHandle,
                            std::shared_ptr<Connection>>>
    findClient(ClientId clientId) {
        std::lock_guard lock{connectionsMutex};
        for (auto const& [handle, connection] : connections) {
            std::lock_guard connectionLock{connection->mutex};
            if (connection->binding &&
                !connection->stopping &&
                connection->binding->principal.clientId() == clientId) {
                return std::pair{handle, connection};
            }
        }
        return std::nullopt;
    }

    EditorSession& runtime;
    // Derived from the session's catalog rather than handed in as a value: a
    // command registered while the server is running must be decodable at once,
    // and a snapshot taken at construction could not be (R8).
    CommandArgumentCodecRegistry argumentCodecs;
    HttpEditorConnectionPolicy& policy;
    HttpEditorRouteConfig config;
    Http::Server& server;
    std::mutex publishMutex;
    std::mutex connectionsMutex;
    std::map<Http::WebSocketHandle, std::shared_ptr<Connection>> connections;
    std::map<ReplayKey, std::deque<ReplayRecord>> replay;
};

HttpEditorRoute::HttpEditorRoute(
    Http::Server& server, EditorSession& runtime,
    HttpEditorConnectionPolicy& policy, HttpEditorRouteConfig config)
    : impl_{std::make_unique<Impl>(server, runtime, policy,
                                   std::move(config))} {}

HttpEditorRoute::~HttpEditorRoute() = default;

void HttpEditorRoute::publish() { impl_->publish(); }

struct HttpEditorServer::Impl {
    Impl(EditorSession& runtime,
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
    EditorSession& runtime,
    HttpEditorConnectionPolicy& policy, HttpEditorServerConfig config)
    : impl_{std::make_unique<Impl>(
          runtime, policy, std::move(config))} {}

HttpEditorServer::~HttpEditorServer() = default;

void HttpEditorServer::start() { impl_->start(); }
void HttpEditorServer::stop() { impl_->stop(); }

}  // namespace ssg
