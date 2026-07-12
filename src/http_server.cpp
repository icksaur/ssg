#include <ssg/http_server.h>

#include <http.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace ssg {

struct HttpEditorServer::Impl {
    struct Connection {
        ClientId client_id;
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<std::string> queue;
        bool stopping{false};
        std::thread writer;
    };

    Impl(CoreEditorSlice& editor_slice, HttpEditorServerConfig server_config)
        : slice{editor_slice},
          config{std::move(server_config)},
          server{config.port} {
        if (config.route.empty() || config.route.front() != '/') {
            throw std::invalid_argument{
                "WebSocket route must start with a slash"};
        }
        if (config.outbound_queue_messages == 0 ||
            config.write_timeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument{
                "WebSocket queue and write timeout must be positive"};
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
        if (started) stop();
    }

    void opened(Http::WebSocketHandle handle) {
        auto connection = std::make_shared<Connection>();
        connection->client_id = ClientId{handle};
        if (!slice.attach({connection->client_id,
                           InvocationOrigin::websocket})) {
            server.closeConnection(handle);
            return;
        }
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

        SliceResponse response;
        if (message.opcode != 0x1) {
            response = {ProtocolError::malformed_message,
                        CommandError::none, slice.snapshot(), std::nullopt,
                        "only text WebSocket messages are accepted"};
        } else {
            auto decoded =
                decode_insert_request(message.data, config.protocol_limits);
            if (!decoded.accepted()) {
                response = {decoded.error, CommandError::none,
                            slice.snapshot(), std::nullopt,
                            std::move(decoded.message)};
            } else {
                response = slice.execute(connection->client_id,
                                         *decoded.request);
            }
        }
        enqueue(handle, connection, encode_slice_response(response));
    }

    void enqueue(Http::WebSocketHandle handle,
                 std::shared_ptr<Connection> const& connection,
                 std::string payload) {
        bool overflow = false;
        {
            std::lock_guard lock{connection->mutex};
            if (connection->stopping) return;
            if (connection->queue.size() >=
                config.outbound_queue_messages) {
                connection->stopping = true;
                overflow = true;
            } else {
                connection->queue.push_back(std::move(payload));
            }
        }
        connection->ready.notify_one();
        if (overflow) server.closeConnection(handle);
    }

    void write_loop(Http::WebSocketHandle handle,
                    std::shared_ptr<Connection> const& connection) {
        for (;;) {
            std::string payload;
            {
                std::unique_lock lock{connection->mutex};
                connection->ready.wait(lock, [&] {
                    return connection->stopping || !connection->queue.empty();
                });
                if (connection->queue.empty()) {
                    return;
                }
                payload = std::move(connection->queue.front());
                connection->queue.pop_front();
            }
            auto const result = server.send(
                handle, payload,
                std::chrono::steady_clock::now() + config.write_timeout);
            if (!result) {
                {
                    std::lock_guard lock{connection->mutex};
                    connection->stopping = true;
                }
                server.closeConnection(handle);
                return;
            }
        }
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
        if (connection->writer.joinable()) connection->writer.join();
        (void)slice.detach(connection->client_id);
    }

    std::shared_ptr<Connection> find(Http::WebSocketHandle handle) {
        std::lock_guard lock{connections_mutex};
        auto const found = connections.find(handle);
        return found == connections.end() ? nullptr : found->second;
    }

    void start() {
        if (started) throw std::logic_error{"HTTP editor server is already started"};
        server.start();
        started = true;
    }

    void stop() {
        if (!started) return;
        server.stop();
        started = false;
    }

    CoreEditorSlice& slice;
    HttpEditorServerConfig config;
    Http::Server server;
    std::mutex connections_mutex;
    std::unordered_map<Http::WebSocketHandle,
                       std::shared_ptr<Connection>>
        connections;
    bool started{false};
};

HttpEditorServer::HttpEditorServer(CoreEditorSlice& slice,
                                   HttpEditorServerConfig config)
    : impl_{std::make_unique<Impl>(slice, std::move(config))} {}

HttpEditorServer::~HttpEditorServer() = default;

void HttpEditorServer::start() { impl_->start(); }
void HttpEditorServer::stop() { impl_->stop(); }

}  // namespace ssg
