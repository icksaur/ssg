import {
  BrowserSession,
  canonicalSessionState,
  renderSession,
} from "../../../examples/browser/client.mjs";

const query = new URLSearchParams(location.search);
const browser = query.get("browser");
const websocket = query.get("websocket");
const assertions = [];
let reported = false;

globalThis.addEventListener("unhandledrejection", (event) => {
  report({
    browser,
    ok: false,
    assertions,
    error: event.reason instanceof Error
      ? `${event.reason.message}\n${event.reason.stack}`
      : String(event.reason),
  });
});

run().then((state) => report({browser, ok: true, assertions, state}))
  .catch((error) => report({
    browser,
    ok: false,
    assertions,
    error: error instanceof Error ? `${error.message}\n${error.stack}` : String(error),
  }));

async function run() {
  const productFixture = await fetch("/examples/browser/index.html");
  check(productFixture.ok && (await productFixture.text()).includes("./app.mjs"),
    "dependency-free loopback server serves the product fixture");
  const remote = mount(document.querySelector("#remote"), "remote");
  const local = mount(document.querySelector("#local"), "local");
  await Promise.all([remote.ready, local.ready]);

  check(remote.root.dataset.fileDrop === "disabled", "remote drop is absent");
  check(!remote.root.querySelector(".ssg-drop-input"),
    "remote drop input is not rendered");
  check(local.root.dataset.fileDrop === "enabled", "local drop is enabled");
  check(local.root.querySelector(".ssg-drop-input"),
    "local drop input is rendered");
  check(local.root.style.getPropertyValue("--ssg-15").length > 0,
    "all sixteen theme colors are applied");
  const labels = [...local.root.querySelectorAll("[aria-label]")]
    .map((node) => node.getAttribute("aria-label"));
  check(labels.length > 0 && labels.every(Boolean),
    "every rendered accessibility label is non-empty");
  check(labels.includes("Workspace /fixture"), "header API label is rendered");
  check(labels.includes("Reopen closed tab"), "status action API label is rendered");

  await local.command("workspace.open_directory");
  await local.command("file.open");

  const revisionBeforeInput = local.state.revision;
  local.root.querySelector(".ssg-editor").dispatchEvent(new InputEvent("beforeinput", {
    bubbles: true,
    cancelable: true,
    inputType: "insertText",
    data: "!",
  }));
  await local.waitForRevisionAfter(revisionBeforeInput);

  local.root.querySelector(".ssg-editor").dispatchEvent(new KeyboardEvent("keydown", {
    bubbles: true,
    cancelable: true,
    code: "KeyA",
  }));
  await waitFor(() =>
    local.state.sections.selection.selections.selections.length === 2);
  check(local.state.sections.selection.selections.selections.length === 2,
    `authoritative keymap command updates selection state: ${
      canonicalSessionState({
        selection: local.state.sections.selection,
        keymap: local.state.sections.keymap,
      })}`);

  await local.command("clipboard.copy", null, {clipboardGesture: true});
  await local.command("clipboard.cut");
  await local.command("clipboard.paste", null, {clipboardGesture: true});
  await local.command("edit.undo");
  await local.command("edit.redo");
  await local.command("view.toggle_word_wrap");

  const revisionBeforeWheel = local.state.revision;
  local.root.querySelector(".ssg-editor").dispatchEvent(new WheelEvent("wheel", {
    bubbles: true,
    cancelable: true,
    deltaY: 1,
    deltaMode: 1,
  }));
  await local.waitForRevisionAfter(revisionBeforeWheel);

  const scrollbar = local.root.querySelector(".ssg-scrollbar");
  scrollbar.value = "40";
  const revisionBeforeScrollbar = local.state.revision;
  scrollbar.dispatchEvent(new Event("input", {bubbles: true}));
  await local.waitForRevisionAfter(revisionBeforeScrollbar);

  await local.command("file.save");
  await local.command("text.insert", {text: " dirty"});
  await local.command("tab.close");
  await local.command("tab.reopen_closed");
  await local.command("file.reload");

  const rejectedRevision = local.state.revision;
  local.session.send({command_id: "text.insert", arguments: {text: "rejected"}});
  await waitFor(() => local.lastResult?.error !== 0n);
  check(local.state.revision === rejectedRevision,
    "read-only rejection is failure atomic");

  await local.command("external.open_diff");
  await local.command("follow_edits.pause");
  await local.command("follow_edits.resume");

  const prompt = local.root.querySelector(".ssg-prompt-input");
  const revisionBeforePrompt = local.state.revision;
  prompt.dispatchEvent(new KeyboardEvent("keydown", {
    bubbles: true,
    cancelable: true,
    key: "Enter",
  }));
  await local.waitForRevisionAfter(revisionBeforePrompt);

  local.root.querySelector(".ssg-status-action").click();

  const input = local.root.querySelector(".ssg-drop-input");
  Object.defineProperty(input, "files", {
    configurable: true,
    value: [new File(["dropped"], "drop.txt")],
  });
  const revisionBeforeDrop = local.state.revision;
  input.dispatchEvent(new Event("change", {bubbles: true}));
  await local.waitForRevisionAfter(revisionBeforeDrop);
  check(local.state.sections.document.text === "dropped",
    "authorized file drop reaches authoritative document state");

  await waitFor(() => remote.state.revision === local.state.revision);
  const remoteRevision = remote.state.revision;
  remote.session.send({
    command_id: "file.open_dropped_content",
    arguments: {bytes: new Uint8Array([120]), suggested_label: "remote.txt"},
  });
  await waitFor(() => remote.lastResult?.error === 4n);
  check(remote.state.revision === remoteRevision,
    "remote file drop is rejected without state mutation");

  return canonicalSessionState(local.state);
}

function mount(root, credential) {
  let resolveReady;
  let clipboardText = "";
  const fixture = {
    root,
    state: undefined,
    lastResult: undefined,
    ready: new Promise((resolve) => { resolveReady = resolve; }),
  };
  const session = new BrowserSession({
    url: websocket,
    credential,
    clipboard: {
      writeText: async (text) => { clipboardText = text; },
      readText: async () => clipboardText,
    },
    secureContext: true,
    render(snapshot, result) {
      fixture.state = snapshot;
      fixture.lastResult = result;
      if (snapshot) {
        renderSession(root, snapshot, {
          send: (command, options) => session.send(command, options),
          invokeStatusAction: (invocation) =>
            session.invokeStatusAction(invocation),
        });
        resolveReady();
      }
    },
  });
  fixture.session = session;
  fixture.command = async (command_id, argumentsValue = null, options) => {
    const revision = fixture.state.revision;
    session.send({command_id, arguments: argumentsValue}, options);
    await fixture.waitForRevisionAfter(revision);
  };
  fixture.waitForRevisionAfter = (revision) =>
    waitFor(() => fixture.state.revision > revision);
  session.connect();
  return fixture;
}

function check(condition, description) {
  if (!condition) throw new Error(description);
  assertions.push(description);
}

async function waitFor(predicate, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  throw new Error("timed out waiting for browser client state");
}

async function report(value) {
  if (reported) return;
  reported = true;
  await fetch("/__result", {
    method: "POST",
    headers: {"content-type": "application/json"},
    body: JSON.stringify(value),
  });
}
