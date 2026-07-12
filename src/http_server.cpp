#include <ssg/http_server.h>

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

char hex_digit(unsigned value) {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + value - 10);
}

std::string hex_encode(std::string_view value) {
    std::string result;
    result.reserve(value.size() * 2);
    for (unsigned char byte : value) {
        result.push_back(hex_digit(byte >> 4));
        result.push_back(hex_digit(byte & 0x0f));
    }
    return result;
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<std::string> hex_decode(std::string_view value) {
    if ((value.size() & 1u) != 0) return std::nullopt;
    std::string result;
    result.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        auto const high = hex_value(value[index]);
        auto const low = hex_value(value[index + 1]);
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

std::string encode_session_attach_request(SessionAttachRequest const& request) {
    return "SSG1 ATTACH " +
           (request.last_applied_revision
                ? std::to_string(request.last_applied_revision->value())
                : std::string{"-"}) +
           " " + hex_encode(request.credential);
}

DecodeSessionAttachRequestResult decode_session_attach_request(
    std::string_view message, ProtocolLimits limits) {
    if (message.size() > limits.max_message_bytes) {
        return {ProtocolError::message_too_large, std::nullopt,
                "attach request exceeds message limit"};
    }
    auto const fields = split(message);
    if (fields.size() != 4 || fields[0] != "SSG1" ||
        fields[1] != "ATTACH") {
        return {ProtocolError::malformed_message, std::nullopt,
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
            return {ProtocolError::malformed_message, std::nullopt,
                    "invalid last-applied revision"};
        }
        revision.emplace(value);
    }
    auto credential = hex_decode(fields[3]);
    if (!credential) {
        return {ProtocolError::malformed_message, std::nullopt,
                "invalid credential encoding"};
    }
    return {ProtocolError::none,
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
        Revision base_revision;
        Revision revision;
        std::string payload;
    };

    using ReplayKey = std::pair<std::string, std::uint64_t>;

    Impl(Http::Server& http_server, EditorSession& editor_session,
         CommandArgumentCodecRegistry command_argument_codecs,
         HttpEditorSessionHost& session_host,
         HttpEditorRouteConfig route_config)
        : session{editor_session},
          argument_codecs{std::move(command_argument_codecs)},
          host{session_host},
          config{std::move(route_config)},
          server{http_server} {
        if (config.route.empty() || config.route.front() != '/') {
            throw std::invalid_argument{
                "WebSocket route must start with a slash"};
        }
        if (config.outbound_queue_messages == 0 || config.replay_deltas == 0 ||
            config.write_timeout <= std::chrono::milliseconds::zero()) {
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
            std::lock_guard lock{connections_mutex};
            connections.emplace(handle, connection);
        }
        connection->writer =
            std::thread{[this, handle, connection] { write_loop(handle, connection); }};
    }

    void received(Http::WebSocketHandle handle,
                  Http::WebSocketMessage message) {
        auto connection = find(handle);
        if (!connection) return;

        std::lock_guard process_lock{processing_mutex};
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

        auto command = decode_command_request(
            message.data, argument_codecs, config.protocol_limits);
        if (command.accepted()) {
            auto const result = session.dispatch(
                connection->binding->principal.client_id(), *command.command);
            if (!result.accepted()) {
                enqueue(handle, connection,
                        {encode_command_result(result), true});
                return;
            }
            publish_session(connection->binding->session_id);
            return;
        }

        auto clipboard =
            decode_clipboard_response(message.data, config.protocol_limits);
        if (clipboard.accepted()) {
            try {
                host.clipboard_response(
                    connection->binding->session_id,
                    connection->binding->principal.client_id(),
                    *clipboard.response);
                publish_session(connection->binding->session_id);
            } catch (...) {
                close(handle, connection);
            }
            return;
        }
        auto status = decode_status_action_invocation(message.data,
                                                       config.protocol_limits);
        if (status.accepted()) {
            try {
                host.status_action(connection->binding->session_id,
                                   connection->binding->principal.client_id(),
                                   *status.invocation);
                publish_session(connection->binding->session_id);
            } catch (...) {
                close(handle, connection);
            }
            return;
        }
        auto binary = decode_binary_frame(message.data, config.protocol_limits);
        if (binary.accepted()) {
            try {
                host.binary(connection->binding->session_id,
                            connection->binding->principal.client_id(),
                            *binary.frame);
                publish_session(connection->binding->session_id);
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
            decode_session_attach_request(payload, config.protocol_limits);
        if (!request.accepted()) return false;
        auto authenticated = host.authenticate(request.request->credential);
        if (!authenticated) return false;
        if (authenticated->principal.origin() != InvocationOrigin::websocket) {
            return false;
        }
        auto const client_id = authenticated->principal.client_id();
        auto const attached =
            session.attach(authenticated->principal, authenticated->view_id);
        if (!attached.accepted()) return false;

        connection->binding.emplace(std::move(*authenticated));
        connection->snapshot.emplace(host.snapshot(
            connection->binding->session_id, client_id));
        auto const current_revision = connection->snapshot->revision();

        bool replayed = false;
        if (request.request->last_applied_revision) {
            auto next = *request.request->last_applied_revision;
            if (next == current_revision) {
                replayed = true;
            } else {
                auto const key = replay_key(*connection->binding);
                auto const found = replay.find(key);
                std::vector<std::string> chain;
                if (found != replay.end()) {
                    for (auto const& record : found->second) {
                        if (record.base_revision == next) {
                            chain.push_back(record.payload);
                            next = record.revision;
                        }
                    }
                }
                if (next == current_revision && !chain.empty() &&
                    chain.size() <= config.outbound_queue_messages) {
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
                    {encode_session_snapshot(*connection->snapshot), true});
        }
        return true;
    }

    void publish_session(SessionId const& session_id) {
        std::vector<std::pair<Http::WebSocketHandle,
                              std::shared_ptr<Connection>>>
            targets;
        {
            std::lock_guard lock{connections_mutex};
            for (auto const& [handle, connection] : connections) {
                std::lock_guard connection_lock{connection->mutex};
                if (connection->binding && !connection->stopping &&
                    connection->binding->session_id == session_id) {
                    targets.emplace_back(handle, connection);
                }
            }
        }
        for (auto const& [handle, connection] : targets) {
            auto current = host.snapshot(
                connection->binding->session_id,
                connection->binding->principal.client_id());
            if (!connection->snapshot ||
                connection->snapshot->revision() == current.revision()) {
                connection->snapshot.emplace(std::move(current));
                continue;
            }
            auto delta = derive_session_delta(*connection->snapshot, current);
            auto encoded = encode_session_delta(delta);
            auto& history = replay[replay_key(*connection->binding)];
            history.push_back({delta.base_revision(), delta.revision(), encoded});
            while (history.size() > config.replay_deltas) history.pop_front();
            connection->snapshot.emplace(std::move(current));
            enqueue(handle, connection, {std::move(encoded), true});
        }
    }

    ReplayKey replay_key(AuthenticatedSession const& binding) const {
        return {std::string{binding.session_id.value()},
                binding.principal.client_id().value()};
    }

    void enqueue(Http::WebSocketHandle handle,
                 std::shared_ptr<Connection> const& connection,
                 Outbound outbound) {
        bool overflow = false;
        {
            std::lock_guard lock{connection->mutex};
            if (connection->stopping) return;
            if (connection->queue.size() >=
                config.outbound_queue_messages) {
                connection->stopping = true;
                overflow = true;
            } else {
                connection->queue.push_back(std::move(outbound));
            }
        }
        connection->ready.notify_one();
        if (overflow) close(handle, connection);
    }

    void write_loop(Http::WebSocketHandle handle,
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
                std::chrono::steady_clock::now() + config.write_timeout;
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
            std::lock_guard process_lock{processing_mutex};
            if (connection->binding && !connection->detached) {
                (void)session.detach(
                    connection->binding->principal.client_id());
                connection->detached = true;
            }
        }
        server.closeConnection(handle);
    }

    void closed(Http::WebSocketHandle handle) {
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard lock{connections_mutex};
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
        std::lock_guard process_lock{processing_mutex};
        if (connection->binding && !connection->detached) {
            (void)session.detach(connection->binding->principal.client_id());
            connection->detached = true;
        }
    }

    std::shared_ptr<Connection> find(Http::WebSocketHandle handle) {
        std::lock_guard lock{connections_mutex};
        auto const found = connections.find(handle);
        return found == connections.end() ? nullptr : found->second;
    }

    std::shared_ptr<Connection> find(ClientId client_id) {
        std::lock_guard lock{connections_mutex};
        for (auto const& [handle, connection] : connections) {
            (void)handle;
            std::lock_guard connection_lock{connection->mutex};
            if (connection->binding &&
                !connection->stopping &&
                connection->binding->principal.client_id() == client_id) {
                return connection;
            }
        }
        return nullptr;
    }

    bool send_to(ClientId client_id, std::string payload) {
        std::lock_guard process_lock{processing_mutex};
        auto connection = find(client_id);
        if (!connection) return false;
        Http::WebSocketHandle handle = 0;
        {
            std::lock_guard lock{connections_mutex};
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
    CommandArgumentCodecRegistry argument_codecs;
    HttpEditorSessionHost& host;
    HttpEditorRouteConfig config;
    Http::Server& server;
    std::recursive_mutex processing_mutex;
    std::mutex connections_mutex;
    std::map<Http::WebSocketHandle, std::shared_ptr<Connection>> connections;
    std::map<ReplayKey, std::deque<ReplayRecord>> replay;
};

HttpEditorRoute::HttpEditorRoute(
    Http::Server& server, EditorSession& session,
    CommandArgumentCodecRegistry argument_codecs,
    HttpEditorSessionHost& host, HttpEditorRouteConfig config)
    : impl_{std::make_unique<Impl>(server, session, std::move(argument_codecs),
                                  host, std::move(config))} {}

HttpEditorRoute::~HttpEditorRoute() = default;

bool HttpEditorRoute::send_clipboard_request(
    ClientId client_id, ClipboardRequest const& request) {
    return impl_->send_to(client_id, encode_clipboard_request(request));
}

bool HttpEditorRoute::send_binary(ClientId client_id,
                                  BinaryFrame const& frame) {
    return impl_->send_to(client_id, encode_binary_frame(frame));
}

struct HttpEditorServer::Impl {
    Impl(EditorSession& session,
         CommandArgumentCodecRegistry argument_codecs,
         HttpEditorSessionHost& host, HttpEditorServerConfig config)
        : server{config.port},
          route{server, session, std::move(argument_codecs), host,
                {std::move(config.route), config.outbound_queue_messages,
                 config.replay_deltas, config.write_timeout,
                 config.protocol_limits}} {}

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
    EditorSession& session, CommandArgumentCodecRegistry argument_codecs,
    HttpEditorSessionHost& host, HttpEditorServerConfig config)
    : impl_{std::make_unique<Impl>(
          session, std::move(argument_codecs), host, std::move(config))} {}

HttpEditorServer::~HttpEditorServer() = default;

void HttpEditorServer::start() { impl_->start(); }
void HttpEditorServer::stop() { impl_->stop(); }

bool HttpEditorServer::send_clipboard_request(
    ClientId client_id, ClipboardRequest const& request) {
    return impl_->route.send_clipboard_request(client_id, request);
}

bool HttpEditorServer::send_binary(ClientId client_id,
                                   BinaryFrame const& frame) {
    return impl_->route.send_binary(client_id, frame);
}

}  // namespace ssg
