const [debugUrl, expectedSection] = process.argv.slice(2);

let targets;
for (let attempt = 0; attempt < 100; ++attempt) {
  try {
    targets = await (await fetch(`${debugUrl}/json`)).json();
    if (targets.some(value => value.type === "page" && value.url.includes("#preview"))) break;
  } catch {}
  await new Promise(resolve => setTimeout(resolve, 25));
}
const target = targets?.find(value => value.type === "page" && value.url.includes("#preview"));
if (!target) throw new Error("Mustermark Visual page was not exposed for focus inspection");

const socket = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  socket.addEventListener("open", resolve, {once: true});
  socket.addEventListener("error", reject, {once: true});
});
let nextId = 0;
const pending = new Map();
socket.addEventListener("message", event => {
  const message = JSON.parse(event.data);
  if (!pending.has(message.id)) return;
  pending.get(message.id)(message.result);
  pending.delete(message.id);
});
const evaluate = expression => new Promise(resolve => {
  const id = ++nextId;
  pending.set(id, result => resolve(result.result.value));
  socket.send(JSON.stringify({
    id,
    method: "Runtime.evaluate",
    params: {expression, returnByValue: true},
  }));
});

let focused;
for (let attempt = 0; attempt < 100; ++attempt) {
  focused = await evaluate(`(() => ({
    input: document.activeElement?.matches('.mm-inline-input') || false,
    label: document.activeElement?.getAttribute('aria-label') || '',
    section: document.querySelector('.mm-inline-input')?.closest('ul')
      ?.previousElementSibling?.textContent || ''
  }))()`);
  if (focused.input) break;
  await new Promise(resolve => setTimeout(resolve, 25));
}
socket.close();
if (!focused?.input || focused.label !== "New item" ||
    !focused.section.includes(expectedSection)) {
  throw new Error(`Append field was not focused in the requested section: ${JSON.stringify(focused)}`);
}
