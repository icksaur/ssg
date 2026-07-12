import {
  BrowserSession,
  canonicalSessionState,
  renderSession,
} from "../../../examples/browser/client.mjs";

const query = new URLSearchParams(location.search);
const browser = query.get("browser");
const websocket = query.get("websocket");

run()
  .then((states) => report({browser, ok: true, per_step_states: states}))
  .catch((error) => report({
    browser,
    ok: false,
    per_step_states: [],
    error: error instanceof Error ? `${error.message}\n${error.stack}` : String(error),
  }));

async function run() {
  const fixture = await mount("local");
  const states = [];
  const record = () => states.push(canonicalSessionState(fixture.state));

  await fixture.command("workspace.open_directory", null);
  record();

  await fixture.command("file.open", null);
  record();

  await fixture.command("text.insert", {text: "!"});
  record();

  await fixture.command("select.add_cursor_down", null);
  record();

  await fixture.command("clipboard.copy", null, {clipboardGesture: true});
  record();

  await fixture.command("clipboard.cut");
  record();

  await fixture.command("clipboard.paste", null, {clipboardGesture: true});
  record();

  await fixture.command("edit.undo");
  record();

  await fixture.command("edit.redo");
  record();

  await fixture.command("view.toggle_word_wrap");
  record();

  await fixture.command("view.scroll_lines", {rows: 1});
  record();

  await fixture.command("view.scroll_to_fraction", {numerator: 1, denominator: 2});
  record();

  await fixture.command("file.save");
  record();

  await fixture.command("text.insert", {text: " dirty"});
  record();

  await fixture.command("tab.close");
  record();

  await fixture.command("tab.reopen_closed");
  record();

  await fixture.command("file.reload");
  record();

  // text.insert is rejected because mode is read_only after file.reload.
  const rejectedRevision = fixture.state.revision;
  fixture.session.send({command_id: "text.insert", arguments: {text: "rejected"}});
  await waitFor(() => fixture.lastResult?.error !== 0n);
  if (fixture.state.revision !== rejectedRevision) {
    throw new Error("rejected text.insert unexpectedly mutated revision");
  }
  record();

  await fixture.command("external.open_diff");
  record();

  await fixture.command("follow_edits.pause");
  record();

  await fixture.command("follow_edits.resume");
  record();

  await fixture.command("prompt.submit");
  record();

  const revisionBeforeDrop = fixture.state.revision;
  fixture.session.send({
    command_id: "file.open_dropped_content",
    arguments: {
      bytes: new Uint8Array([100, 114, 111, 112, 112, 101, 100]),
      suggested_label: "drop.txt",
    },
  });
  await fixture.waitForRevisionAfter(revisionBeforeDrop);
  record();

  return states;
}

function mount(credential) {
  let resolveReady;
  const fixture = {
    root: document.querySelector("#local"),
    state: undefined,
    lastResult: undefined,
    ready: new Promise((resolve) => { resolveReady = resolve; }),
  };
  const session = new BrowserSession({
    url: websocket,
    credential,
    render(snapshot, result) {
      fixture.state = snapshot;
      fixture.lastResult = result;
      if (snapshot) {
        renderSession(fixture.root, snapshot, {
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
  return fixture.ready.then(() => fixture);
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
  await fetch("/__result", {
    method: "POST",
    headers: {"content-type": "application/json"},
    body: JSON.stringify(value),
  });
}
