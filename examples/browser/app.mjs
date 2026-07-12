import {
  BrowserSession,
  isRejectedCommandResult,
  renderSession,
} from "./client.mjs";

export function consumeCredential(locationObject = location,
  historyObject = history) {
  const url = new URL(locationObject.href);
  const fragment = new URLSearchParams(url.hash.slice(1));
  const credential = fragment.get("credential");
  if (credential !== null) {
    fragment.delete("credential");
    url.hash = fragment.toString();
    historyObject.replaceState(null, "", url.href);
    return credential;
  }
  return url.searchParams.get("credential") ?? "";
}

if (typeof document !== "undefined") {
  const query = new URLSearchParams(location.search);
  const root = document.querySelector("#ssg");
  const credential = consumeCredential();
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
}
