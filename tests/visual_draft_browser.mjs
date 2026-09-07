const [debugUrl, serverUrl, token] = process.argv.slice(2);

function assert(value, message) {
  if (!value) throw new Error(message);
}

const targets = await (await fetch(`${debugUrl}/json`)).json();
const target = targets.find(value => value.type === "page" && value.url.startsWith(serverUrl));
assert(target, "Mustermark browser target was not found");

const socket = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  socket.addEventListener("open", resolve, {once: true});
  socket.addEventListener("error", reject, {once: true});
});
let nextId = 0;
const pending = new Map();
socket.addEventListener("message", event => {
  const message = JSON.parse(event.data);
  if (!message.id || !pending.has(message.id)) return;
  const {resolve, reject} = pending.get(message.id);
  pending.delete(message.id);
  if (message.error) reject(new Error(message.error.message));
  else resolve(message.result);
});
function command(method, params = {}) {
  const id = ++nextId;
  socket.send(JSON.stringify({id, method, params}));
  return new Promise((resolve, reject) => pending.set(id, {resolve, reject}));
}
async function evaluate(expression) {
  const result = await command("Runtime.evaluate", {
    expression,
    awaitPromise: true,
    returnByValue: true,
  });
  if (result.exceptionDetails) throw new Error(
    result.exceptionDetails.exception?.description || result.exceptionDetails.text);
  return result.result.value;
}
async function waitFor(expression, message) {
  for (let attempt = 0; attempt < 100; ++attempt) {
    if (await evaluate(expression)) return;
    await new Promise(resolve => setTimeout(resolve, 30));
  }
  throw new Error(message);
}

await command("Runtime.enable");
await waitFor("Boolean(typeof currentState !== 'undefined' && currentState && document.querySelector('.mm-structure'))",
              "Visual document did not become ready");

// Hold a forced refresh response until a new draft has opened and received text.
await evaluate(`(() => {
  const originalFetch=window.fetch;
  window.fetch=async (...args)=>{const response=await originalFetch(...args);
    await new Promise(resolve=>window.releaseDraftRefresh=resolve);return response};
  window.pendingDraftRefresh=refresh(true);window.fetch=originalFetch;return true;
})()`);
await waitFor("typeof window.releaseDraftRefresh === 'function'", "Refresh was not held");

assert(await evaluate(`(() => {
  const item=[...document.querySelectorAll('[data-mm-kind="item"]')]
    .find(value=>value.textContent.includes('and some notes'));
  const button=[...item.querySelectorAll('button')]
    .find(value=>value.getAttribute('aria-label')==='Edit Markdown');
  button.click();
  return Boolean(document.querySelector('.mm-inline-editor'));
})()`), "Inline edit did not receive focus");
await new Promise(resolve => setTimeout(resolve, 50));
assert(await evaluate(`(() => {
  const editor=document.querySelector('.mm-inline-editor');
  editor.value='- edited draft survives refresh';editor.focus();editor.setSelectionRange(9,9);
  return document.activeElement===editor && editor.selectionStart===9;
})()`), "Inline edit did not receive the test caret");
await evaluate("window.releaseDraftRefresh();window.pendingDraftRefresh");
assert(await evaluate(`(() => {const editor=document.querySelector('.mm-inline-editor');
  return editor?.value==='- edited draft survives refresh' && document.activeElement===editor &&
    editor.selectionStart===9})()`), "An older forced refresh erased the new draft");

let state = await (await fetch(`${serverUrl}api/state`)).json();
const task = state.nodes.find(node => node.task);
let response = await fetch(`${serverUrl}api/actions`, {
  method: "POST",
  headers: {"content-type": "application/json", "x-mustermark-token": token},
  body: JSON.stringify({action: "task_set", node: task.ref, checked: true,
                        baseRevision: state.revision, origin: "browser-test"}),
});
assert(response.ok, "External task update failed");
await new Promise(resolve => setTimeout(resolve, 250));
const preservedDraft = await evaluate(`(() => {
  const editor=document.querySelector('.mm-inline-editor');
  return {present:Boolean(editor),focused:document.activeElement===editor,
    value:editor?.value||'',start:editor?.selectionStart??-1,end:editor?.selectionEnd??-1,
    invalid:Boolean(activeDraft?.invalid)};
})()`);
assert(preservedDraft.present && preservedDraft.focused &&
       preservedDraft.value==='- edited draft survives refresh' &&
       preservedDraft.start===9 && preservedDraft.end===9,
       `Inline draft text, focus, or caret was lost during refresh: ${JSON.stringify(preservedDraft)}`);
assert(await evaluate(`([...document.querySelectorAll('[data-mm-task="true"]')]
  .find(value=>value.textContent.includes('and a checklist'))?.querySelector('input[type="checkbox"]')?.checked===true)`),
  "Content outside the active draft did not refresh");

await evaluate("document.querySelector('.mm-inline-editor').dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true}))");
await new Promise(resolve => setTimeout(resolve, 200));
assert(await evaluate("document.querySelector('.mm-inline-editor')?.value==='- edited draft survives refresh'"),
       "Stale commit removed the draft");
state = await (await fetch(`${serverUrl}api/state`)).json();
assert(!state.source.includes("edited draft survives refresh"), "Stale draft changed Markdown");

await evaluate("document.querySelector('.mm-inline-editor').dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}))");
await waitFor("!document.querySelector('.mm-inline-editor')", "Escape did not cancel the local draft");

state = await (await fetch(`${serverUrl}api/state`)).json();
const removedTarget = state.nodes.find(node => node.kind === "item" &&
  node.text.includes("and some notes"));
assert(await evaluate(`(() => {
  const item=[...document.querySelectorAll('[data-mm-kind="item"]')]
    .find(value=>value.textContent.includes('and some notes'));
  [...item.querySelectorAll('button')]
    .find(value=>value.getAttribute('aria-label')==='Edit Markdown').click();
  return Boolean(document.querySelector('.mm-inline-editor'));
})()`), "Deletion test draft did not open");
await new Promise(resolve => setTimeout(resolve, 50));
await evaluate(`(() => {const editor=document.querySelector('.mm-inline-editor');
  editor.value='- recover this deleted target';editor.focus();editor.setSelectionRange(12,12);return true})()`);
response = await fetch(`${serverUrl}api/actions`, {
  method: "POST",
  headers: {"content-type": "application/json", "x-mustermark-token": token},
  body: JSON.stringify({action: "delete", node: removedTarget.ref,
                        baseRevision: state.revision, origin: "browser-test"}),
});
assert(response.ok, "External target deletion failed");
await waitFor("Boolean(activeDraft?.invalid)", "Deleted draft target was not invalidated");
assert(await evaluate(`(() => {const editor=document.querySelector('.mm-inline-editor');
  return editor?.value==='- recover this deleted target' && document.activeElement===editor &&
    editor.selectionStart===12})()`), "Deleted-target draft was not recoverable");
await evaluate("document.querySelector('.mm-inline-editor').dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true}))");
await new Promise(resolve => setTimeout(resolve, 100));
assert(await evaluate("document.querySelector('.mm-inline-editor')?.value==='- recover this deleted target'"),
       "Invalid target commit removed its draft");
await evaluate("document.querySelector('.mm-inline-editor').dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}))");
await waitFor("!document.querySelector('.mm-inline-editor')", "Deleted-target draft did not cancel");

state = await (await fetch(`${serverUrl}api/state`)).json();
assert(await evaluate(`(() => {
  const heading=currentState.nodes.find(node=>node.kind==='heading'&&node.text==='and a subtitle');
  const element=document.querySelector('[data-mm-ref="'+CSS.escape(heading.ref)+'"]');
  beginInlineItem(element,heading,currentState,'task');
  return Boolean(document.querySelector('.mm-inline-input'));
})()`), "Provisional insertion did not open");
await new Promise(resolve => setTimeout(resolve, 50));
assert(await evaluate(`(() => {const input=document.querySelector('.mm-inline-input');
  input.value='dictated first line\\nsecond line';input.focus();input.setSelectionRange(10,10);
  input.dispatchEvent(new CompositionEvent('compositionstart',{data:'dictated'}));
  return document.activeElement===input&&input.selectionStart===10})()`),
  "Provisional insertion did not receive focus");
const remainingTask = state.nodes.find(node => node.task);
response = await fetch(`${serverUrl}api/actions`, {
  method: "POST",
  headers: {"content-type": "application/json", "x-mustermark-token": token},
  body: JSON.stringify({action: "task_set", node: remainingTask.ref,
                        checked: !remainingTask.checked, baseRevision: state.revision,
                        origin: "browser-test"}),
});
assert(response.ok, "External update during provisional insertion failed");
await new Promise(resolve => setTimeout(resolve, 200));
assert(await evaluate(`(() => {const input=document.querySelector('.mm-inline-input');
  return input?.value==='dictated first line\\nsecond line' && document.activeElement===input &&
    input.selectionStart===10 && !activeDraft?.invalid})()`),
  "Provisional multiline draft, focus, or caret was lost during refresh");
await evaluate("document.querySelector('.mm-inline-input').dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true}))");
await new Promise(resolve => setTimeout(resolve, 100));
assert(await evaluate("document.querySelector('.mm-inline-input')?.value==='dictated first line\\nsecond line'"),
       "Stale provisional commit removed the draft");
await evaluate("document.querySelector('.mm-inline-input').dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}))");
await waitFor("!document.querySelector('.mm-inline-input')", "Provisional insertion did not cancel");
state = await (await fetch(`${serverUrl}api/state`)).json();
assert(!state.source.includes("dictated first line"), "Cancelled provisional insertion changed Markdown");
socket.close();
