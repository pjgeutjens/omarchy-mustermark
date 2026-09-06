# Mustermark

Mustermark is a minimal Linux editor for writing and reorganising Markdown as structured heading sections and nested list items. The Markdown source stays on screen and remains the source of truth.

The project is an early working prototype. It is built for Omarchy but runs on other Linux desktops with fallback colours.

## What works

- Write new Markdown, open a file, or reopen one of the 10 most recent files in one source editor.
- Select and move heading sections or list-item subtrees. Ordinary blocks can move within their section too.
- Change one heading to an exact level or shift the selected heading branch together.
- Autosave named files, detect conflicting disk changes, and keep recovery copies of unsaved edits.

Mustermark has no notebook database, vault, rendered block canvas, or permanent sidebar. Optional tracking adds stable IDs and labels to lists and items as hidden HTML comments inside the Markdown file.

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
| `Shift+H` / `Shift+L` | Promote or demote the selected heading or list item |
| `Ctrl+Shift+H` / `Ctrl+Shift+L` | Promote or demote it together with its child headings or list items |
| `Space` | Toggle the selected task item |
| `Ctrl+S` | Save immediately |
| `Ctrl+O` | Open a Markdown file |
| `Ctrl+Shift+O` | Open the recent-files chooser |

The footer exposes file and structural actions to the mouse. `recent` opens the recent-files chooser. For headings, `title H1` through `H6` changes only the selected heading. Turn on `+children` before `promote` or `demote` to shift the selected structure and its descendants while preserving their relative levels.

## Command line and local API

The executable also exposes the parser and safe source mutations:

```text
mustermark inspect FILE.md
mustermark validate FILE.md
mustermark track|repair|untrack FILE.md
mustermark apply FILE.md ACTION NODE [--scope=self|subtree] [--target=ID] [--label=LABEL] [--text=TEXT] [--level=N]
mustermark api --stdio
```

`promote` and `demote` use `--scope=self` by default. Pass `--scope=subtree` to apply the change to descendant headings or list items too.

`mustermark api --stdio` accepts newline-delimited JSON-RPC 2.0. Mutations use source ranges and revision checks. Mustermark refuses a rewrite when it cannot preserve the Markdown safely.

See [PLAN.md](PLAN.md) for the file format, API decisions, test gates, and unfinished work.

## License

[MIT](LICENSE)
