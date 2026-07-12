import {spawn, spawnSync} from "node:child_process";
import fs from "node:fs";
import http from "node:http";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import {fileURLToPath} from "node:url";

class BidiConnection {
  #id = 0;
  #pending = new Map();
  #socket;

  static open(url) {
    return new Promise((resolve, reject) => {
      const socket = new WebSocket(url);
      socket.addEventListener("open", () =>
        resolve(new BidiConnection(socket)), {once: true});
      socket.addEventListener("error", () =>
        reject(new Error("WebDriver BiDi connection failed")), {once: true});
    });
  }

  constructor(socket) {
    this.#socket = socket;
    socket.addEventListener("message", (event) => {
      const message = JSON.parse(event.data);
      const pending = this.#pending.get(message.id);
      if (!pending) return;
      this.#pending.delete(message.id);
      if (message.type === "success") pending.resolve(message.result);
      else pending.reject(new Error(message.message ?? JSON.stringify(message)));
    });
  }

  call(method, params) {
    return new Promise((resolve, reject) => {
      const id = ++this.#id;
      this.#pending.set(id, {resolve, reject});
      this.#socket.send(JSON.stringify({id, method, params}));
    });
  }

  close() {
    this.#socket.close();
  }
}

const directory = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(directory, "../../..");
const required = new Set(
  (argumentValue("--require") ?? "").split(",").filter(Boolean));
const requested = required.size > 0
  ? [...required]
  : ["chromium", "firefox", "webkit"];
const executables = {
  chromium: findExecutable(process.env.SSG_CHROMIUM, [
    "chromium", "chromium-browser", "google-chrome", "google-chrome-stable",
  ]),
  firefox: findExecutable(process.env.SSG_FIREFOX, ["firefox"]),
  webkit: findExecutable(process.env.SSG_WEBKIT, [
    "MiniBrowser", "/usr/lib/webkit2gtk-4.1/MiniBrowser",
  ]),
};

const missing = requested.filter((browser) => !executables[browser]);
if (missing.length > 0 && required.size > 0) {
  console.error(`missing required browser runtimes: ${missing.join(", ")}`);
  process.exit(1);
}

const available = requested.filter((browser) => executables[browser]);
if (available.length === 0) {
  console.log("browser input live gate skipped: no supported runtime found");
  process.exit(77);
}

const reports = new Map();
const waiters = new Map();
const server = http.createServer(async (request, response) => {
  try {
    const url = new URL(request.url, "http://127.0.0.1");
    if (request.method === "POST" && url.pathname === "/__result") {
      const value = JSON.parse(await readBody(request));
      reports.set(value.browser, value);
      waiters.get(value.browser)?.(value);
      response.writeHead(204).end();
      return;
    }
    const relative = url.pathname === "/"
      ? "tests/browser/input/harness.html"
      : decodeURIComponent(url.pathname.slice(1));
    const filename = path.resolve(root, relative);
    if (filename !== root && !filename.startsWith(`${root}${path.sep}`)) {
      response.writeHead(403).end();
      return;
    }
    if (!fs.existsSync(filename) || !fs.statSync(filename).isFile()) {
      response.writeHead(404).end();
      return;
    }
    response.writeHead(200, {"content-type": contentType(filename)});
    fs.createReadStream(filename).pipe(response);
  } catch (error) {
    response.writeHead(500).end(String(error));
  }
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const port = server.address().port;

let failed = false;
for (const browser of available) {
  const trusted = browser === "firefox";
  const url = `http://127.0.0.1:${port}/tests/browser/input/harness.html` +
    `?browser=${browser}&trusted=${trusted ? "1" : "0"}`;
  try {
    const reportPromise = waitForReport(browser, 30000);
    reportPromise.catch(() => {});
    if (browser === "firefox") {
      await runFirefox(executables[browser], url, reportPromise);
    } else {
      await runSelfReportingBrowser(browser, executables[browser], url,
        reportPromise);
    }
    const report = reports.get(browser) ?? await reportPromise;
    if (!report.ok) throw new Error(report.error);
    console.log(`${browser}: ${report.assertions.length} assertions passed`);
  } catch (error) {
    failed = true;
    console.error(`${browser}: ${error instanceof Error ? error.message : error}`);
  }
}
server.close();
process.exit(failed ? 1 : 0);

function argumentValue(name) {
  const prefix = `${name}=`;
  return process.argv.find((argument) => argument.startsWith(prefix))
    ?.slice(prefix.length);
}

function findExecutable(explicit, candidates) {
  for (const candidate of [explicit, ...candidates]) {
    if (!candidate) continue;
    if (candidate.includes("/") && fs.existsSync(candidate)) return candidate;
    const result = spawnSync("sh", ["-c", `command -v "$1"`, "sh", candidate], {
      encoding: "utf8",
    });
    if (result.status === 0) return result.stdout.trim();
  }
  return undefined;
}

function waitForReport(browser, timeoutMs) {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() =>
      reject(new Error("timed out waiting for browser report")), timeoutMs);
    waiters.set(browser, (value) => {
      clearTimeout(timeout);
      resolve(value);
    });
  });
}

async function runSelfReportingBrowser(browser, executable, url, reportPromise) {
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), `ssg-${browser}-`));
  const args = browser === "chromium"
    ? ["--headless=new", "--no-first-run", "--disable-gpu",
      `--user-data-dir=${temporary}`, url]
    : [url];
  const child = spawn(executable, args, {stdio: ["ignore", "pipe", "pipe"]});
  try {
    await reportPromise;
  } finally {
    child.kill("SIGTERM");
    await waitForExit(child);
    fs.rmSync(temporary, {recursive: true, force: true});
  }
}

async function runFirefox(executable, url, reportPromise) {
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "ssg-firefox-"));
  const remotePort = await unusedPort();
  const child = spawn(executable, [
    "--headless", "--new-instance", "--profile", temporary,
    "--remote-debugging-port", String(remotePort), "about:blank",
  ], {stdio: ["ignore", "pipe", "pipe"]});
  let diagnostics = "";
  child.stderr.on("data", (chunk) => {
    diagnostics += chunk;
  });
  let bidi;
  try {
    try {
      bidi = await connectBidi(remotePort);
    } catch (error) {
      throw new Error(`${error.message}\n${diagnostics.trim()}`);
    }
    await bidi.call("session.new", {
      capabilities: {alwaysMatch: {acceptInsecureCerts: true}},
    });
    const created = await bidi.call("browsingContext.create", {type: "tab"});
    const context = created.context;
    await bidi.call("browsingContext.navigate", {
      context, url, wait: "complete",
    });
    await waitUntilReady(bidi, context);
    await bidi.call("input.performActions", {
      context,
      actions: [{
        type: "key",
        id: "keyboard",
        actions: [
          {type: "keyDown", value: "\uE009"},
          {type: "keyDown", value: "\uE008"},
          {type: "keyDown", value: "m"},
          {type: "keyUp", value: "m"},
          {type: "keyUp", value: "\uE008"},
          {type: "keyUp", value: "\uE009"},
          {type: "keyDown", value: "a"}, {type: "keyUp", value: "a"},
          {type: "keyDown", value: "a"}, {type: "keyUp", value: "a"},
        ],
      }],
    });
    await reportPromise;
  } finally {
    bidi?.close();
    child.kill("SIGTERM");
    await waitForExit(child);
    fs.rmSync(temporary, {recursive: true, force: true});
  }
}

async function waitUntilReady(bidi, context) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const result = await bidi.call("script.evaluate", {
      expression: "window.__trustedReady === true",
      target: {context},
      awaitPromise: false,
    });
    if (result.result?.value === true) return;
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
  throw new Error("Firefox harness did not become ready");
}

async function connectBidi(port) {
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    if (await tcpAccepts(port)) {
      return BidiConnection.open(`ws://127.0.0.1:${port}/session`);
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error("Firefox WebDriver BiDi endpoint did not start");
}

function tcpAccepts(port) {
  return new Promise((resolve) => {
    const socket = net.connect(port, "127.0.0.1");
    socket.once("connect", () => {
      socket.destroy();
      resolve(true);
    });
    socket.once("error", () => resolve(false));
  });
}

function unusedPort() {
  return new Promise((resolve, reject) => {
    const listener = net.createServer();
    listener.once("error", reject);
    listener.listen(0, "127.0.0.1", () => {
      const port = listener.address().port;
      listener.close(() => resolve(port));
    });
  });
}

function waitForExit(child) {
  if (child.exitCode !== null) return Promise.resolve();
  return new Promise((resolve) => {
    const timeout = setTimeout(() => {
      child.kill("SIGKILL");
    }, 1000);
    child.once("exit", () => {
      clearTimeout(timeout);
      resolve();
    });
  });
}

function readBody(request) {
  return new Promise((resolve, reject) => {
    let body = "";
    request.setEncoding("utf8");
    request.on("data", (chunk) => {
      body += chunk;
      if (body.length > 1024 * 1024) reject(new Error("result too large"));
    });
    request.on("end", () => resolve(body));
    request.on("error", reject);
  });
}

function contentType(filename) {
  if (filename.endsWith(".html")) return "text/html; charset=utf-8";
  if (filename.endsWith(".mjs")) return "text/javascript; charset=utf-8";
  if (filename.endsWith(".json")) return "application/json; charset=utf-8";
  return "application/octet-stream";
}
