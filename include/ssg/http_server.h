#pragma once

#include <ssg/protocol.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ssg {

struct HttpEditorServerConfig {
    std::uint16_t port;
    std::string route{"/session"};
    std::size_t outbound_queue_messages{32};
    std::chrono::milliseconds write_timeout{1000};
    ProtocolLimits protocol_limits{};
};

class HttpEditorServer {
public:
    HttpEditorServer(CoreEditorSlice& slice, HttpEditorServerConfig config);
    ~HttpEditorServer();

    HttpEditorServer(HttpEditorServer const&) = delete;
    HttpEditorServer& operator=(HttpEditorServer const&) = delete;

    void start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
