import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import {spawnSync} from "node:child_process";
import {fileURLToPath} from "node:url";

import {
  decodeEnvelope,
  encodeBinaryFrame,
  encodeCommandRequest,
  encodeSessionAttach,
} from "../../../examples/browser/protocol.mjs";
import {
  BrowserSession,
  applySessionDelta,
  canonicalSessionState,
  isRejectedCommandResult,
  renderSession,
} from "../../../examples/browser/client.mjs";
import {consumeCredential} from "../../../examples/browser/app.mjs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../..");
const fixtureExecutable = process.argv[2];
assert.ok(fixtureExecutable, "browser fixture executable path is required");

function fixture(name) {
  return Buffer.from(fs.readFileSync(
    path.join(root, `tests/fixtures/protocol/${name}.hex`), "utf8").trim(), "hex");
}

const decoded = decodeEnvelope(fixture("session_snapshot"));
assert.equal(decoded.kind, "session_snapshot");
assert.equal(decoded.value.revision, 4n);
assert.equal(decoded.value.sections.document.text, "alpha");
assert.deepEqual(decoded.value.client.capabilities, ["local_file_drop"]);
assert.equal(decoded.value.sections.theme.palette.length, 16);
assert.throws(() => decodeEnvelope(
  Uint8Array.from([...fixture("session_snapshot"), 0])), /trailing protocol bytes/);

assert.equal(
  Buffer.from(encodeCommandRequest({
    command_id: "edit.undo",
    base_revision: 3n,
    arguments: null,
  })).toString("hex"),
  fixture("command_request_no_payload").toString("hex"));
assert.equal(
  Buffer.from(encodeCommandRequest({
    command_id: "text.insert",
    base_revision: 5n,
    arguments: {text: "hello"},
  })).toString("hex"),
  fixture("command_request_text_input").toString("hex"));
const positiveScroll = decodeEnvelope(encodeCommandRequest({
  command_id: "view.scroll_lines",
  base_revision: 5n,
  arguments: {rows: 1},
}));
assert.equal(positiveScroll.value.payload.rows, 1n);
const negativeScroll = decodeEnvelope(encodeCommandRequest({
  command_id: "view.scroll_lines",
  base_revision: 5n,
  arguments: {rows: -1},
}));
assert.equal(negativeScroll.value.payload.rows, -1n);
const selectionNavigation = decodeEnvelope(encodeCommandRequest({
  command_id: "select.add_cursor_down",
  base_revision: 5n,
  arguments: null,
}));
assert.deepEqual(selectionNavigation.value.payload,
  {position: null, selection: null});
assert.equal(
  Buffer.from(encodeBinaryFrame({
    kind: "dropped_content",
    request_id: 99n,
    bytes: Uint8Array.from([9, 8, 7, 6, 5]),
  })).toString("hex"),
  fixture("binary_frame").toString("hex"));
assert.equal(encodeSessionAttach("remote fixture", undefined),
  "SSG1 ATTACH - 72656d6f74652066697874757265");
assert.equal(encodeSessionAttach("local", 42n),
  "SSG1 ATTACH 42 6c6f63616c");

const historyCalls = [];
const browserLocation = {
  href: "http://127.0.0.1:9000/?websocket=ws%3A%2F%2Ffixture#credential=launch-token",
};
assert.equal(consumeCredential(browserLocation, {
  replaceState(state, title, url) { historyCalls.push({state, title, url}); },
}), "launch-token");
assert.equal(historyCalls.length, 1);
assert.equal(historyCalls[0].url,
  "http://127.0.0.1:9000/?websocket=ws%3A%2F%2Ffixture");

const fixtureLocation = {
  href: "http://127.0.0.1:9000/?credential=remote",
};
assert.equal(consumeCredential(fixtureLocation, {
  replaceState() { assert.fail("query fixture credential must not rewrite history"); },
}), "remote");

const localSource = fs.readFileSync(
  path.join(root, "examples/browser/local.html"), "utf8");
assert.doesNotMatch(localSource, /credential.*local|local.*credential/i);

const oracle = spawnSync(fixtureExecutable, ["--delta-oracle"], {
  encoding: "utf8",
});
assert.equal(oracle.status, 0, oracle.stderr);
const oracleFrames = Object.fromEntries(oracle.stdout.trim().split("\n").map((line) => {
  const separator = line.indexOf(" ");
  return [line.slice(0, separator), line.slice(separator + 1)];
}));
const before = decodeEnvelope(Buffer.from(oracleFrames.BEFORE, "hex")).value;
const delta = decodeEnvelope(Buffer.from(oracleFrames.DELTA, "hex")).value;
const after = decodeEnvelope(Buffer.from(oracleFrames.AFTER, "hex")).value;
assert.equal(canonicalSessionState(applySessionDelta(before, delta)),
  canonicalSessionState(after));
assert.throws(() => applySessionDelta(after, delta), /delta base revision/);

class FakeNode {
  constructor(tag) {
    this.tag = tag;
    this.children = [];
    this.dataset = {};
    this.listeners = new Map();
    this.styleValues = new Map();
    this.style = {
      setProperty: (name, value) => this.styleValues.set(name, value),
    };
  }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = children; }
  setAttribute(name, value) { this[name] = value; }
  addEventListener(name, listener) { this.listeners.set(name, listener); }
  dispatch(name, event = {}) { return this.listeners.get(name)?.(event); }
}

const fakeDocument = {
  createElement(tag) { return new FakeNode(tag); },
};
const localRoot = new FakeNode("main");
renderSession(localRoot, decoded.value, {document: fakeDocument});
assert.equal(localRoot.dataset.revision, "4");
assert.equal(localRoot.dataset.fileDrop, "enabled");
assert.equal(localRoot.styleValues.size, 16);
assert.ok(findByClass(localRoot, "ssg-drop-input"));
assert.ok(accessibleLabels(localRoot).every((label) => label.length > 0));

const remoteSnapshot = structuredClone(decoded.value);
remoteSnapshot.client.capabilities = [];
const remoteRoot = new FakeNode("main");
renderSession(remoteRoot, remoteSnapshot, {document: fakeDocument});
assert.equal(remoteRoot.dataset.fileDrop, "disabled");
assert.equal(findByClass(remoteRoot, "ssg-drop-input"), undefined);

let opened;
class FakeSocket {
  constructor(url) {
    this.url = url;
    this.sent = [];
    opened = this;
  }
  send(value) { this.sent.push(value); }
  close() {}
}
const clipboardWrites = [];
const sessionRenders = [];
const session = new BrowserSession({
  url: "ws://127.0.0.1/session",
  credential: "remote",
  socketFactory: (url) => new FakeSocket(url),
  clipboard: {
    async writeText(text) { clipboardWrites.push(text); },
  },
  secureContext: true,
  render(snapshot, result) { sessionRenders.push({snapshot, result}); },
});
session.connect();
opened.onopen();
assert.equal(opened.binaryType, "arraybuffer");
assert.equal(opened.sent[0], "SSG1 ATTACH - 72656d6f7465");
await session.receive(fixture("session_snapshot"));
session.send({command_id: "text.insert", arguments: {text: "!"}});
const sentCommand = decodeEnvelope(opened.sent.at(-1));
assert.equal(sentCommand.value.base_revision, 4n);
assert.equal(sentCommand.value.payload.text, "!");
await session.receive(fixture("clipboard_request"));
const clipboardResponse = decodeEnvelope(opened.sent.at(-1));
assert.equal(clipboardResponse.kind, "clipboard_response");
assert.equal(clipboardResponse.value.id, 42n);
assert.equal(clipboardResponse.value.request_revision, 6n);
assert.equal(clipboardResponse.value.observed_document_revision, 4n);
assert.equal(clipboardResponse.value.status, 0n);
assert.deepEqual(clipboardWrites, ["copied text"]);
assert.ok(sessionRenders.length > 0);
assert.equal(isRejectedCommandResult({error: 0n, message: ""}), false);
assert.equal(isRejectedCommandResult({error: 1n, message: "rejected"}), true);

session.connect();
opened.onopen();
assert.equal(opened.sent[0], "SSG1 ATTACH 4 72656d6f7465");

const clientSource = fs.readFileSync(
  path.join(root, "examples/browser/client.mjs"), "utf8");
assert.doesNotMatch(clientSource, /default-keymap|undoStack|redoStack|pieceTree/);
assert.equal((clientSource.match(/new WebSocket/g) ?? []).length, 1);

function findByClass(node, className) {
  if (node.className === className) return node;
  for (const child of node.children ?? []) {
    const found = findByClass(child, className);
    if (found) return found;
  }
  return undefined;
}

function accessibleLabels(node) {
  const result = [];
  if (node["aria-label"] !== undefined) result.push(String(node["aria-label"]));
  for (const child of node.children ?? []) result.push(...accessibleLabels(child));
  return result;
}

console.log("browser client source contract passed");
