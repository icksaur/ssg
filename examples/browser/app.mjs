import {
  BrowserSession,
  isRejectedCommandResult,
  renderSession,
} from "./client.mjs";

const query = new URLSearchParams(location.search);
const root = document.querySelector("#ssg");
const credential = query.get("credential") ?? "remote";
const websocket = query.get("websocket") ??
  `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/session`;

const session = new BrowserSession({
  url: websocket,
  credential,
  render(snapshot, result) {
    if (isRejectedCommandResult(result)) {
      root.setAttribute("aria-label", result.message || "Command rejected");
      return;
    }
    renderSession(root, snapshot, {
      send: (command, options) => session.send(command, options),
      invokeStatusAction: (action) => session.invokeStatusAction(action),
    });
  },
});
session.connect();
