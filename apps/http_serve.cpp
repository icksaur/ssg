#include "http_serve.h"

#include <ssg/EditorRuntime.h>
#include <ssg/HttpEditorServer.h>
#include <ssg/Protocol.h>
#include <ssg/TextInputCommands.h>
#include <ssg/session_snapshot.h>

#include <http.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ssg::app {

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

// A minimal browser client: it opens the WebSocket, sends the real attach
// preamble, decodes the semantic snapshot's ProtocolValue tree, renders the
// document text, and forwards single-character keystrokes as TYPE frames.  The
// tag decoder mirrors protocol/schema/README.md's wire encoding.  Full DOM/CSS
// presentation of the semantic model is M2; this proves the same-binary
// serve/attach/round-trip seam.
constexpr char kPage[] = R"HTML(<!doctype html>
<meta charset="utf-8">
<title>ssg</title>
<body style="font:13px/1.4 monospace;margin:0">
<div id="status" style="padding:4px 8px;border-bottom:1px solid">connecting...</div>
<pre id="doc" tabindex="0" style="margin:0;padding:8px;white-space:pre-wrap;outline:none;min-height:80vh"></pre>
<script>
const statusEl = document.getElementById('status');
const docEl = document.getElementById('doc');

// Decode one tagged ProtocolValue from a DataView at {p}. Returns [value,next].
function decodeValue(dv, p) {
  const tag = dv.getUint8(p); p += 1;
  switch (tag) {
    case 0: return [null, p];
    case 1: return [dv.getUint8(p) !== 0, p + 1];
    case 2: { const v = dv.getBigInt64(p, true); return [v, p + 8]; }
    case 3: { const v = dv.getBigUint64(p, true); return [v, p + 8]; }
    case 4: case 5: {
      const n = dv.getUint32(p, true); p += 4;
      const bytes = new Uint8Array(dv.buffer, dv.byteOffset + p, n); p += n;
      if (tag === 4) return [new TextDecoder().decode(bytes), p];
      return [bytes.slice(), p];
    }
    case 6: {
      const n = dv.getUint32(p, true); p += 4; const arr = [];
      for (let i = 0; i < n; i++) { const [v, np] = decodeValue(dv, p); arr.push(v); p = np; }
      return [arr, p];
    }
    case 7: {
      const n = dv.getUint32(p, true); p += 4; const obj = {};
      for (let i = 0; i < n; i++) {
        const kl = dv.getUint32(p, true); p += 4;
        const key = new TextDecoder().decode(new Uint8Array(dv.buffer, dv.byteOffset + p, kl)); p += kl;
        const [v, np] = decodeValue(dv, p); obj[key] = v; p = np;
      }
      return [obj, p];
    }
    default: throw new Error('bad tag ' + tag + ' at ' + (p - 1));
  }
}

// Recursively find the first {document:{text:...}} in the decoded tree.
function findDocumentText(node) {
  if (Array.isArray(node)) {
    for (const item of node) { const f = findDocumentText(item); if (f !== null) return f; }
  } else if (node && typeof node === 'object') {
    if (node.document && typeof node.document === 'object' &&
        typeof node.document.text === 'string') {
      return node.document.text;
    }
    for (const k of Object.keys(node)) { const f = findDocumentText(node[k]); if (f !== null) return f; }
  }
  return null;
}

let serverText = '';
const ws = new WebSocket('ws://' + location.host + '/session');
ws.binaryType = 'arraybuffer';

ws.onopen = () => { statusEl.textContent = 'attached; awaiting snapshot'; ws.send('SSG1 ATTACH -'); };
ws.onmessage = (e) => {
  if (typeof e.data === 'string') { statusEl.textContent = e.data; return; }
  try {
    const dv = new DataView(e.data);
    const [tree] = decodeValue(dv, 2);
    const text = findDocumentText(tree);
    if (text !== null) serverText = text;
    docEl.textContent = serverText;
    statusEl.textContent = 'live (' + e.data.byteLength + ' bytes)';
    docEl.focus();
  } catch (err) { statusEl.textContent = 'decode error: ' + err.message; }
};
ws.onclose = () => { statusEl.textContent += ' [closed]'; };
ws.onerror = () => { statusEl.textContent = 'ws error'; };

docEl.addEventListener('keydown', (ev) => {
  if (ev.key.length === 1 && !ev.ctrlKey && !ev.metaKey) {
    ev.preventDefault();
    ws.send('TYPE:' + ev.key);
  }
});
</script>
)HTML";

}  // namespace

int run_http_server(EditorRuntime& runtime, unsigned short port) {
    // Attach the single browser client through the runtime's own API so
    // follow-model registration and every per-client setup runs, exactly as the
    // TUI's InProcess attach does.
    ClientId const client{1};
    if (!runtime.attach({client, InvocationOrigin::Websocket, {}}, ViewId{1})
             .accepted()) {
        std::fprintf(stderr, "ssg: --http attach failed\n");
        return 1;
    }
    // An always-editable buffer to type into, mirroring the TUI's startup.
    if (!runtime.dispatch(client, {"file.new", runtime.revision(), {}})
             .accepted()) {
        std::fprintf(stderr, "ssg: --http failed to open a buffer\n");
        return 1;
    }

    Http::Server server{port, Http::BindAddress::loopback};

    // M1 serves exactly one browser, sharing the single attached ClientId; a
    // second concurrent attach is refused explicitly rather than silently
    // mutating the first client's state. Multi-client is a later milestone.
    auto attached = std::make_shared<std::atomic<bool>>(false);

    // The browser lays out natively, so it consumes the dimensionless semantic
    // snapshot -- no grid projection is sent.
    auto sendSnapshot = [&runtime, &server, client](Http::WebSocketHandle handle) {
        auto snapshot = runtime.snapshot(client);
        if (!snapshot) {
            (void)server.send(handle, std::string{"no snapshot for client"});
            return;
        }
        auto const bytes = ProtocolCodec{}.encodeSessionSnapshot(*snapshot);
        (void)server.send(
            handle, std::vector<std::uint8_t>{bytes.begin(), bytes.end()});
    };

    server.get("/", [](Http::Context&) {
        return Http::Ok(std::string{kPage}, "text/html");
    });
    server.ws(
        "/session",
        Http::WebSocketHandler{
            .onOpen = {},
            .onMessage =
                [&runtime, &server, client, sendSnapshot, attached](
                    Http::WebSocketHandle handle,
                    Http::WebSocketMessage message) {
                    std::string_view const payload{message.data};
                    if (payload.rfind("TYPE:", 0) == 0) {
                        (void)runtime.dispatch(
                            client,
                            {"text.insert", runtime.revision(),
                             TextInputArguments{std::string{payload.substr(5)}}});
                        sendSnapshot(handle);
                        return;
                    }
                    auto const attach = decodeSessionAttachRequest(message.data);
                    if (!attach.accepted()) {
                        return;
                    }
                    bool expected = false;
                    if (!attached->compare_exchange_strong(expected, true)) {
                        (void)server.send(
                            handle,
                            std::string{"a client is already attached"});
                        return;
                    }
                    sendSnapshot(handle);
                },
            .onClose =
                [attached](Http::WebSocketHandle) {
                    attached->store(false);
                },
        });

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
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
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    server.stop();
    return 0;
}

}  // namespace ssg::app
