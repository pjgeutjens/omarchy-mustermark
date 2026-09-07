# Mustermark

Mustermark is a minimal Linux application for writing and reorganising Markdown as structured heading sections and nested list items. Normal, Insert, and rendered Visual mode share the same Markdown document.

The same structural operations are available to scripts and agents through a CLI, JSON-RPC, and a live HTTP API. A tool can move a section or complete a task while you watch the change in the editor. Markdown stays the shared file.

This is an early release for Omarchy and Arch Linux on x86_64. Other Linux desktops and architectures are not yet release-tested.

Watch the demo (2 min 26 sec):

https://github.com/user-attachments/assets/c4fe6105-bad9-4ec2-a218-769d9400a1bd

The demo covers structural editing, Normal/Insert/Visual modes, image attachments, a few CSS color changes, and the live API.

## An editor your tools can work with

Mustermark exposes document structure: headings, nested lists, tasks, source ranges, and attachments. Tools can address a specific item, move its subtree, change its task state, or take a snapshot of a section through the same core operations used by the editor.

- **Live editing:** the HTTP API shares the open document with Normal, Insert, and Visual modes. Successful API changes appear in that session; change events let other clients follow along.
- **Revision protection:** mutations include the revision they read. A stale request is rejected so the client can reread the document before trying again.
- **Identity:** live session IDs survive controlled moves. Tools that need identity across restarts can explicitly initialize a linked sidecar and bind their own external IDs.
- **Several entry points:** use CLI commands for shell scripts, persistent JSON-RPC over stdio for a tool process, or localhost HTTP for a live client. No hosted account is required.

For example, an agent can read a task with its child notes and images, act on that snapshot, and mark the original task complete. Another tool can reorganize a heading section without reconstructing the whole Markdown file. Mustermark supplies the document operations; your tool supplies the workflow.

[Try a complete API example](docs/API-QUICKSTART.md) · [HTTP reference](#live-html-and-api) · [Persistent identity contract](INTEGRATION-API.md)

## What works

- Write new Markdown, open a file, or reopen one of the 10 most recent files in one source editor.
- Select and move heading sections or list-item subtrees. Ordinary blocks can move within their section too.
- Change one heading to an exact level or shift the selected heading branch together.
- Paste or drop an image onto a list item. The relative Markdown image link moves with the item.
- Autosave named files, detect conflicting disk changes, save a local copy, and restore recovery drafts.
- Serve the open document as live HTML. Dragging a heading or item and checking a task in the browser changes the Markdown and the editor.
- Open a focused provisional task or bullet with `mustermark append` for typing, paste, input methods, or system dictation.
- Export a bounded item or section snapshot through the CLI, JSON-RPC, or loopback HTTP API.

Mustermark has no notebook database, vault, rendered block canvas, permanent sidebar, menu bar, or toolbar.

Every running document session assigns temporary node IDs automatically. IDs and temporary labels stay in memory and never change the Markdown file.

Optional tools can link documents through the [integration API](INTEGRATION-API.md). Linking creates a `<source>.mustermark.json` sidecar for persistent identities and external bindings; keep it beside the Markdown file. Mustermark runs independently of Feed the Flock. Delivery and queue behavior belong to that separate application.

## Install or update

Install the current early release:

```sh
curl -fsSL https://raw.githubusercontent.com/pjgeutjens/omarchy-mustermark/main/install.sh | sh
```

It downloads a pinned source release, verifies its SHA-256 checksum, and builds against your system Qt libraries with two build jobs. Missing Arch packages require confirmation and sudo; the application installs as your normal user under `~/.local`. A failed build leaves the active installation unchanged. Running the command again installs the version named by the current installer. Restart the app after an update.

To inspect the script first, choose a version, or uninstall:

```sh
curl -fsSL https://raw.githubusercontent.com/pjgeutjens/omarchy-mustermark/main/install.sh -o install.sh
less install.sh
sh install.sh --version 0.2.0
sh install.sh --uninstall
```

Uninstall removes only the installer's application files. Documents, images, settings, and recovery data remain. System dependencies also remain installed. The installer refuses to overwrite a manual installation; move it aside yourself or use `--prefix /absolute/path`. Prefixes currently support letters, digits, slashes, periods, underscores, and hyphens. Earlier builds remain under `~/.local/lib/mustermark` until uninstall. Checksums detect download corruption; they are not an independent signature of the publisher.

Report problems through [GitHub Issues](https://github.com/pjgeutjens/omarchy-mustermark/issues), with your Mustermark version, desktop, and steps to reproduce. Use a disposable example document when reporting editing problems.

## Build on Arch Linux

Install the build dependencies:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-declarative qt6-svg qt6-webengine cmark-gfm curl jq nodejs chromium
```

Configure, build, and test:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Run it against a Markdown file:

```bash
./build/mustermark notes.md
```

That opens Editor Mode, the default keyboard-native Markdown source view. Open Visual Mode as a rendered, mouse-native application instead with either form:

```bash
./build/mustermark notes.md --visual
./build/mustermark --visual notes.md
```

The modes share the same parser, source mutations, revision checks, and file format. Their controls are intentionally suited to their interfaces rather than being identical.

Or install it for the current user:

```bash
cmake --install build --prefix "$HOME/.local"
```

Qt 6.6 or newer and `cmark-gfm` are required. Package names differ outside Arch Linux.

## Editor Mode controls

Mustermark starts a new, untitled document in `INSERT` mode, where normal typing and text selection work as expected. Opening an existing file starts in `NORMAL` mode with its first structure selected.

| Input | Action |
| --- | --- |
| `Esc` | Enter `NORMAL` mode and select the structure at the cursor |
| `?` in `NORMAL` | Show or hide the keybindings reference, with actions for the selected structure first |
| `i` or `Enter` in `NORMAL` | Enter `INSERT` at the selected structure's text, after its Markdown marker |
| `o` / `O` in `NORMAL` | Add the same structural type below or above and start typing |
| `a` or `Ctrl+Enter` in `NORMAL` | Append an item at the bottom of the selected item's current list, or the selected list |
| `A` or `Ctrl+Shift+Enter` in `NORMAL` | Prepend an item at position 1 of the selected item's current list, or the selected list |
| `Enter` at the end of a list item | Continue its bullet, number, or unchecked task marker |
| `J` / `K` | Select the next or previous structure |
| `Left` | Select the current structure's parent; from an item, this selects its list |
| `Shift+J` / `Shift+K` | Move the selected structure down or up |
| `Shift+H` / `Shift+L` | Promote or demote the selected heading, or a list item with its children |
| `Ctrl+Shift+H` / `Ctrl+Shift+L` | Promote or demote a heading together with its child headings |
| `Space` | Toggle the selected task item |
| `d` or `Delete` in `NORMAL` | Delete the selected paragraph, item, list, or heading section; `Ctrl+Z` restores it |
| `P` or `Ctrl+V` | Attach the clipboard image to the current list item |
| `Ctrl+Shift+V` | Paste clipboard text; from `NORMAL`, return to `INSERT` first |
| `Ctrl+Shift+A` in `NORMAL` | Open the selected item's images; use `J/K` to select and `D` to remove |
| `Shift+V` in `NORMAL` | Switch to rendered Visual mode in the same window |
| `Ctrl+Z` in `NORMAL` | Undo the latest committed Editor change; an Insert-mode session is one step |
| `Ctrl+S` | Save immediately |
| `Ctrl+O` | Open a Markdown file |
| `Ctrl+Shift+O` | Open the recent-files chooser |

Clicking source text preserves the current mode. In Normal mode it selects the structure under the pointer; in Insert mode it places the text cursor. The compact footer shows mode, document path, save or conflict status, Visual mode, and cursor position; paths under the home directory start with `~`, narrow paths are middle-elided, and hovering reveals the absolute path. Conflict and recovery actions appear only when needed. Structural actions stay in Normal mode and the contextual `?` keybinding guide. A single heading cannot be moved to the same or a deeper level than one of its descendant headings. List items always move with nested child items. Ordered-list additions, removals, and rearrangements keep sequential source markers. Select an item's parent list with `Left`, then use `Shift+J/K` to move the whole list past a sibling paragraph or list.

Visual mode uses the main window's document controller and loopback server. API diagnostics are hidden initially and remain available from the header. `Ctrl+Z` undoes the latest successful document change; an active inline field retains its own text undo until committed. Escape dismisses a placeholder, inline editor, image, or dialog before returning to Normal mode. `Ctrl+Q` closes Mustermark. Committed rendered edits update the shared source and are saved to disk.

The document has no editing chrome until the pointer enters a structure. Hovering over a section, list, item, or paragraph shows its controls without selecting it. The control bar sits on the nearest horizontal edge of that structure, close to the hover point, and stays inside the window. Use the handle to drag the structure, `+` to add an item, `§+` to add a section, the arrows to move or change depth, and `×` to remove it. An item's `▧+` control opens the system image picker. Empty structures are removed immediately; populated structures ask for confirmation. Select `Don't ask again for removals in this preview` to skip the remaining confirmations until that preview closes. Adding inserts a focused placeholder in the document; Enter commits it and Escape cancels it. Right-click a structure to copy its exact Markdown directly, without opening a menu. The pencil replaces any heading, list, item, or paragraph with an inline Markdown editor; clipboard Paste is available there through the browser. Enter commits, Shift+Enter adds a line, and Escape cancels the edit. Paragraphs and whole lists can move between heading sections. Task checkboxes remain directly clickable. Click a list item to select it, then press `Ctrl+V` to attach an image from the clipboard. Each rejected action stays next to its request in the API-instructions pane with its error code and message.

In the source editor, `o` creates the same type below the selected structure and `O` creates it immediately above. From an item or list, `a` or `Ctrl+Enter` appends a new final item to that list; `A` or `Ctrl+Shift+Enter` prepends item 1. A heading added below lands after the selected heading's full section, including child headings and their contents. Task items start unchecked, list markers and heading levels are retained, and the cursor begins after the new marker. Enter accepts the addition, Shift+Enter adds a continuation line, and Escape restores the document and selection from before the operation. Autosave waits until the addition is accepted.

## Fast append and system dictation

Open Visual Mode at a focused provisional item with:

```bash
mustermark append notes.md --section="Inbox" --kind=task
mustermark append notes.md --section="Ideas" --kind=bullet
```

The section name must match one heading exactly. With no `--section`, Mustermark uses the last append section saved for that document, then the selected item's section, then the first heading. The preference is stored outside Markdown. Enter commits and saves the item. Shift+Enter adds a continuation line. Escape cancels without changing the file. A second command activates Visual mode in the existing window for that file.

The installed desktop file also has an **Append item in Visual Mode** action. System dictation works through the same focused text area as typing and paste; Mustermark does not record or transcribe audio.

## Images

Save the Markdown file before attaching an image. Press `Ctrl+V` to attach a clipboard image to the list item at the selection or insertion cursor. `Ctrl+Shift+V` pastes clipboard text. In `NORMAL`, `P` is an image-paste alias and `Ctrl+Shift+A` opens the selected item's images. You can also drop a local image file onto a list item. PNG, JPEG, WebP, and GIF files are accepted up to 8 MiB and 8192 by 8192 pixels.

Mustermark copies the file into a document-specific `<name>.assets` directory beside the Markdown file and adds a relative image line inside the selected item:

```markdown
- [ ] Compare the layouts
  ![clipboard image](review-notes.assets/7a82d7c0.png)
```

Visual Mode can add images too: hover an item and use its `▧+` control for a file, or click the item and press `Ctrl+V` for a clipboard image. An active inline Markdown editor retains normal text-paste behavior. Visual Mode displays each associated image as an icon, including images attached to ordered-list items. Click an icon to open its image, click it again to close it, or click another icon to switch images. Right-click the open image to copy its image data to the system clipboard. Put custom rules in `~/.config/mustermark/preview.css`; that file loads after Mustermark's defaults. The attachment selectors are `.mm-attachment`, `.mm-attachment-icon`, and `.mm-attachment-preview`. Press `Ctrl+R` in Visual Mode after editing the CSS file.

The image line is part of the item subtree, so moving the item keeps the association. `Ctrl+Shift+A` opens the selected item's attachment list and its remove controls. Mustermark deletes a file on removal only when it is inside the managed asset directory.

The association format is ordinary Markdown. A hosted client such as Feed the Flock can import the relative file into its own attachment store while keeping delivery state outside Mustermark.

## Live HTML and API

Open the editor and serve the same live session:

```bash
mustermark notes.md --serve
```

Use `--serve=4000` to choose a port. A headless session is also available:

```bash
mustermark serve notes.md
```

The HTML view reads the same active Omarchy colour roles as the source editor. On another Linux desktop it uses fallback colours.

Pass a CSS file to either form with `--html-style=FILE.css`. Mustermark loads its defaults and active theme first, then your file, so every HTML rule can be replaced without changing the app:

```bash
mustermark notes.md --serve --html-style=examples/html-theme.css
```

The example changes the accent and label treatment, then renders completed tasks as red text without Mustermark's default strikethrough.

The default HTML style strikes through completed task text. Override the selector `[data-mm-task="true"][data-mm-checked="true"]` to change or remove that treatment.

The first output line contains its loopback URL and a new write token:

```json
{"ok":true,"url":"http://127.0.0.1:35839/","token":"...","path":"/path/to/notes.md"}
```

Open the URL in a browser. Hover controls work immediately and can add, rename, move, indent, outdent, or remove sections and list items. Dragging starts only from the handle. Task checkboxes can also be changed there. The source editor and browser share one session and update each other.

The server listens only on `127.0.0.1` and exposes:

- `GET /` — the live HTML document and an optional instruction pane.
- `GET /api/state` — Markdown source, parsed nodes, invalidated session IDs, revision, safe HTML, and recent instructions.
- `GET /api/view` — the semantic HTML fragment with `data-mm-*` attributes.
- `GET /api/events` — change events for live consumers.
- `POST /api/actions` — one structural action. It requires the startup token in `X-Mustermark-Token` and the current SHA-256 revision in `baseRevision`.
- `POST /api/attachments` — an authenticated PNG, JPEG, WebP, or GIF body for a selected item. It uses the same base-revision protection and document-owned asset storage as Editor Mode.

Accepted actions include `snapshot`, `undo`, `section_add`, `list_add`, `item_add`, `move_before`, `move_after`, `replace`, `promote`, `demote`, `set_heading_level`, `task_set`, `toggle_task`, `attachment_add`, `attachment_remove`, `label_add`, `label_remove`, `edit`, and `delete`. `tracking_start` and `tracking_stop` remain as deprecated, idempotent compatibility calls. `undo` restores the source before the latest successful document-changing action in that live session. `section_add` inserts a same-level heading after the selected section. `list_add` creates a list under a heading, while `item_add` appends to a selected list or inserts after a selected item. `replace` updates one structure from a Markdown string and rejects content that changes its structural type or creates extra peers. `attachment_add` accepts a safe document-relative `path` and optional `alt` text. Heading level changes accept `scope: "self"` or `scope: "subtree"`. List-item level changes always include children.

Raw HTML from the Markdown is omitted from the rendered view. A stale mutation returns HTTP 409.

### Session identity

Each node gets a unique session ID such as `s:3f67a211:8` when a document session opens. It lasts until the process exits. A controlled move keeps that ID and returns its new source range. Ambiguous source edits retire the old ID instead of assigning it to the nearest candidate. Temporary labels are indexed by the session ID and disappear with the session.

Every parsed node also has a compact content fingerprint such as `mm1:item:...`, derived from its kind and normalised direct text. Fingerprints can be regenerated but are not unique: two identical items intentionally share one. Consumers should address mutations with the session ID and use the fingerprint for matching or diagnostics.

Neither value is inserted into ordinary Markdown. `strip-metadata` exists only to remove comments made by early Mustermark prototypes:

```bash
mustermark strip-metadata notes.md
```

## Other machine interfaces

The parser and one-shot source operations are available from the command line:

```text
mustermark inspect FILE.md
mustermark validate FILE.md
mustermark apply FILE.md ACTION NODE [--scope=self|subtree] [--target=REF] [--checked=true|false] [--text=TEXT] [--level=N]
mustermark snapshot FILE.md NODE [--scope=item|section]
mustermark append FILE.md [--section=HEADING] [--kind=task|bullet]
mustermark api --stdio
```

`mustermark api --stdio` accepts newline-delimited JSON-RPC 2.0. It keeps document sessions in memory for the life of that process. API version `0.2` adds automatic identity, invalidation reporting, and `document.snapshot`. Mutations use source ranges and revision checks. Mustermark refuses a rewrite when it cannot preserve the Markdown safely.

`inspect` returns both `ref`, the live session ID for that invocation, and `sourceRef`, the revision-scoped locator. Pass `sourceRef` to a separate one-shot `apply` or `snapshot` command. Stateful stdio and HTTP clients should use `ref` so controlled moves retain identity.

Snapshots are copied values. Item scope returns one item subtree. Section scope returns its top-level list items as independent units in document order. Each unit contains exact Markdown, deterministic plain text, task state, and descriptors for supported relative images. Images must be regular non-symbolic-link files inside the document directory and no larger than 8 MiB. Snapshot creation never changes the document.

## Demo scenario

The included scenario begins with a normal Markdown file, then changes it through the HTTP API:

```bash
cp examples/html-serving-demo.md /tmp/mustermark-html-demo.md
mustermark serve /tmp/mustermark-html-demo.md
MUSTERMARK_DEMO_DELAY=1.4 ./scripts/demo-html-scenario.sh URL TOKEN
```

It checks a task, adds two temporary labels, changes one heading and a whole heading branch, and nests then unnests a list-item subtree. Each instruction appears in the optional pane. The page can issue the same actions from its hover controls, drag handles, and task checkboxes.

The release build has seven automated targets: engine, QML, HTTP, stdio, shared GUI/server, Visual activation, and a headless Chromium draft-preservation test. HTTP and browser targets need loopback access.

See the [API quickstart](docs/API-QUICKSTART.md) for a live editing example and [release maintenance](docs/RELEASING.md) for validation and archive preparation.

## License

[MIT](LICENSE)

Visual mode shares the main window and document with Normal and Insert. Press `Shift+V` in Normal mode to enter the rendered view; `Escape` first dismisses an inline edit or popup, then returns to Normal at the selected structure. `i` enters the text of that structure. Existing files open in Normal; new files open in Insert.
