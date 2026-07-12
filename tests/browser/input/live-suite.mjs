import {
  BrowserInputAdapter,
  canonicalStroke,
  expandKeymap,
} from "./browser-input.mjs";

const parameters = new URLSearchParams(location.search);
const browser = parameters.get("browser");
const trusted = parameters.get("trusted") === "1";

try {
  const [keymap, reserved, cases] = await Promise.all([
    readJson("/data/default-keymap.json"),
    readJson("/tests/browser/fixtures/reserved-chords.json"),
    readJson("/tests/browser/input/event-cases.json"),
  ]);
  const assertions = [];
  const check = (condition, message) => {
    if (!condition) throw new Error(message);
    assertions.push(message);
  };

  const observed = [];
  const capture = (event) => observed.push(canonicalStroke(event));
  document.addEventListener("keydown", capture);
  for (const fixture of cases.keyboard) {
    document.dispatchEvent(new KeyboardEvent("keydown", {
      code: fixture.event.code,
      ctrlKey: fixture.event.ctrlKey,
      altKey: fixture.event.altKey,
      metaKey: fixture.event.metaKey,
      shiftKey: fixture.event.shiftKey,
      bubbles: true,
    }));
  }
  document.removeEventListener("keydown", capture);
  check(JSON.stringify(observed) ===
    JSON.stringify(cases.keyboard.map((fixture) => fixture.stroke)),
  "engine event fields preserve canonical strokes");

  const bindings = expandKeymap(keymap);
  check(bindings.length === 159, "all keymap commands expand");
  const keyboardCommands = [];
  const keyboardAdapter = new BrowserInputAdapter({
    browser,
    bindings,
    reserved: reserved.reserved,
    emit: (command) => keyboardCommands.push(command),
  });
  const sequenceEvents = bindings[0].sequence.map(eventForStroke);
  for (const event of sequenceEvents) keyboardAdapter.handleKeyDown(event);
  check(keyboardCommands[0]?.command_id === "text.insert" &&
    sequenceEvents.every((event) => event.defaultPrevented),
  "engine keyboard events map and suppress accepted semantic commands");

  const reservedStroke = reserved.reserved.find((entry) =>
    entry.browser === browser)?.sequence[0];
  const reservedEvent = eventForStroke(reservedStroke);
  const reservedCommands = [];
  new BrowserInputAdapter({
    browser,
    bindings: [{
      sequence: [reservedStroke],
      command_id: "test.reserved",
      context: "*",
    }],
    reserved: reserved.reserved,
    emit: (command) => reservedCommands.push(command),
  }).handleKeyDown(reservedEvent);
  check(reservedCommands.length === 0 && !reservedEvent.defaultPrevented,
    "browser-reserved chord is neither mapped nor suppressed");

  const emitted = [];
  const statuses = [];
  const adapter = new BrowserInputAdapter({
    browser,
    bindings,
    reserved: reserved.reserved,
    internalClipboard: "fallback",
    emit: (command) => emitted.push(command),
    publishStatus: (status) => statuses.push(status),
  });
  adapter.handleCompositionStart();
  for (const update of cases.ime.updates) {
    adapter.handleBeforeInput(new InputEvent("beforeinput", {
      inputType: "insertCompositionText",
      data: update,
    }));
  }
  adapter.handleCompositionEnd(new CompositionEvent("compositionend", {
    data: cases.ime.committed,
  }));
  adapter.handleBeforeInput(new InputEvent("beforeinput", {
    inputType: "insertFromComposition",
    data: cases.ime.committed,
  }));
  check(JSON.stringify(emitted) === JSON.stringify([cases.ime.command]),
    "IME emits only committed UTF-8");
  adapter.activateHitTarget({command: cases.pointer.command});
  adapter.handleWheel({
    deltaY: 1,
    deltaMode: 1,
    preventDefault() {},
  });
  adapter.handleScrollbar(3, 10);
  check(emitted.at(-3).command_id === "cursor.set_position" &&
    emitted.at(-2).command_id === "view.scroll_lines" &&
    emitted.at(-1).command_id === "view.scroll_to_fraction",
  "pointer, wheel, and scrollbar share semantic command paths");

  await adapter.paste({
    secureContext: true,
    userGesture: true,
    clipboard: {readText: async () => {
      throw new DOMException("denied", "NotAllowedError");
    }},
  });
  check(emitted.at(-1).arguments.text === "fallback",
    "clipboard denial uses internal register");
  check(statuses.at(-1).id === "clipboard.system_paste_unavailable" &&
    statuses.at(-1).action_label.length > 0,
  "clipboard denial publishes actionable semantic status");

  const localCommands = [];
  const local = new BrowserInputAdapter({
    capabilities: ["local_file_drop"],
    emit: (command) => localCommands.push(command),
  });
  const remote = new BrowserInputAdapter();
  check(!remote.shouldExposeFileDrop() &&
    !remote.openDroppedFiles([{name: "remote.txt", bytes: [1]}]),
  "remote and unknown clients hide and reject file drop");
  check(local.shouldExposeFileDrop() &&
    local.openDroppedFiles([{name: "local.txt", bytes: [1]}]) &&
    localCommands[0].command_id === "file.open_dropped_content",
  "host-authorized local client translates file drop");

  if (trusted) {
    const trustedCommands = [];
    const trustedAdapter = new BrowserInputAdapter({
      browser,
      bindings,
      reserved: reserved.reserved,
      emit: (command) => trustedCommands.push(command),
    });
    const trustedEvents = [];
    const listener = (event) => {
      trustedEvents.push(event.isTrusted);
      trustedAdapter.handleKeyDown(event);
    };
    document.addEventListener("keydown", listener);
    window.__trustedReady = true;
    await waitFor(() => trustedCommands.length === 1, 5000);
    document.removeEventListener("keydown", listener);
    check(trustedEvents.length >= 3 && trustedEvents.every(Boolean),
      "native automation produced trusted key events");
    check(trustedCommands[0].command_id === "text.insert",
      "trusted key sequence maps to accepted semantic command");
  }

  await report({ok: true, browser, assertions, userAgent: navigator.userAgent});
} catch (error) {
  await report({
    ok: false,
    browser,
    error: error instanceof Error
      ? `${error.name}: ${error.message}\n${error.stack ?? ""}`
      : String(error),
    userAgent: navigator.userAgent,
  });
}

async function readJson(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`failed to load ${url}`);
  return response.json();
}

async function report(value) {
  await fetch("/__result", {
    method: "POST",
    headers: {"content-type": "application/json"},
    body: JSON.stringify(value),
  });
  document.body.textContent = value.ok ? "PASS" : `FAIL: ${value.error}`;
}

async function waitFor(predicate, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error("trusted input timed out");
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
}

function eventForStroke(stroke) {
  const pieces = stroke.split("+");
  return new KeyboardEvent("keydown", {
    code: pieces.at(-1),
    ctrlKey: pieces.includes("Ctrl"),
    altKey: pieces.includes("Alt"),
    metaKey: pieces.includes("Meta"),
    shiftKey: pieces.includes("Shift"),
    cancelable: true,
  });
}
