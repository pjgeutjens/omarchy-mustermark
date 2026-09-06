# Mustermark restart plan

## Product statement

Mustermark is a native app and local API for writing, navigating, tracking, and reorganising Markdown headings, lists, and list items with full mouse and keyboard control.

The distinction from Omawrite is structural editing. Omawrite is a small text-first Markdown writer. Mustermark treats headings, lists, and list subtrees as things that can be selected, moved, nested, folded, labelled, and tracked without hiding the Markdown source.

The app is useful only if rearranging a real document is faster and clearer than doing the same work manually in Neovim.

## Product boundaries

- Support new and existing `.md` files.
- Keep Markdown as the source of truth. Do not create a notebook, vault, graph, or managed document database.
- Work with one explicit file per window.
- Show the Markdown syntax. Do not replace source editing with a Notion-style block canvas.
- Keep the application native and fast. Use Qt Quick/QML with a C++ document engine.
- Read the active Omarchy theme and fall back cleanly on another Linux desktop.
- Provide every operation through both mouse and keyboard controls.
- Preserve unfamiliar Markdown as opaque source ranges. Refuse a structural operation when its meaning cannot be preserved safely.
- Limit external drag-and-drop in the first release. Internal headings, lists, and item subtrees are the drag targets.
- Keep Feed the Flock delivery state, queues, errors, and in-flight snapshots outside Mustermark.
- Use the MIT license. Do not publish or submit the app without confirming the destination account.

## Shared Markdown engine

Build the app and engine together in small end-to-end slices. The GUI must call the same operations that the CLI exposes.

### Parsing

- Parse CommonMark plus GFM tables, task lists, autolinks, and strikethrough with `cmark-gfm`.
- Return headings, lists, nested items, tasks, and opaque blocks with exact byte and line ranges.
- Give every parsed node a reference scoped to the current file revision and a compact content fingerprint.
- Give every node a unique in-memory session ID while tracking is active.
- Treat headings as movable section containers.

### Tracking and identity

Tracking is explicit and temporary. Opening, tracking, or stopping tracking must not insert metadata into a Markdown file.

- Generate a unique `s:<session>:<counter>` ID for every parsed node while tracking is active.
- Keep an ID across controlled moves, level changes, task changes, and text edits. Return its updated source range after each action.
- Hold labels in memory under the session ID. Labels disappear when tracking stops or the process exits.
- Keep labels as unique lowercase slugs containing letters, digits, `_`, `-`, `.`, or `/`.
- Generate a 96-bit Base64URL content fingerprint from a version marker, node kind, and normalised direct text.
- Do not treat a fingerprint as identity. Identical items can have the same fingerprint.
- Reconcile direct source typing by content and structural position. A controlled operation supplies the target ID so duplicate items remain distinguishable.
- Keep removal of old inline Mustermark comments as a migration command. Do not create new inline tracking comments.

### Mutations and saving

- Support task toggles, label changes, text edits, deletion, movement, nesting, heading promotion, and heading demotion.
- Move full heading and list-item subtrees, including nested content.
- Change only the required source ranges. Do not serialise the whole syntax tree back to Markdown.
- Preserve line endings, list markers, indentation style, permissions, and final-newline state where the requested operation permits it.
- Require a SHA-256 base revision for every mutation.
- Reject stale revisions instead of overwriting an external edit.
- Save through atomic replacement.
- When a move would change Markdown meaning in an uncertain way, leave the file unchanged and return `unsafe_rewrite`.

### CLI and machine interface

Use one `mustermark` executable:

```text
mustermark FILE.md [--serve[=PORT]]
mustermark inspect|validate|strip-metadata FILE.md
mustermark apply FILE.md ACTION NODE [--target=REF] [--checked=true|false] [--text=TEXT]
mustermark api --stdio
```

- Use JSON-RPC 2.0 over newline-delimited standard input and output.
- Keep tracking state and labels for the lifetime of the standard-I/O process.
- Start at API version `0.1`; do not promise compatibility until Feed the Flock or a Neovim client uses it.
- Keep the API file-addressed. Do not add a daemon, folder watcher, or permanent index in the first release.
- Return typed failures for stale revisions, missing IDs, invalid destinations, and unsafe rewrites.

### Document-scoped HTTP serving

- `mustermark serve FILE.md [--port=N]` starts a foreground loopback server for one explicit file. It is not a daemon, folder watcher, or permanent index.
- `mustermark FILE.md --serve[=PORT]` lets the source editor and browser share one live document session.
- The HTML view reads the active Omarchy colour roles and falls back to its bundled palette elsewhere.
- `--html-style=FILE.css` loads a user stylesheet after the defaults and active theme. The source editor keeps its native QML and Omarchy styling.
- `GET /api/state` returns the parsed document, safe enriched HTML, and recent instructions. `GET /api/view` returns the HTML fragment.
- `GET /api/events` emits a change event after source, tracking, or label state changes.
- `POST /api/actions` accepts the same structural operations as the CLI. Writes require a per-process token and the current base revision.
- The built-in page listens for change events and retains a polling fallback. The instruction stream can be hidden.
- HTML carries `data-mm-kind`, `data-mm-ref`, temporary `data-mm-id` values while tracking, fingerprints, labels, and task state. Source HTML is never passed through to the browser.
- While tracking, headings and list items are draggable and task checkboxes write through the same action endpoint.
- Completed tasks use a CSS strikethrough in HTML by default. Every HTML style remains overridable through the user stylesheet.

## Application interaction

### Main layout

- The window contains one Markdown source editor and a footer. There is no permanent outline, preview, split view, menu bar, or top toolbar.
- Markdown syntax stays visible. The editor starts in Insert mode and works as a normal text editor.
- `Esc` selects the narrowest structural unit at the cursor. A heading selection includes its section; a list-item selection includes its nested children.
- In Normal mode, moving the pointer across the source changes the structural selection. Clicking the source returns to Insert mode at that position.
- The selected source range gets a low-contrast background. Do not add block cards, borders, handles, or controls inside the document.

### Structural controls

- Put the available actions in the footer. Do not open a menu when a structure is selected.
- A heading gets move up/down, exact `H1` through `H6`, and promote/demote actions. Exact heading level changes only the selected title. Promotion and demotion can affect either that title or its descendant titles too.
- A list item gets move up/down among its siblings, promote/demote, and task toggle when applicable. Promotion and demotion always carry nested child items with it; leaving children behind is not a supported mode.
- A paragraph, quote, code block, or other top-level block gets move up/down within its section.
- Disable or omit actions that cannot apply to the current selection.

### Modal keyboard model

- `Esc`: enter Normal mode and select the narrowest structural node at the source cursor.
- `i` or `Enter`: enter Insert mode at the selected structure.
- Clicking in source: enter Insert mode.
- `J` / `K`: select the next or previous structure.
- `Shift+J` / `Shift+K`: move the selected subtree down or up.
- `Shift+H` / `Shift+L`: promote or demote the selected heading, or a list item with its children.
- `Ctrl+Shift+H` / `Ctrl+Shift+L`: promote or demote a heading and its descendant headings.
- `Space`: toggle a task.
- `Ctrl+Shift+O`: open the recent-files chooser.

Always call the modes `NORMAL` and `INSERT`. Do not use `COMMAND` as the visible name.

## Footer

The footer is the only permanent application chrome. It borrows Powerline's mode and status grouping without requiring separators, icons, or a decorated bar.

The default footer contains plain text segments:

- Left: mode, new, open, recent, and save.
- Centre: the selected structure and its available actions.
- Right: save status, tracking state, and cursor position.

Footer configuration is a later slice. It must not add a settings window to the main editing flow.

- Allow users to reorder, show, and hide segments independently on the left and right.
- Provide configurable full and thin separators for both directions.
- Allow each segment to select foreground and background colour roles from the active theme.
- Allow custom text for built-in segments and static user segments.
- Reload status-line configuration without restarting the app.
- Keep click actions for built-in segments: new, open, save, tracking, mode, structural actions, and position.
- Provide built-in values for mode, file name, save state, status message, tracking state, selected structure, and cursor line/column.
- Use a Nerd Font when available. Supply plain triangular separators if Powerline glyphs are absent.
- Store the user override under `$XDG_CONFIG_HOME/mustermark/statusline.json` or an equally direct text format.
- Ship a documented default configuration that reproduces the app default exactly.
- Do not execute arbitrary shell commands from status-line configuration in the first release. A future provider interface may add external values after its security and refresh behaviour are specified.

## File behaviour

- New opens an empty recoverable buffer. Save As establishes its path.
- Keep the 10 most recently opened or newly saved files. Show them in an in-app chooser with mouse, arrow, `J`/`K`, and Enter controls.
- Debounced autosave applies to named files; `Ctrl+S` remains available.
- Write recovery snapshots for unsaved changes and clear them after a successful save.
- Reload an external change automatically when the local buffer is clean.
- If local changes exist, stop autosave and offer Reload or Save Copy. Never overwrite silently.
- Preserve selection by session ID while tracking, then by the nearest source position.

## Market references

- Zettlr and Obsidian already let users navigate headings and drag heading sections. Mustermark must handle nested list items with the same directness.
  - <https://docs.zettlr.com/en/sidebar/table-of-contents/>
  - <https://obsidian.md/help/plugins/outline>
- Org mode is the keyboard reference for folding, moving, promoting, and demoting subtrees.
  - <https://orgmode.org/manual/Structure-Editing.html>
- Logseq and SiYuan provide block identity inside much larger knowledge systems. Mustermark should remain file-first.
- Joplin and QOwnNotes are broader note managers. Mustermark should not acquire notebook or sync responsibilities.
- Omawrite is the visual and product-size reference, while Mustermark remains structure-first.
  - <https://github.com/omacom/omawrite>

## Test gates

- Prove byte-for-byte identity after parsing and saving an unchanged file.
- Check nested lists, mixed markers, ordered lists, multiline items, tasks, duplicate text, headings, front matter, tables, HTML, Unicode, CRLF, and missing final newlines.
- Assert that each mutation changes only its declared byte ranges.
- Verify session IDs across controlled moves, nesting changes, wording edits, and duplicate content.
- Prove that starting tracking, adding labels, and stopping tracking leave source bytes unchanged.
- Check tracking teardown, label queries, content fingerprints, and old-metadata removal.
- Check stale-revision rejection and atomic-write recovery.
- Exercise each action through the engine, JSON interface, keyboard, and mouse.
- Test a document with at least 10,000 lines. Measure warm launch and structural action times.
- Inspect the rendered application at desktop and narrow widths, with multiple Omarchy themes and a non-Omarchy fallback.
- Make the build available on the ThinkPad and test it there before claiming release readiness.

## Prototype state on 2026-09-06

The unfinished prototype is in this repository.

Implemented:

- CMake build using Qt 6 and `cmark-gfm`.
- C++ parser and source-range mutation engine.
- Temporary session IDs, content fingerprints, and in-memory labels without Markdown metadata.
- CLI commands and a stateful JSON-RPC standard-I/O loop.
- Atomic file writes with revision checks.
- A loopback HTTP server with safe HTML, token-protected actions, and change events.
- One live session shared by the GUI and HTTP server when launched with `--serve`.
- Browser drag-and-drop for headings and list items, plus writable task checkboxes.
- Omarchy-aware HTML CSS variables with a final user stylesheet override.
- Default checked-task strikethrough, with task and label presentation replaceable in user CSS.
- Qt Quick application shell and source editor.
- Omarchy colour loading and Markdown syntax colouring.
- Normal/Insert mode switching.
- One direct Markdown editing surface with no menu, toolbar, sidebar, or preview.
- Structural selection and range highlighting in the source.
- Contextual footer controls for heading sections, list items, and other blocks.
- Separate exact-title and whole-branch heading level changes.
- A restrained footer with mode, file actions, save state, tracking, and position.
- Engine tests covering round trips, clean session tracking, labels, duplicate content, moves, task toggles, and external-write rejection.
- An offscreen QML test covering typing, mode switches, footer movement, task toggle, and keyboard branch changes.
- An HTTP scenario covering clean tracking, temporary labels, structural changes, task changes, and stale revisions.

Last verified commands:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-declarative qt6-svg cmark-gfm
cmake -S . -B /tmp/mustermark-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/mustermark-build -j2
ctest --test-dir /tmp/mustermark-build --output-on-failure
qmllint qml/Main.qml
```

All five engine, QML, HTTP, standard-I/O, and shared-session tests pass in the current release build. The HTTP tests need loopback access. A two-window editor/HTML session was staged and inspected on Omarchy, including typing, list continuation, structural keyboard actions, stable IDs across mutations, multiple labels, and default-versus-user CSS. It has not been tested on the ThinkPad or packaged for a distribution.

Known unfinished work:

- Refactor the footer into the configurable component specified above.
- Test pointer hover, pointer clicks, autosave, open/save dialogs, and conflict recovery by hand.
- Decide and implement the proposed `AUTO` versus `SESSION` persistence policy, including explicit save/discard behaviour for an ephemeral working document.
- Add creation commands for new lists and items to the structural API.
- Finish shortcut remapping and recovery restoration.
- Review list indentation rules beyond the covered fixtures. Keep refusing uncertain rewrites.

## Recommended restart

Start by reading this file and running all test targets. The next slice after the shared-session demo is manual autosave and conflict testing. Keep the list-oriented Markdown API independent of the GUI so Feed the Flock or a future Neovim client can call the same source operations.
