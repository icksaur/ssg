import {
  decodeEnvelope,
  encodeClipboardResponse,
  encodeCommandRequest,
  encodeSessionAttach,
  encodeStatusActionInvocation,
} from "./protocol.mjs";

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder("utf-8", {fatal: true});

export class BrowserSession {
  #clipboard;
  #clipboardGesture = false;
  #credential;
  #lastRevision;
  #render;
  #secureContext;
  #socket;
  #socketFactory;
  #state;
  #url;

  constructor({
    url,
    credential,
    socketFactory = (address) => new WebSocket(address),
    clipboard = globalThis.navigator?.clipboard,
    secureContext = globalThis.isSecureContext === true,
    render,
  }) {
    if (!url || !credential || typeof render !== "function") {
      throw new TypeError("url, credential, and render are required");
    }
    this.#url = url;
    this.#credential = credential;
    this.#socketFactory = socketFactory;
    this.#clipboard = clipboard;
    this.#secureContext = secureContext;
    this.#render = render;
  }

  connect() {
    const socket = this.#socketFactory(this.#url);
    socket.binaryType = "arraybuffer";
    socket.onopen = () => socket.send(
      encodeSessionAttach(this.#credential, this.#lastRevision));
    socket.onmessage = async (event) => {
      const bytes = event.data instanceof Blob
        ? new Uint8Array(await event.data.arrayBuffer())
        : new Uint8Array(event.data);
      await this.receive(bytes);
    };
    this.#socket = socket;
  }

  async receive(bytes) {
    const message = decodeEnvelope(bytes);
    if (message.kind === "session_snapshot") {
      this.#state = message.value;
    } else if (message.kind === "session_delta") {
      this.#state = applySessionDelta(this.#state, message.value);
    } else if (message.kind === "clipboard_request") {
      await this.#handleClipboard(message.value);
      return;
    } else if (message.kind === "command_result") {
      this.#render(this.#state, message.value);
      return;
    } else {
      throw new RangeError(`unexpected server message: ${message.kind}`);
    }
    this.#lastRevision = this.#state.revision;
    this.#render(this.#state);
  }

  send({command_id, arguments: payload = null}, {clipboardGesture = false} = {}) {
    if (!this.#state) throw new Error("session snapshot has not arrived");
    this.#clipboardGesture ||= clipboardGesture;
    this.#socket.send(encodeCommandRequest({
      command_id,
      base_revision: this.#state.revision,
      arguments: payload,
    }));
  }

  invokeStatusAction(invocation) {
    this.#socket.send(encodeStatusActionInvocation(invocation));
  }

  close() { this.#socket?.close(); }

  async #handleClipboard(request) {
    let status = 2;
    let text = "";
    try {
      if (request.kind === 0n && this.#secureContext &&
          this.#clipboard?.writeText) {
        await this.#clipboard.writeText(request.text);
        status = 0;
      } else if (request.kind === 1n && this.#secureContext &&
          this.#clipboardGesture && this.#clipboard?.readText) {
        text = await this.#clipboard.readText();
        status = 0;
      }
    } catch {
      status = 1;
    }
    if (request.kind === 1n) this.#clipboardGesture = false;
    this.#socket.send(encodeClipboardResponse({
      id: request.id,
      request_revision: request.request_revision,
      observed_document_revision:
        this.#state?.sections?.document?.revision ?? request.request_revision,
      status,
      text,
    }));
  }
}

export function isRejectedCommandResult(result) {
  return result !== undefined && result !== null && result.error !== 0n;
}

export function applySessionDelta(snapshot, delta) {
  if (!snapshot) throw new Error("session delta arrived before snapshot");
  if (snapshot.revision !== delta.base_revision ||
      delta.revision <= delta.base_revision) {
    throw new RangeError("session delta base revision does not match");
  }
  if (snapshot.client.client_id !== delta.client_id ||
      snapshot.client.view_id !== delta.view_id ||
      canonicalSessionState(snapshot.client.capabilities) !==
        canonicalSessionState(delta.capabilities)) {
    throw new RangeError("session delta client attachment does not match");
  }

  const next = structuredClone(snapshot);
  next.revision = delta.revision;
  if (delta.topology !== null && delta.topology !== undefined) {
    next.topology = delta.topology;
  }
  if (delta.viewport) {
    next.client.viewport = replacement(delta.viewport, next.client.viewport);
  }

  const sections = next.sections;
  if (delta.document) {
    sections.document = applyDocumentDelta(
      sections.document, delta.document, delta.document_caret);
  } else if (delta.document_caret !== null &&
      delta.document_caret !== undefined) {
    sections.document.caret = delta.document_caret;
  }

  for (const key of ["selection", "history", "clipboard", "prompt_status",
    "find_replace", "keymap"]) {
    if (delta[key]) sections[key] = replacement(delta[key], sections[key]);
  }
  for (const key of ["search", "tabs", "lsp_sync", "lsp_features"]) {
    if (delta[key]?.state) sections[key] = delta[key].state;
  }
  if (delta.text_encoding) sections.text_encoding = delta.text_encoding.after;
  if (delta.settings?.changes?.length > 0) {
    const entries = [...sections.settings.entries];
    for (const change of delta.settings.changes) {
      const index = entries.findIndex((entry) => entry.key === change.key);
      if (index < 0) throw new RangeError("settings delta key is missing");
      entries[index] = {...entries[index], effective: change.after};
    }
    sections.settings = {entries};
  }
  for (const key of ["diff", "external_modification"]) {
    if (delta[key]) sections[key] = applyCollectionDelta(sections[key], delta[key]);
  }
  if (delta.follow_edits?.replacement) {
    sections.follow_edits = delta.follow_edits.replacement;
  }
  if (delta.tree) sections.tree = applyTreeDelta(sections.tree, delta.tree);
  if (delta.syntax) {
    const syntax = {...sections.syntax, revision: delta.syntax.revision};
    for (const [key, value] of Object.entries(
      without(delta.syntax, ["base_revision", "revision"]))) {
      if (value !== null && value !== undefined) syntax[key] = value;
    }
    sections.syntax = syntax;
  }
  if (delta.theme?.replacement) sections.theme = delta.theme.replacement;
  if (delta.shell?.replacement) sections.shell = delta.shell.replacement;
  return next;
}

function applyDocumentDelta(document, delta, caret) {
  if (document.revision !== delta.base_revision) {
    throw new RangeError("document delta base revision does not match");
  }
  const bytes = textEncoder.encode(document.text);
  const start = Number(delta.start);
  const erased = Number(delta.erased_bytes);
  if (!Number.isSafeInteger(start) || !Number.isSafeInteger(erased) ||
      start < 0 || erased < 0 || start + erased > bytes.length) {
    throw new RangeError("document delta range is invalid");
  }
  const inserted = textEncoder.encode(delta.inserted_text);
  const combined = new Uint8Array(bytes.length - erased + inserted.length);
  combined.set(bytes.subarray(0, start));
  combined.set(inserted, start);
  combined.set(bytes.subarray(start + erased), start + inserted.length);
  return {
    revision: delta.revision,
    text: textDecoder.decode(combined),
    caret: caret ?? document.caret,
  };
}

function replacement(delta, previous) {
  if (delta.changed === false) return previous;
  if (delta.changed === true && !delta.replacement) {
    throw new RangeError("changed delta has no replacement");
  }
  return delta.replacement ?? delta.after ?? delta.state ?? previous;
}

function applyCollectionDelta(state, delta) {
  const files = new Map((state.files ?? []).map((file) =>
    [String(file.file ?? file.id ?? file.path), file]));
  for (const id of delta.removed ?? []) files.delete(String(id));
  for (const file of delta.upserted ?? []) {
    files.set(String(file.file ?? file.id ?? file.path), file);
  }
  return {...state, revision: delta.revision, files: [...files.values()]};
}

function applyTreeDelta(state, delta) {
  if (state.revision !== delta.base_revision || delta.snapshot_required) {
    throw new RangeError("tree delta cannot be replayed");
  }
  const providers = [...state.providers];
  for (const change of delta.providers) {
    const key = String(change.provider_id);
    const index = providers.findIndex((provider) =>
      String(provider.provider_id) === key);
    if (change.remove_provider) {
      if (index < 0) throw new RangeError("tree provider is missing");
      providers.splice(index, 1);
      continue;
    }
    if (index < 0) {
      providers.push({
        provider_id: change.provider_id,
        kind: change.kind,
        nodes: change.insert,
      });
      continue;
    }
    const provider = providers[index];
    const start = Number(change.start);
    const erase = Number(change.erase_count);
    providers[index] = {
      ...provider,
      kind: change.kind,
      nodes: [
        ...provider.nodes.slice(0, start),
        ...change.insert,
        ...provider.nodes.slice(start + erase),
      ],
    };
  }
  return {revision: delta.revision, providers};
}

function without(value, keys) {
  const omitted = new Set(keys);
  return Object.fromEntries(Object.entries(value).filter(([key]) =>
    !omitted.has(key)));
}

export function canonicalSessionState(value) {
  return JSON.stringify(value, (_key, item) => {
    if (typeof item === "bigint") return item.toString();
    if (item instanceof Uint8Array) return [...item];
    return item;
  });
}

export function renderSession(root, snapshot, {
  document = globalThis.document,
  send = () => {},
  invokeStatusAction = () => {},
} = {}) {
  root.dataset.revision = String(snapshot.revision);
  const capabilities = snapshot.client.capabilities;
  const local = capabilities.includes("local_file_drop");
  root.dataset.fileDrop = local ? "enabled" : "disabled";
  applyPalette(root, snapshot.sections.theme?.palette ?? []);

  const shellState = snapshot.sections.shell;
  const labels = shellLabels(shellState?.accessibility_nodes ?? []);
  const shell = element(document, "section", "ssg-shell");

  appendRegion(document, shell, "header", shellState?.header,
    labels.get("0")?.[0]);

  const tabs = element(document, "nav", "ssg-tabs", labels.get("5")?.[0]);
  for (const tab of snapshot.sections.tabs?.tabs ?? []) {
    const button = element(document, "button", "ssg-tab", tab.label);
    button.textContent = tab.label;
    button.dataset.active = tab.id === snapshot.sections.tabs.active
      ? "true" : "false";
    button.addEventListener("click", () =>
      send({command_id: "tab.activate", arguments: null}));
    tabs.append(button);
  }
  shell.append(tabs);

  const editor = element(document, "pre", "ssg-editor", labels.get("9")?.[0]);
  editor.tabIndex = 0;
  editor.textContent = snapshot.sections.document?.text ?? "";
  let suppressCompositionInput = false;
  editor.addEventListener("compositionend", (event) => {
    if (!event.data) return;
    send({command_id: "text.insert", arguments: {text: event.data}});
    suppressCompositionInput = true;
  });
  editor.addEventListener("beforeinput", (event) => {
    if (event.inputType === "insertCompositionText") return;
    if (event.inputType === "insertFromComposition" &&
        suppressCompositionInput) {
      suppressCompositionInput = false;
      return;
    }
    if (event.inputType === "insertText" && event.data) {
      event.preventDefault();
      send({command_id: "text.insert", arguments: {text: event.data}});
    }
  });
  const keyDispatcher = keymapDispatcher(
    snapshot.sections.keymap?.bindings ?? [], send);
  editor.addEventListener("keydown", keyDispatcher);
  editor.addEventListener("wheel", (event) => {
    const rows = event.deltaMode === 1
      ? Math.trunc(event.deltaY)
      : Math.sign(event.deltaY);
    if (rows === 0) return;
    event.preventDefault();
    send({command_id: "view.scroll_lines", arguments: {rows}});
  });
  editor.addEventListener("pointerdown", () => {
    const target = snapshot.client.viewport?.hit_targets?.[0];
    if (!target) return;
    send({
      command_id: "cursor.set_position",
      arguments: {
        position: {
          byte_offset: target.byte_offset,
          line: target.logical_line,
          cell: target.cell,
        },
        selection: null,
      },
    });
  });
  shell.append(editor);

  const scrollbarMetrics = snapshot.client.viewport?.scrollbar;
  if (scrollbarMetrics) {
    const scrollbar = element(
      document, "input", "ssg-scrollbar", labels.get("10")?.[0]);
    scrollbar.type = "range";
    scrollbar.min = "0";
    scrollbar.max = String(scrollbarMetrics.maximum_first_row);
    scrollbar.value = String(scrollbarMetrics.first_row);
    scrollbar.addEventListener("input", () => send({
      command_id: "view.scroll_to_fraction",
      arguments: {
        numerator: Number(scrollbar.value),
        denominator: Math.max(1, Number(scrollbar.max)),
      },
    }));
    shell.append(scrollbar);
  }

  if (local) {
    const capabilityLabel = capabilities.find((value) =>
      value === "local_file_drop");
    const drop = element(document, "label", "ssg-drop", capabilityLabel);
    drop.textContent = "Drop local files";
    const input = element(
      document, "input", "ssg-drop-input", capabilityLabel);
    input.type = "file";
    input.multiple = true;
    input.addEventListener("change", async () => {
      for (const file of input.files) {
        send({
          command_id: "file.open_dropped_content",
          arguments: {
            bytes: new Uint8Array(await file.arrayBuffer()),
            suggested_label: file.name,
          },
        });
      }
    });
    drop.append(input);
    shell.append(drop);
  }

  appendPrompt(document, shell, snapshot.sections.prompt_status?.prompt, send);
  appendStatuses(document, shell, snapshot.sections.prompt_status?.status,
    invokeStatusAction);
  appendRegion(document, shell, "footer", shellState?.footer,
    labels.get("2")?.[0]);
  appendAccessibilityNodes(document, shell,
    shellState?.accessibility_nodes ?? []);
  root.replaceChildren(shell);
}

function applyPalette(root, palette) {
  if (palette.length !== 16) {
    throw new RangeError("browser snapshots require exactly sixteen colors");
  }
  palette.forEach((color, index) => {
    const channels = [color.red, color.green, color.blue]
      .map((value) => Number(value) / 255);
    root.style.setProperty(
      `--ssg-${index}`, `color(srgb ${channels.join(" ")})`);
  });
}

function appendRegion(document, parent, name, rect, accessibleLabel) {
  if (!rect) return;
  const region = element(document, name, `ssg-${name}`, accessibleLabel);
  region.dataset.x = String(rect.x);
  region.dataset.y = String(rect.y);
  region.dataset.width = String(rect.width);
  region.dataset.height = String(rect.height);
  region.textContent = accessibleLabel ?? "";
  parent.append(region);
}

function appendPrompt(document, parent, prompt, send) {
  if (!prompt) return;
  const region = element(
    document, "section", "ssg-prompt", prompt.accessible_label);
  for (const control of prompt.controls ?? []) {
    const input = element(
      document, "input", "ssg-prompt-input", control.accessible_label);
    input.value = control.value ?? "";
    input.checked = control.checked === true;
    input.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        send({command_id: "prompt.submit", arguments: null});
      } else if (event.key === "Escape") {
        event.preventDefault();
        send({command_id: "prompt.cancel", arguments: null});
      }
    });
    region.append(input);
  }
  parent.append(region);
}

function appendStatuses(document, parent, status, invoke) {
  if (!status) return;
  const region = element(document, "section", "ssg-status");
  for (const item of status.items ?? []) {
    const output = element(
      document, "output", "ssg-status-item", item.accessible_label);
    output.textContent = item.accessible_label;
    for (const action of item.actions ?? []) {
      const button = element(
        document, "button", "ssg-status-action", action.accessible_label);
      button.textContent = action.accessible_label;
      button.addEventListener("click", () => invoke({
        status_id: item.id,
        action_id: action.id,
        generation: item.generation,
      }));
      output.append(button);
    }
    region.append(output);
  }
  parent.append(region);
}

function appendAccessibilityNodes(document, parent, nodes) {
  for (const node of nodes) {
    const value = element(document, "span", "ssg-a11y", node.label);
    value.textContent = node.label;
    value.dataset.apiId = node.id;
    parent.append(value);
  }
}

function shellLabels(nodes) {
  const labels = new Map();
  for (const node of nodes) {
    const key = String(node.kind);
    const values = labels.get(key) ?? [];
    values.push(node.label);
    labels.set(key, values);
  }
  return labels;
}

function keymapDispatcher(bindings, send) {
  let pending = [];
  return (event) => {
    if (event.isComposing) return false;
    const stroke = [
      event.ctrlKey && "Ctrl", event.altKey && "Alt", event.metaKey && "Meta",
      event.shiftKey && "Shift", event.code,
    ].filter(Boolean).join("+");
    const candidate = [...pending, stroke];
    const matches = bindings.filter((binding) =>
      startsWith(binding.sequence.map(formatStroke), candidate));
    if (matches.length === 0) {
      pending = [];
      return false;
    }
    event.preventDefault();
    const complete = matches.find((binding) =>
      binding.sequence.length === candidate.length);
    if (complete) {
      pending = [];
      send({command_id: complete.command_id, arguments: null});
    } else {
      pending = candidate;
    }
    return true;
  };
}

function startsWith(sequence, prefix) {
  return prefix.length <= sequence.length &&
    prefix.every((stroke, index) => sequence[index] === stroke);
}

function formatStroke(stroke) {
  return [
    stroke.control && "Ctrl", stroke.alt && "Alt", stroke.meta && "Meta",
    stroke.shift && "Shift", stroke.code,
  ].filter(Boolean).join("+");
}

function element(document, tag, className, accessibleLabel) {
  const value = document.createElement(tag);
  value.className = className;
  if (accessibleLabel) value.setAttribute("aria-label", accessibleLabel);
  return value;
}
