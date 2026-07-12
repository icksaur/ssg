import {spawn, spawnSync} from "node:child_process";
import fs from "node:fs";
import http from "node:http";
import net from "node:net";
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

  close() { this.#socket.close(); }
}

const directory = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(directory, "../../..");
const fixtureExecutable = argumentValue("--server");
if (!fixtureExecutable) throw new Error("--server must name browser_client_fixture");
const required = new Set(
  (argumentValue("--require") ?? "").split(",").filter(Boolean));
const requested = required.size > 0
  ? [...required]
  : ["chromium", "firefox", "webkit"];
const executables = {
  chromium: findExecutable(process.env.SSG_CHROMIUM, [
    "chromium", "chromium-browser", "google-chrome", "google-chrome-stable",
    "/usr/lib/electron/electron", "/usr/lib/electron43/electron",
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
  console.log("end-to-end parity browser gate skipped: no supported runtime found");
  process.exit(77);
}

const oracleResult = spawnSync(fixtureExecutable, ["--per-step-oracle"], {
  encoding: "utf8",
});
if (oracleResult.status !== 0) {
  throw new Error(oracleResult.stderr || "per-step oracle failed");
}
const oracleLines = oracleResult.stdout.trimEnd().split("\n").filter(Boolean);
const expectedStates = oracleLines.map(
  (line) => canonical(decodeProtocolHex(line.trim())));
const browserTemporaryRoot = path.join(root, ".browser-tmp");
fs.mkdirSync(browserTemporaryRoot, {recursive: true});

const reports = new Map();
const waiters = new Map();
const staticServer = http.createServer(async (request, response) => {
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
      ? "tests/browser/end_to_end/harness.html"
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
await new Promise((resolve) => staticServer.listen(0, "127.0.0.1", resolve));
const staticPort = staticServer.address().port;

let failed = false;
for (const browser of available) {
  const websocketPort = await unusedPort();
  const fixture = spawn(fixtureExecutable,
    ["--serve", String(websocketPort)], {stdio: ["pipe", "pipe", "pipe"]});
  let fixtureOutput = "";
  let fixtureErrors = "";
  fixture.stdout.setEncoding("utf8");
  fixture.stderr.setEncoding("utf8");
  fixture.stdout.on("data", (chunk) => { fixtureOutput += chunk; });
  fixture.stderr.on("data", (chunk) => { fixtureErrors += chunk; });
  const url = `http://127.0.0.1:${staticPort}/tests/browser/end_to_end/harness.html` +
    `?browser=${browser}&websocket=${encodeURIComponent(
      `ws://127.0.0.1:${websocketPort}/session`)}`;
  try {
    await waitFor(() => fixtureOutput.includes("READY"), 10000);
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
    if (!Array.isArray(report.per_step_states)) {
      throw new Error("browser report missing per_step_states array");
    }
    if (report.per_step_states.length !== expectedStates.length) {
      throw new Error(
        `step count mismatch: oracle ${expectedStates.length}, browser ${report.per_step_states.length}`);
    }
    for (let i = 0; i < expectedStates.length; i++) {
      if (report.per_step_states[i] !== expectedStates[i]) {
        throw new Error(`step ${i + 1}: per-step canonical state diverges from oracle`);
      }
    }
    console.log(`${browser}: ${expectedStates.length} per-step states match oracle`);
  } catch (error) {
    failed = true;
    console.error(`${browser}: ${error instanceof Error ? error.message : error}`);
    console.error(fixtureOutput);
  } finally {
    fixture.stdin.end("\n");
    await waitForExit(fixture);
    if (fixture.exitCode !== 0) {
      failed = true;
      console.error(fixtureErrors || `fixture exited ${fixture.exitCode}`);
    }
  }
}

staticServer.close();
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
  const temporary = path.join(
    browserTemporaryRoot, `${browser}-${process.pid}-${Date.now()}`);
  fs.mkdirSync(temporary, {recursive: true});
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
  const temporary = path.join(
    browserTemporaryRoot, `firefox-${process.pid}-${Date.now()}`);
  fs.mkdirSync(temporary, {recursive: true});
  const remotePort = await unusedPort();
  const child = spawn(executable, [
    "--headless", "--new-instance", "--profile", temporary,
    "--remote-debugging-port", String(remotePort), "about:blank",
  ], {stdio: ["ignore", "pipe", "pipe"]});
  let diagnostics = "";
  child.stderr.on("data", (chunk) => { diagnostics += chunk; });
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
    await bidi.call("browsingContext.navigate", {
      context: created.context, url, wait: "complete",
    });
    await reportPromise;
  } finally {
    bidi?.close();
    child.kill("SIGTERM");
    await waitForExit(child);
    fs.rmSync(temporary, {recursive: true, force: true});
  }
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

function waitFor(predicate, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  return new Promise((resolve, reject) => {
    const poll = () => {
      if (predicate()) {
        resolve();
      } else if (Date.now() >= deadline) {
        reject(new Error("timed out waiting for fixture"));
      } else {
        setTimeout(poll, 20);
      }
    };
    poll();
  });
}

function waitForExit(child) {
  if (child.exitCode !== null) return Promise.resolve();
  return new Promise((resolve) => {
    const timeout = setTimeout(() => child.kill("SIGKILL"), 1000);
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
      if (body.length > 4 * 1024 * 1024) {
        reject(new Error("result too large"));
      }
    });
    request.on("end", () => resolve(body));
    request.on("error", reject);
  });
}

function contentType(filename) {
  if (filename.endsWith(".html")) return "text/html; charset=utf-8";
  if (filename.endsWith(".mjs")) return "text/javascript; charset=utf-8";
  if (filename.endsWith(".css")) return "text/css; charset=utf-8";
  return "application/octet-stream";
}

function decodeProtocolHex(value) {
  const bytes = Buffer.from(value, "hex");
  let offset = 2;
  return readValue();

  function take(length) {
    const result = bytes.subarray(offset, offset + length);
    offset += length;
    return result;
  }
  function u32() {
    const value = bytes.readUInt32LE(offset);
    offset += 4;
    return value;
  }
  function u64(signed = false) {
    const result = signed
      ? bytes.readBigInt64LE(offset)
      : bytes.readBigUInt64LE(offset);
    offset += 8;
    return result;
  }
  function readValue() {
    switch (bytes[offset++]) {
      case 0: return null;
      case 1: return bytes[offset++] === 1;
      case 2: return u64(true);
      case 3: return u64();
      case 4: return take(u32()).toString("utf8");
      case 5: return Uint8Array.from(take(u32()));
      case 6: return Array.from({length: u32()}, readValue);
      case 7: {
        const object = {};
        const count = u32();
        for (let index = 0; index < count; ++index) {
          const key = take(u32()).toString("utf8");
          object[key] = readValue();
        }
        return object;
      }
      default: throw new Error("invalid oracle protocol value");
    }
  }
}

function canonical(value) {
  return JSON.stringify(value, (_key, item) => {
    if (typeof item === "bigint") return item.toString();
    if (item instanceof Uint8Array) return [...item];
    return item;
  });
}
