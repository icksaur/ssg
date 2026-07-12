const modifierOrder = [
  ["ctrlKey", "Ctrl"],
  ["altKey", "Alt"],
  ["metaKey", "Meta"],
  ["shiftKey", "Shift"],
];

export function canonicalStroke(event) {
  const modifiers = modifierOrder
    .filter(([property]) => event[property])
    .map(([, label]) => label);
  return [...modifiers, event.code].join("+");
}

export function expandKeymap(keymap) {
  const {prefix, alphabet, width, when} = keymap.encoding;
  return keymap.commands.map((command_id, index) => {
    const suffix = [];
    let value = index;
    for (let position = 0; position < width; ++position) {
      suffix.unshift(`Key${alphabet[value % alphabet.length]}`);
      value = Math.floor(value / alphabet.length);
    }
    if (value !== 0) {
      throw new RangeError("keymap encoding cannot represent every command");
    }
    return {
      sequence: [prefix, ...suffix],
      command_id,
      context: when,
    };
  });
}

export class BrowserInputAdapter {
  #browser;
  #bindings;
  #capabilities;
  #emit;
  #internalClipboard;
  #pending = [];
  #publishStatus;
  #reserved;
  #suppressCommittedInput = false;

  constructor({
    browser = "unknown",
    bindings = [],
    reserved = [],
    capabilities = [],
    emit = () => {},
    publishStatus = () => {},
    internalClipboard = "",
  } = {}) {
    this.#browser = browser;
    this.#bindings = bindings;
    this.#reserved = reserved;
    this.#capabilities = new Set(capabilities);
    this.#emit = emit;
    this.#publishStatus = publishStatus;
    this.#internalClipboard = internalClipboard;
  }

  handleKeyDown(event) {
    if (event.isComposing) {
      return false;
    }
    const stroke = canonicalStroke(event);
    const candidate = [...this.#pending, stroke];
    if (this.#isReserved(candidate)) {
      this.#pending = [];
      return false;
    }

    const matches = this.#bindings.filter((binding) =>
      startsWith(binding.sequence, candidate));
    if (matches.length === 0) {
      this.#pending = [];
      return false;
    }

    event.preventDefault();
    const complete = matches.find((binding) =>
      binding.sequence.length === candidate.length);
    if (complete) {
      this.#pending = [];
      this.#emit({command_id: complete.command_id, arguments: {}});
    } else {
      this.#pending = candidate;
    }
    return true;
  }

  handleCompositionStart() {
    this.#suppressCommittedInput = false;
  }

  handleBeforeInput(event) {
    if (event.inputType === "insertCompositionText") {
      return false;
    }
    if (event.inputType === "insertFromComposition" &&
        this.#suppressCommittedInput) {
      this.#suppressCommittedInput = false;
      return false;
    }
    if (event.inputType === "insertText" && event.data) {
      this.#emit(textInsert(event.data));
      return true;
    }
    return false;
  }

  handleCompositionEnd(event) {
    if (!event.data) {
      return false;
    }
    this.#emit(textInsert(event.data));
    this.#suppressCommittedInput = true;
    return true;
  }

  activateHitTarget(target) {
    this.#emit(target.command);
  }

  handleWheel(event) {
    const rows = event.deltaMode === 1
      ? Math.trunc(event.deltaY)
      : Math.sign(event.deltaY);
    if (rows === 0) {
      return false;
    }
    event.preventDefault();
    this.#emit({command_id: "view.scroll_lines", arguments: {rows}});
    return true;
  }

  handleScrollbar(numerator, denominator) {
    if (!Number.isSafeInteger(denominator) || denominator <= 0) {
      throw new RangeError("scrollbar denominator must be positive");
    }
    const bounded = Math.max(0, Math.min(denominator, Math.trunc(numerator)));
    this.#emit({
      command_id: "view.scroll_to_fraction",
      arguments: {numerator: bounded, denominator},
    });
  }

  async copy(text, {clipboard, secureContext, userGesture} = {}) {
    this.#internalClipboard = text;
    if (!secureContext || !userGesture || !clipboard?.writeText) {
      return {exported: false};
    }
    try {
      await clipboard.writeText(text);
      return {exported: true};
    } catch {
      return {exported: false};
    }
  }

  async paste({clipboard, secureContext, userGesture} = {}) {
    let text;
    let source = "internal_register";
    if (secureContext && userGesture && clipboard?.readText) {
      try {
        text = await clipboard.readText();
        source = "system_clipboard";
      } catch {
        text = undefined;
      }
    }
    if (text === undefined) {
      text = this.#internalClipboard;
      this.#publishStatus({
        id: "clipboard.system_paste_unavailable",
        action_label: "Paste from system clipboard",
      });
    }
    this.#emit(textInsert(text));
    return {source};
  }

  shouldExposeFileDrop() {
    return this.#capabilities.has("local_file_drop");
  }

  openDroppedFiles(files) {
    if (!this.shouldExposeFileDrop()) {
      return false;
    }
    this.#emit({
      command_id: "file.open_dropped_content",
      arguments: {
        files: files.map((file) => ({
          suggested_name: file.name,
          bytes: Array.from(file.bytes),
        })),
      },
    });
    return true;
  }

  #isReserved(candidate) {
    return this.#reserved.some((entry) =>
      entry.browser === this.#browser &&
      startsWith(candidate, entry.sequence));
  }
}

function startsWith(sequence, prefix) {
  return prefix.length <= sequence.length &&
    prefix.every((stroke, index) => sequence[index] === stroke);
}

function textInsert(text) {
  return {command_id: "text.insert", arguments: {text}};
}
