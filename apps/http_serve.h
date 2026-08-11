// The `ssg --http PORT` entry point: serve the editor over HTTP/WebSocket on a
// loopback port from the same binary as the TUI.  The library owns all editor
// behavior; this host owns only the transport and the browser stub page.

#pragma once

namespace ssg {
class EditorRuntime;
}

namespace ssg::app {

// Serve `runtime` over a loopback HTTP/WebSocket listener on `port` until a
// termination signal (SIGINT/SIGTERM) arrives, then return a process exit code.
// The runtime must outlive the call; the server is stopped before returning.
[[nodiscard]] int run_http_server(EditorRuntime& runtime, unsigned short port);

}  // namespace ssg::app
