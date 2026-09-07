# Work with the open document through the API

This example checks a task through HTTP while the same document is open in Mustermark. It needs curl and jq. Use a disposable file:

```sh
printf '# Project\n\n- [ ] Review the draft\n  - Keep the examples short\n' > /tmp/mustermark-api-example.md
mustermark /tmp/mustermark-api-example.md --serve
```

The startup JSON contains a localhost `url` and a write `token`. Leave the application open. Press `Shift+V` in Normal mode to watch the rendered view, or stay in the source editor.

In another terminal, replace the example URL and token with the values printed by your session:

```sh
MUSTERMARK_URL='http://127.0.0.1:PORT'
MUSTERMARK_TOKEN='TOKEN_FROM_STARTUP'

state=$(curl -fsS "$MUSTERMARK_URL/api/state")
printf '%s' "$state" | jq '.nodes[] | select(.task == true) | {ref, text, checked}'

request=$(printf '%s' "$state" | jq -ce '
  .revision as $revision |
  first(.nodes[] | select(.task == true)) |
  {action: "task_set", node: .ref, checked: true, baseRevision: $revision}
')
curl -fsS "$MUSTERMARK_URL/api/actions" \
  -H "X-Mustermark-Token: $MUSTERMARK_TOKEN" \
  -H 'Content-Type: application/json' \
  --data "$request"
```

The selected task becomes checked in the document. Its child note stays attached. If the document changed after the read, the server returns HTTP 409 instead of applying a request based on old content. Read again and decide whether the operation still makes sense; do not blindly overwrite the newer revision.

To follow document changes, a client can hold an event stream open:

```sh
curl -N "$MUSTERMARK_URL/api/events"
```

The HTTP server binds to `127.0.0.1`. Keep its write token local. The API is for local tools, not an authenticated internet service.

## Other ways to connect

`mustermark inspect FILE.md` returns parsed structure without opening a window. A separate one-shot CLI invocation should use the returned `sourceRef`, because the live `ref` belongs to the inspect process's session. `mustermark api --stdio` provides JSON-RPC 2.0 sessions over newline-delimited JSON. `mustermark serve FILE.md` serves a document without the GUI.

Snapshots return exact Markdown, plain text, task state, and supported relative attachment descriptors. A section snapshot contains its outer list items in source order; nested items remain context within their parent. A snapshot is a copied value, so a consumer can retain the input it acted on even as the live document changes.

Session IDs identify individual nodes, including duplicate text. Content fingerprints are matching evidence and may be identical for duplicate items. Persistent document identities and namespaced external bindings require explicit linked-file initialization, described in the [integration contract](../INTEGRATION-API.md). Ambiguous external edits can retire an identity rather than silently assign it to another item.

The API is version `0.2` and is still evolving. Clients should handle rejected edits and invalidated identities. See the [README reference](../README.md#live-html-and-api) for endpoints and actions.
