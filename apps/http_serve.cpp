#include "http_serve.h"

#include <ssg/EditorRuntime.h>
#include <ssg/CommandCatalog.h>
#include <ssg/CompiledKeymap.h>
#include <ssg/FindReplace.h>
#include <ssg/HttpEditorServer.h>
#include <ssg/KeyCode.h>
#include <ssg/Keymap.h>
#include <ssg/Protocol.h>
#include <ssg/PromptRouting.h>
#include <ssg/PromptSurface.h>
#include <ssg/StatusQueue.h>
#include <ssg/focus.h>
#include <ssg/session_snapshot.h>

#include <http.h>

#include <array>
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

// The browser client: opens the WebSocket, sends the real attach preamble,
// decodes the semantic snapshot's ProtocolValue tree, and renders it natively in
// the DOM -- document text colored by syntax scope, caret and selection from the
// semantic selection set, and a tab strip -- with every color drawn from the
// theme's roles mapped to CSS custom properties. Keystrokes go back as KEY
// frames. The tag decoder mirrors protocol/schema/README.md's wire encoding.
constexpr char kPage[] = R"HTML(<!doctype html>
<meta charset="utf-8">
<title>ssg</title>
<style>
  body { margin:0; font:13px/1.4 monospace; color:var(--ssg-text); background:var(--ssg-canvas); }
  #status { padding:2px 8px; border-bottom:1px solid; opacity:.6; }
  #tabs { display:flex; gap:1px; padding:2px 4px; border-bottom:1px solid; }
  #tabs .tab { padding:2px 8px; opacity:.55; }
  #tabs .tab.active { opacity:1; font-weight:bold; }
  #doc { margin:0; padding:8px; white-space:pre-wrap; outline:none; min-height:80vh; }
  #doc .sel { background:var(--ssg-selection); }
  #doc .caret { border-left:2px solid var(--ssg-caret); margin-left:-1px; }
</style>
<div id="status">connecting...</div>
<div id="tabs"></div>
<pre id="doc" tabindex="0"></pre>
<script>
const statusEl = document.getElementById('status');
const tabsEl = document.getElementById('tabs');
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

// Find the sections object: the first node with a document.text string. Its
// siblings (selection, syntax, tabs, theme, focus) are the semantic model.
function findSections(node) {
  if (Array.isArray(node)) {
    for (const item of node) { const f = findSections(item); if (f) return f; }
  } else if (node && typeof node === 'object') {
    if (node.document && typeof node.document === 'object' &&
        typeof node.document.text === 'string') return node;
    for (const k of Object.keys(node)) { const f = findSections(node[k]); if (f) return f; }
  }
  return null;
}

const num = (v) => typeof v === 'bigint' ? Number(v) : v;
const hex2 = (n) => (n & 255).toString(16).padStart(2, '0');
const cssColor = (c) => c ? ('#' + hex2(num(c.red)) + hex2(num(c.green)) + hex2(num(c.blue))) : '';
const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const idKey = (v) => JSON.stringify(v, (k, x) => typeof x === 'bigint' ? x.toString() : x);

// Map char-boundary byte offsets to UTF-16 string indices: document positions
// are UTF-8 byte offsets, JS slices by UTF-16 code unit.
function byteToIndex(text, offsets) {
  const want = new Set(offsets.map(num));
  const map = new Map();
  let byte = 0, idx = 0;
  if (want.has(0)) map.set(0, 0);
  for (const ch of text) {
    const cp = ch.codePointAt(0);
    byte += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    idx += ch.length;
    if (want.has(byte)) map.set(byte, idx);
  }
  return map;
}

// Role ordinals the renderer maps to CSS custom properties; pinned by
// test_theme's role-ordinal contract so a reorder cannot silently mis-color.
const ROLE = { text: 0, canvas: 1, caret: 2, selection: 3 };

function applyTheme(theme) {
  const root = document.documentElement.style;
  const rc = (theme && Array.isArray(theme.role_colors)) ? theme.role_colors : [];
  // Re-derive every property each snapshot, clearing any set by an earlier theme,
  // so a short or absent role table never leaves a stale color on screen.
  const set = (name, i) => { const c = cssColor(rc[i]); if (c) root.setProperty(name, c); else root.removeProperty(name); };
  set('--ssg-text', ROLE.text);
  set('--ssg-canvas', ROLE.canvas);
  set('--ssg-caret', ROLE.caret);
  set('--ssg-selection', ROLE.selection);
  return (theme && Array.isArray(theme.syntax_colors)) ? theme.syntax_colors : [];
}

function renderTabs(tabs) {
  tabsEl.textContent = '';
  if (!tabs || !Array.isArray(tabs.tabs)) return;
  const activeId = idKey(tabs.active);
  for (const t of tabs.tabs) {
    const el = document.createElement('span');
    el.className = 'tab' + (idKey(t.id) === activeId ? ' active' : '');
    el.textContent = (t.dirty ? '\u25CF ' : '') + (t.label || '');
    tabsEl.appendChild(el);
  }
}

// Segment the text at every syntax-span edge, selection edge, and the caret;
// color each segment by its syntax scope through the theme and mark selected
// segments and the caret. Geometry is the browser's; only offsets are semantic.
function renderDoc(sections, syntaxColors) {
  const text = sections.document.text;
  const caret = num(sections.document.caret);
  const spans = (sections.syntax && Array.isArray(sections.syntax.spans)) ? sections.syntax.spans : [];
  const sels = (sections.selection && Array.isArray(sections.selection.selections)) ? sections.selection.selections : [];

  const ranges = [];
  for (const s of sels) {
    const a = num(s.anchor.byte_offset), b = num(s.active.byte_offset);
    if (a !== b) ranges.push([Math.min(a, b), Math.max(a, b)]);
  }

  const total = new TextEncoder().encode(text).length;
  const bounds = new Set([0, total, caret]);
  for (const sp of spans) { bounds.add(num(sp.begin)); bounds.add(num(sp.end)); }
  for (const r of ranges) { bounds.add(r[0]); bounds.add(r[1]); }
  const cuts = [...bounds].filter(b => b >= 0 && b <= total).sort((a, b) => a - b);
  const map = byteToIndex(text, cuts);

  const scopeAt = (byte) => { for (const sp of spans) { if (byte >= num(sp.begin) && byte < num(sp.end)) return num(sp.scope); } return -1; };
  const selectedAt = (byte) => ranges.some(r => byte >= r[0] && byte < r[1]);

  let html = '';
  for (let i = 0; i + 1 < cuts.length; i++) {
    const a = cuts[i], b = cuts[i + 1];
    if (a === caret) html += '<span class="caret"></span>';
    const ia = map.get(a), ib = map.get(b);
    if (ia === undefined || ib === undefined || ib <= ia) continue;
    const scope = scopeAt(a);
    // Both span.scope and syntax_colors come from the same snapshot, so the
    // index is self-consistent (unlike the hardcoded role ordinals); the bounds
    // check only guards a truncated palette, degrading to the default color.
    const color = (scope >= 0 && scope < syntaxColors.length) ? cssColor(syntaxColors[scope]) : '';
    const cls = selectedAt(a) ? ' class="sel"' : '';
    const style = color ? ' style="color:' + color + '"' : '';
    html += '<span' + cls + style + '>' + esc(text.substring(ia, ib)) + '</span>';
  }
  if (caret >= total) html += '<span class="caret"></span>';
  docEl.innerHTML = html;
}

const ws = new WebSocket('ws://' + location.host + '/session');
ws.binaryType = 'arraybuffer';

ws.onopen = () => { statusEl.textContent = 'attached; awaiting snapshot'; ws.send('SSG1 ATTACH -'); };
ws.onmessage = (e) => {
  if (typeof e.data === 'string') { statusEl.textContent = e.data; return; }
  try {
    const dv = new DataView(e.data);
    const [tree] = decodeValue(dv, 2);
    const sections = findSections(tree);
    if (!sections) { statusEl.textContent = 'no sections in snapshot'; return; }
    const syntaxColors = applyTheme(sections.theme);
    renderTabs(sections.tabs);
    renderDoc(sections, syntaxColors);
    statusEl.textContent = 'live (' + e.data.byteLength + ' bytes)';
    docEl.focus();
  } catch (err) { statusEl.textContent = 'render error: ' + err.message; }
};
ws.onclose = () => { statusEl.textContent += ' [closed]'; };
ws.onerror = () => { statusEl.textContent = 'ws error'; };

docEl.addEventListener('keydown', (ev) => {
  // Send every keydown as code + modifiers + (printable text, if any); the host
  // resolves bindings and routes text through the one shared input seam.
  const mods = (ev.ctrlKey ? 'c' : '') + (ev.altKey ? 'a' : '') +
               (ev.metaKey ? 'm' : '') + (ev.shiftKey ? 's' : '');
  // Array.from counts Unicode scalars, so a supplementary-plane character (two
  // UTF-16 code units in ev.key) still registers as one printable scalar and is
  // sent, consistent with the UTF-8 offset contract.
  const printable = Array.from(ev.key).length === 1 && !ev.ctrlKey && !ev.metaKey;
  const text = printable ? ev.key : '';
  ev.preventDefault();
  ws.send('KEY:' + ev.code + ':' + mods + ':' + text);
});
</script>
)HTML";

// Map the runtime's live focus and active prompt onto the shared routing seam,
// so the web host makes the exact text-routing decision the TUI does. The
// palette query is the one client-owned derived view: the browser edits it
// locally, so an AppendPaletteQuery result dispatches nothing here.
void routeText(EditorRuntime& runtime, ClientId client, std::string const& text) {
    auto snapshot = runtime.snapshot(client);
    if (!snapshot) return;
    auto const& sections = snapshot->sections();
    PromptRoutingState state;
    state.focus = sections.focus;
    if (sections.promptStatus.activeKind) {
        switch (*sections.promptStatus.activeKind) {
        case PromptKind::Palette:
            state.prompt = ActivePrompt::Palette;
            break;
        case PromptKind::Find:
            state.prompt = ActivePrompt::Find;
            state.currentValue = sections.findReplace.query;
            break;
        case PromptKind::Replace:
            state.prompt = ActivePrompt::Replace;
            state.currentValue = sections.findReplace.replacement;
            break;
        case PromptKind::Path:
        case PromptKind::Settings:
        case PromptKind::CommandArgument:
            // The generic text prompts do not publish their current value in the
            // semantic snapshot, so routing prompt.update_value here with only
            // the new text would OVERWRITE the existing value, not append. Leave
            // the prompt unset (the seam then ignores the text) until that value
            // is in the snapshot and the DOM renderer shows these prompts; the
            // TUI, which holds the value app-side, is unaffected.
            break;
        }
    }
    auto const route = PromptTextRouter{}.route(state, text);
    switch (route.kind) {
    case PromptTextRoute::Kind::Dispatch:
        (void)runtime.dispatch(
            client, {route.command, runtime.revision(), route.payload});
        break;
    case PromptTextRoute::Kind::AppendPaletteQuery:
    case PromptTextRoute::Kind::Ignore:
        break;
    }
}

// Drive a KEY:<event.code>:<mods>:<text> frame exactly as the TUI's keystroke
// path: a printable without a keycode routes as text; a keycode resolves against
// the keymap for the current focus and dispatches when bound, otherwise falls
// back to inserting its text. Single-stroke resolution mirrors the TUI, which
// also passes one stroke per resolve; a per-connection pending buffer would give
// the web host multi-stroke behavior the TUI does not have, so it is deferred
// until the keymap grows a multi-stroke binding and both clients adopt it.
void handleKey(EditorRuntime& runtime, ClientId client,
               CompiledKeymap const& keymap, std::string_view body) {
    auto const firstColon = body.find(':');
    if (firstColon == std::string_view::npos) return;
    auto const secondColon = body.find(':', firstColon + 1);
    if (secondColon == std::string_view::npos) return;
    std::string_view const codeName = body.substr(0, firstColon);
    std::string_view const mods =
        body.substr(firstColon + 1, secondColon - firstColon - 1);
    std::string const text{body.substr(secondColon + 1)};

    KeyStroke stroke;
    stroke.code = keyCodeFromName(codeName);
    stroke.control = mods.find('c') != std::string_view::npos;
    stroke.alt = mods.find('a') != std::string_view::npos;
    stroke.meta = mods.find('m') != std::string_view::npos;
    stroke.shift = mods.find('s') != std::string_view::npos;

    if (stroke.code == KeyCode::None) {
        if (!text.empty()) routeText(runtime, client, text);
        return;
    }

    FocusTarget focus = FocusTarget::Editor;
    if (auto const snapshot = runtime.snapshot(client)) {
        focus = snapshot->sections().focus;
    }
    auto const resolution =
        keymap.resolve(std::array{CompiledKeymap::compile(stroke)}, focus);
    if (resolution.kind == KeymapMatchKind::Resolved) {
        (void)runtime.dispatch(client,
                               {resolution.command, runtime.revision(), {}});
    } else if (!text.empty()) {
        routeText(runtime, client, text);
    }
}

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

    // Compile the runtime's keymap once for keystroke resolution. The --http
    // path runs no init script, so the bindings are the defaults; rebuilding on
    // a keymap change is deferred until the web path can load one.
    std::shared_ptr<CompiledKeymap> compiledKeymap;
    if (auto const initial = runtime.snapshot(client)) {
        compiledKeymap = std::make_shared<CompiledKeymap>(
            initial->sections().keymap, *runtime.commandCatalog());
    }

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
                [&runtime, &server, client, sendSnapshot, attached,
                 compiledKeymap](Http::WebSocketHandle handle,
                                 Http::WebSocketMessage message) {
                    std::string_view const payload{message.data};
                    if (payload.rfind("KEY:", 0) == 0) {
                        if (compiledKeymap) {
                            handleKey(runtime, client, *compiledKeymap,
                                      payload.substr(4));
                        }
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
