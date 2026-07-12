import assert from "node:assert/strict";
import fs from "node:fs/promises";
import path from "node:path";
import {fileURLToPath} from "node:url";

import {
  BrowserInputAdapter,
  canonicalStroke,
  expandKeymap,
} from "./browser-input.mjs";

const directory = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(directory, "../../..");
const readJson = async (name) =>
  JSON.parse(await fs.readFile(path.join(root, name), "utf8"));

const keymap = await readJson("data/default-keymap.json");
const reserved = await readJson("tests/browser/fixtures/reserved-chords.json");
const cases = await readJson("tests/browser/input/event-cases.json");

const bindings = expandKeymap(keymap);
assert.equal(bindings.length, keymap.commands.length);
assert.deepEqual(bindings[0], {
  sequence: ["Ctrl+Shift+KeyM", "KeyA", "KeyA"],
  command_id: "text.insert",
  context: "*",
});
assert.equal(
  new Set(bindings.map((binding) => binding.command_id)).size,
  keymap.commands.length,
);

for (const fixture of cases.keyboard) {
  assert.equal(canonicalStroke(fixture.event), fixture.stroke, fixture.name);
}

{
  const emitted = [];
  const prevented = [];
  const adapter = new BrowserInputAdapter({
    browser: "firefox",
    bindings,
    reserved: reserved.reserved,
    emit: (command) => emitted.push(command),
  });
  for (const [index, stroke] of bindings[0].sequence.entries()) {
    const event = eventForStroke(stroke, () => prevented.push(index));
    adapter.handleKeyDown(event);
  }
  assert.deepEqual(emitted, [{command_id: "text.insert", arguments: {}}]);
  assert.deepEqual(prevented, [0, 1, 2]);
}

{
  const emitted = [];
  let prevented = false;
  const adapter = new BrowserInputAdapter({
    browser: "firefox",
    bindings: [{
      sequence: ["Ctrl+KeyL"],
      command_id: "test.must_not_run",
      context: "*",
    }],
    reserved: reserved.reserved,
    emit: (command) => emitted.push(command),
  });
  adapter.handleKeyDown(eventForStroke("Ctrl+KeyL", () => {
    prevented = true;
  }));
  assert.deepEqual(emitted, []);
  assert.equal(prevented, false);
}

{
  const emitted = [];
  const adapter = new BrowserInputAdapter({emit: (command) => emitted.push(command)});
  adapter.handleCompositionStart();
  for (const text of cases.ime.updates) {
    adapter.handleBeforeInput({inputType: "insertCompositionText", data: text});
  }
  adapter.handleCompositionEnd({data: cases.ime.committed});
  adapter.handleBeforeInput({
    inputType: "insertFromComposition",
    data: cases.ime.committed,
  });
  assert.deepEqual(emitted, [cases.ime.command]);
}

{
  const emitted = [];
  const adapter = new BrowserInputAdapter({emit: (command) => emitted.push(command)});
  adapter.activateHitTarget({command: cases.pointer.command});
  adapter.handleWheel({deltaY: 2.4, deltaMode: 1, preventDefault() {}});
  adapter.handleScrollbar(25, 100);
  assert.deepEqual(emitted, [
    cases.pointer.command,
    {command_id: "view.scroll_lines", arguments: {rows: 2}},
    {
      command_id: "view.scroll_to_fraction",
      arguments: {numerator: 25, denominator: 100},
    },
  ]);
}

{
  const emitted = [];
  const statuses = [];
  const adapter = new BrowserInputAdapter({
    emit: (command) => emitted.push(command),
    publishStatus: (status) => statuses.push(status),
    internalClipboard: "register text",
  });
  const result = await adapter.paste({
    secureContext: true,
    userGesture: true,
    clipboard: {readText: async () => {
      throw new DOMException("denied", "NotAllowedError");
    }},
  });
  assert.equal(result.source, "internal_register");
  assert.deepEqual(emitted, [{
    command_id: "text.insert",
    arguments: {text: "register text"},
  }]);
  assert.deepEqual(statuses, [{
    id: "clipboard.system_paste_unavailable",
    action_label: "Paste from system clipboard",
  }]);
}

{
  const emitted = [];
  const remote = new BrowserInputAdapter({emit: (command) => emitted.push(command)});
  assert.equal(remote.shouldExposeFileDrop(), false);
  assert.equal(remote.openDroppedFiles([{name: "a.txt", bytes: [97]}]), false);
  assert.deepEqual(emitted, []);

  const local = new BrowserInputAdapter({
    capabilities: ["local_file_drop"],
    emit: (command) => emitted.push(command),
  });
  assert.equal(local.shouldExposeFileDrop(), true);
  assert.equal(local.openDroppedFiles([{name: "../a.txt", bytes: [97]}]), true);
  assert.deepEqual(emitted, [{
    command_id: "file.open_dropped_content",
    arguments: {files: [{suggested_name: "../a.txt", bytes: [97]}]},
  }]);
}

console.log("browser input source contract passed");

function eventForStroke(stroke, preventDefault) {
  const pieces = stroke.split("+");
  const code = pieces.at(-1);
  return {
    code,
    ctrlKey: pieces.includes("Ctrl"),
    altKey: pieces.includes("Alt"),
    metaKey: pieces.includes("Meta"),
    shiftKey: pieces.includes("Shift"),
    isComposing: false,
    preventDefault,
  };
}
