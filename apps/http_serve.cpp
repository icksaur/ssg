#include "http_serve.h"

#include <ssg/EditorRuntime.h>
#include <ssg/HttpEditorServer.h>

#include <http.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include <poll.h>

namespace ssg::app {

std::string_view embeddedWebAsset(std::string_view key);

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

struct WebAsset {
    std::string_view route;
    std::string_view key;
    std::string_view contentType;
};

constexpr std::array<WebAsset, 4> kWebAssets{{
    {"/", "index.html", "text/html"},
    {"/client.mjs", "client.mjs", "text/javascript"},
    {"/reconcile.mjs", "reconcile.mjs", "text/javascript"},
    {"/fuzzy.mjs", "fuzzy.mjs", "text/javascript"},
}};

class BrowserConnectionPolicy final : public HttpEditorConnectionPolicy {
public:
    [[nodiscard]] std::optional<AttachedSession> attach() override {
        return AttachedSession{
            SessionId{"browser"},
            InvocationPrincipal{ClientId{1}, InvocationOrigin::Websocket, {}},
            ViewId{1}};
    }
};

}  // namespace

int run_http_server(EditorRuntime& runtime, unsigned short port) {
    ClientId const setupClient{1};
    if (!runtime
             .attach({setupClient, InvocationOrigin::InProcess}, ViewId{1})
             .accepted()) {
        std::fprintf(stderr, "ssg: --http setup attach failed\n");
        return 1;
    }
    auto const initialized = runtime.dispatch(
        setupClient, {"file.new", runtime.revision(), {}});
    (void)runtime.detach(setupClient);
    if (!initialized.accepted()) {
        std::fprintf(stderr, "ssg: --http failed to open a buffer\n");
        return 1;
    }

    Http::Server server{port, Http::BindAddress::loopback};
    for (auto const& asset : kWebAssets) {
        server.get(std::string{asset.route}, [asset](Http::Context&) {
            return Http::Ok(std::string{embeddedWebAsset(asset.key)},
                            std::string{asset.contentType});
        });
    }
    BrowserConnectionPolicy policy;
    HttpEditorRoute route{server, runtime, policy, {.route = "/session"}};

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    runtime.primeDeferred();
    try {
        server.start();
    } catch (std::exception const& error) {
        std::fprintf(stderr, "ssg: --http failed: %s\n", error.what());
        return 1;
    }
    std::fprintf(stderr,
                 "ssg: serving http://127.0.0.1:%u/  (Ctrl-C to stop)\n",
                 static_cast<unsigned>(port));

    while (!g_stop.load()) {
        int const wake = runtime.gitDiffWakeDescriptor();
        if (wake == -1) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        } else {
            pollfd ready{wake, POLLIN, 0};
            (void)::poll(&ready, 1, 100);
        }
        (void)runtime.pump();
        (void)runtime.flushDueAutosaveDrafts();
        route.publish();
    }
    (void)runtime.flushAllAutosaveDrafts();
    route.publish();
    server.stop();
    return 0;
}

}  // namespace ssg::app
