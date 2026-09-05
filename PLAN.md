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
- Give every parsed node a reference scoped to the current file revision.
- Give lists and list items persistent IDs only when tracking is enabled.
- Treat headings as movable section containers, but do not add persistent heading IDs in the first release.

### Tracking format

Tracking is opt-in for each file. Opening or rearranging an ordinary Markdown file must not insert metadata.

```markdown
<!-- mustermark:tracking version=1 -->

<!-- mustermark:list id=lst_550e8400-e29b-41d4-a716-446655440000 labels=planning -->
- [ ] Define the API
  <!-- mustermark:item id=itm_6ba7b810-9dad-41d1-80b4-00c04fd430c8 labels=next,research -->
```

- Use typed UUIDv4 values with `lst_` and `itm_` prefixes.
- Store labels in the same hidden comments so they travel with the file.
- Labels are unique lowercase slugs. Permit letters, digits, `_`, `-`, `.`, and `/`.
- Add IDs to new app-created lists and items when a file is tracked.
- Report externally added items without IDs as incomplete tracking. Do not rewrite them during inspection.
- Report duplicate IDs as errors. Repair them only through an explicit command.

### Mutations and saving

- Support task toggles, label changes, text edits, deletion, movement, nesting, heading promotion, and heading demotion.
- Move full heading and list-item subtrees, including their metadata and nested content.
- Change only the required source ranges. Do not serialise the whole syntax tree back to Markdown.
- Preserve line endings, list markers, indentation style, permissions, and final-newline state where the requested operation permits it.
- Require a SHA-256 base revision for every mutation.
- Reject stale revisions instead of overwriting an external edit.
- Save through atomic replacement.
- When a move would change Markdown meaning in an uncertain way, leave the file unchanged and return `unsafe_rewrite`.

### CLI and machine interface

Use one `mustermark` executable:

```text
mustermark FILE.md
mustermark inspect|validate|track|repair|untrack FILE.md
mustermark apply FILE.md ACTION NODE [--target=ID] [--label=LABEL] [--text=TEXT]
mustermark api --stdio
```

- Use JSON-RPC 2.0 over newline-delimited standard input and output.
- Start at API version `0.1`; do not promise compatibility until Feed the Flock or a Neovim client uses it.
- Keep the API file-addressed. Do not add a daemon, folder watcher, or permanent index in the first release.
- Return typed failures for stale revisions, missing IDs, duplicate IDs, malformed metadata, invalid destinations, and unsafe rewrites.

## Application interaction

### Main layout

- The window contains one Markdown source editor and a footer. There is no permanent outline, preview, split view, menu bar, or top toolbar.
- Markdown syntax stays visible. The editor starts in Insert mode and works as a normal text editor.
- `Esc` selects the narrowest structural unit at the cursor. A heading selection includes its section; a list-item selection includes its nested children.
- In Normal mode, moving the pointer across the source changes the structural selection. Clicking the source returns to Insert mode at that position.
- The selected source range gets a low-contrast background. Do not add block cards, borders, handles, or controls inside the document.

### Structural controls

- Put the available actions in the footer. Do not open a menu when a structure is selected.
- A heading gets move up/down, exact `H1` through `H6`, and branch promote/demote actions. Exact heading level changes only the selected title. Branch actions change that title and every descendant title by the same amount.
- A list item gets move up/down among its siblings, outdent/indent, and task toggle when applicable. Nested children move with their item.
- A paragraph, quote, code block, or other top-level block gets move up/down within its section.
- Disable or omit actions that cannot apply to the current selection.

### Modal keyboard model

- `Esc`: enter Normal mode and select the narrowest structural node at the source cursor.
- `i` or `Enter`: enter Insert mode at the selected structure.
- Clicking in source: enter Insert mode.
- `J` / `K`: select the next or previous structure.
- `Shift+J` / `Shift+K`: move the selected subtree down or up.
- `Shift+H` / `Shift+L`: promote or outdent; demote or indent.
- `Space`: toggle a task.

Always call the modes `NORMAL` and `INSERT`. Do not use `COMMAND` as the visible name.

## Footer

The footer is the only permanent application chrome. It borrows Powerline's mode and status grouping without requiring separators, icons, or a decorated bar.

The default footer contains plain text segments:

- Left: mode, new, open, and save.
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
- Debounced autosave applies to named files; `Ctrl+S` remains available.
- Write recovery snapshots for unsaved changes and clear them after a successful save.
- Reload an external change automatically when the local buffer is clean.
- If local changes exist, stop autosave and offer Reload or Save Copy. Never overwrite silently.
- Preserve selection by persistent ID where available, then by the nearest source position.

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
- Verify IDs across moves, nesting changes, wording edits, external reordering, and restarts.
- Check explicit tracking, incomplete tracking, duplicate repair, label queries, and metadata removal.
- Check stale-revision rejection and atomic-write recovery.
- Exercise each action through the engine, JSON interface, keyboard, and mouse.
- Test a document with at least 10,000 lines. Measure warm launch and structural action times.
- Inspect the rendered application at desktop and narrow widths, with multiple Omarchy themes and a non-Omarchy fallback.
- Make the build available on the ThinkPad and test it there before claiming release readiness.

## Prototype state on 2026-09-05

The unfinished prototype is in this repository.

Implemented:

- CMake build using Qt 6 and `cmark-gfm`.
- C++ parser and source-range mutation engine.
- Opt-in list and item IDs with labels.
- CLI commands and JSON-RPC standard-I/O loop.
- Atomic file writes with revision checks.
- Qt Quick application shell and source editor.
- Omarchy colour loading and Markdown syntax colouring.
- Normal/Insert mode switching.
- One direct Markdown editing surface with no menu, toolbar, sidebar, or preview.
- Structural selection and range highlighting in the source.
- Contextual footer controls for heading sections, list items, and other blocks.
- Separate exact-title and whole-branch heading level changes.
- A restrained footer with mode, file actions, save state, tracking, and position.
- Engine tests covering round trips, tracking, labels, duplicate repair, moves, task toggles, and external-write rejection.
- An offscreen QML test covering typing, mode switches, footer movement, task toggle, and keyboard branch changes.

Last verified commands:

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-declarative qt6-svg cmark-gfm
cmake -S . -B /tmp/mustermark-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/mustermark-build -j2
ctest --test-dir /tmp/mustermark-build --output-on-failure
qmllint qml/Main.qml
```

Both the engine and offscreen QML tests pass in the current build. Software-rendered Insert and Normal mode frames were inspected. The app mapped on Omarchy, but the desktop locked before the physical-display frame could be inspected. It was not tested on the ThinkPad, packaged, installed, or published.

Known unfinished work:

- Refactor the footer into the configurable component specified above.
- Inspect the rebuilt window on an unlocked Omarchy desktop. The current run mapped successfully, but the screen locked before a physical-display capture could be checked.
- Test pointer hover, pointer clicks, autosave, open/save dialogs, and conflict recovery by hand.
- Add creation commands for new lists and items to the structural API.
- Finish shortcut remapping, recent files, recovery restoration, package metadata, README, and license file.
- Review list indentation rules beyond the covered fixtures. Keep refusing uncertain rewrites.

## Recommended restart

Start by reading this file and running both test executables. The next slice is an unlocked desktop check of the direct editor, followed by manual autosave and conflict testing. Keep the list-oriented Markdown API independent of the GUI so a future Neovim client can call the same safe mutations.
