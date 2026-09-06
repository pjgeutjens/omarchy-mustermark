# Mustermark

Mustermark is a minimal Linux editor for writing and reorganising Markdown as structured heading sections and nested list items. The Markdown source stays on screen and remains the source of truth.

The project is an early working prototype. It is built for Omarchy but runs on other Linux desktops with fallback colours.

## What works

- Write new Markdown, open a file, or reopen one of the 10 most recent files in one source editor.
- Select and move heading sections or list-item subtrees. Ordinary blocks can move within their section too.
- Change one heading to an exact level or shift the selected heading branch together.
- Autosave named files, detect conflicting disk changes, and keep recovery copies of unsaved edits.
- Serve the open document as live HTML. Dragging a heading or item and checking a task in the browser changes the Markdown and the editor.

Mustermark has no notebook database, vault, rendered block canvas, permanent sidebar, menu bar, or toolbar.

Tracking belongs to the running editor or server session. It assigns temporary node IDs and holds temporary labels in memory. Starting or stopping tracking does not change the Markdown file. Structural changes still write the requested Markdown change.

## Build on Arch Linux

Install the build dependencies:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-declarative qt6-svg cmark-gfm
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

Or install it for the current user:

```bash
cmake --install build --prefix "$HOME/.local"
```

Qt 6.6 or newer and `cmark-gfm` are required. Package names differ outside Arch Linux.

## Controls

Mustermark starts in `INSERT` mode, where normal typing and text selection work as expected.

| Input | Action |
| --- | --- |
| `Esc` | Enter `NORMAL` mode and select the structure at the cursor |
| `?` in `NORMAL` | Show or hide the keybindings reference |
| `i`, `Enter`, or click | Return to `INSERT` mode |
| `Enter` at the end of a list item | Continue its bullet, number, or unchecked task marker |
| `J` / `K` | Select the next or previous structure |
| `Shift+J` / `Shift+K` | Move the selected structure down or up |
| `Shift+H` / `Shift+L` | Promote or demote the selected heading, or a list item with its children |
| `Ctrl+Shift+H` / `Ctrl+Shift+L` | Promote or demote a heading together with its child headings |
| `Space` | Toggle the selected task item |
| `Ctrl+S` | Save immediately |
| `Ctrl+O` | Open a Markdown file |
| `Ctrl+Shift+O` | Open the recent-files chooser |

The footer exposes file and structural actions to the mouse. `recent` opens the recent-files chooser. For headings, `title H1` through `H6` changes only the selected heading. Turn on `+children` before `promote` or `demote` to shift a heading and its descendant headings while preserving their relative levels. List items always move with nested child items.

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

Open the URL in a browser. Start tracking from either footer. While tracking is active, headings and list items can be dragged in the HTML view and task checkboxes can be changed there. The source editor and browser share one session and update each other.

The server listens only on `127.0.0.1` and exposes:

- `GET /` — the live HTML document and an optional instruction pane.
- `GET /api/state` — Markdown source, parsed nodes, revision, safe HTML, tracking state, and recent instructions.
- `GET /api/view` — the semantic HTML fragment with `data-mm-*` attributes.
- `GET /api/events` — change events for live consumers.
- `POST /api/actions` — one structural action. It requires the startup token in `X-Mustermark-Token` and the current SHA-256 revision in `baseRevision`.

Accepted actions include `tracking_start`, `tracking_stop`, `move_before`, `move_after`, `promote`, `demote`, `set_heading_level`, `task_set`, `toggle_task`, `label_add`, `label_remove`, `edit`, and `delete`. Heading level changes accept `scope: "self"` or `scope: "subtree"`. List-item level changes always include children.

Raw HTML from the Markdown is omitted from the rendered view. A stale mutation returns HTTP 409.

### Identity during tracking

Each node gets a unique session ID such as `s:3f67a211:8`. It lasts until tracking stops or the process exits. A controlled move keeps that ID and returns its new source range. Temporary labels are indexed by that ID and disappear when tracking stops.

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
mustermark api --stdio
```

`mustermark api --stdio` accepts newline-delimited JSON-RPC 2.0. It keeps temporary tracking sessions in memory for the life of that process. Mutations use source ranges and revision checks. Mustermark refuses a rewrite when it cannot preserve the Markdown safely.

## Demo scenario

The included scenario begins with a normal Markdown file, then tracks and changes it through the HTTP API:

```bash
cp examples/html-serving-demo.md /tmp/mustermark-html-demo.md
mustermark serve /tmp/mustermark-html-demo.md
MUSTERMARK_DEMO_DELAY=1.4 ./scripts/demo-html-scenario.sh URL TOKEN
```

It checks a task, adds two temporary labels, changes one heading and a whole heading branch, and nests then unnests a list-item subtree. Each instruction appears in the optional pane. The same actions can be initiated through drag-and-drop and task checkboxes in the page.

See [PLAN.md](PLAN.md) for product boundaries, API decisions, test gates, and unfinished work.

## License

[MIT](LICENSE)
