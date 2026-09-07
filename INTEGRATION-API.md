# Linked-source API contract

Implemented integration contract, 2026-09-07. Mustermark owns Markdown and identity
metadata. Feed the Flock owns selection, queue priority, claims, and delivery
history. Opening, importing, and selecting sections never starts delivery.

## Initialization and identity storage

`document.link.initialize` takes `params.path` for an existing `.md` or `.markdown` file. Initialization is explicit; opening an ordinary Markdown file does not create linked metadata.
It initializes missing metadata or reads existing identity. `document.link.inspect`
requires an existing sidecar. Both return the normal document payload with
`documentId`, `identityGeneration`, `durableHeadingIdentityVersion: 1`, and
`durableTaskIdentityVersion: 1`. Heading and item nodes have `durableId`; eligible
section summaries carry their heading's durable ID. Session IDs remain separate.
Ordinary inspection never initializes metadata.

The adjacent `<source>.mustermark.json` is owned by Mustermark. New files have
owner-only permissions. Version 2 stores a document UUID, generation, revision,
base64 source snapshot, heading/item `nodes` with UUIDs and structural evidence,
retired UUIDs, and namespaced `bindings`. A version 1 heading sidecar upgrades on
read and preserves its heading identities. Older version 1 writers reject version
2 instead of discarding task metadata. Sources are limited to 2 MiB, sidecars to
16 MiB, and lock acquisition to one second.

At an unchanged revision, source ranges and the stored snapshot restore exact
identity, including identical items. Changed external source uses unique exact
structural evidence. Missing or ambiguous matches retire IDs; content hashes are
matching evidence, not identities. Controlled Mustermark edits carry identity
mappings through renames and moves. Copied items have distinct UUIDs. Ambiguous
external copies may retire both the original match and its binding.

GUI and API saves use the canonical sidecar lock and check the saved source and
metadata generation. A prepared write stores the next metadata generation before
source replacement, then finalizes it. Recovery accepts the old or new source;
a third revision pauses recovery. Source-only copies receive new identities on
explicit linking. Save As does not copy external bindings.

Move the source, sidecar, and relative assets together. Feed the Flock's **Locate
moved file** requires the old path to be absent and the new sidecar to match.
Duplicate document registrations and metadata older than the stored generation
are rejected. An unseen filesystem copy cannot be distinguished from a move
without outside history. Missing/corrupt sidecars and ambiguous matches pause the
operation rather than guessing. Prepared-boundary recovery is tested; sudden
power loss and filesystem durability guarantees are not.

## Section summaries

API version `0.2` adds `sectionSchemaVersion: 1`, `sections`, and
`unsectionedLists`. Each section has its heading `node`, ancestor-text `path`,
direct `lists`, and their outer `items`. Arrays follow document order. A heading
qualifies at any depth when it directly owns a list, even with intervening
paragraphs. Lists under subheadings belong to those subheadings. Nested items are
context inside their outer task, never independent tasks. Root lists are outside
heading selection. Repeated labels must be distinguished by identity.

Saved reads refresh the visible workspace every five seconds and before claims.
New headings start unselected. An observed loss of direct-list eligibility clears
selection and queue membership; restoration requires reselection. Read errors
retain selection and pause delivery. Unsaved editor drafts are never input.
There is no cross-process dirty-buffer detection: the last saved file can be
claimed while an editor has an unfinished draft.

## External bindings

Stdio methods `document.external_id.bind` and `document.external_id.resolve`
require `path`, `baseRevision`, `namespace`, and opaque external `value`. Bind also
requires `node`. For linked files it accepts a current durable node UUID or live
session identity and returns the durable UUID with `identityScope: "document"`.
Bindings persist in the sidecar across API process restarts. Ordinary unlinked
sessions keep the earlier session-only behavior and require a live session ID.
Inspect/apply advertise `externalIdBindingVersion: 1` and `externalIdScope`;
their general `identityScope: "session"` still describes session node references.

```json
{"jsonrpc":"2.0","id":1,"method":"document.external_id.bind","params":{"path":"/tmp/tasks.muster.md","baseRevision":"REVISION","namespace":"feed-the-flock","value":"task-123","node":"DURABLE-UUID"}}
```

Resolve omits `node`. Success includes `ok`, `revision`, `identityScope`,
`namespace`, `value`, and `node`. A namespace matches
`[a-z0-9][a-z0-9_.-]{0,63}`. Values are nonblank, case-sensitive, at most 256 UTF-16
code units, and cannot contain NUL. A node can have multiple values; a value can
refer to only one node per document/namespace. Repeating a binding is idempotent.
Neither operation changes Markdown bytes or the source revision. Binding can
advance the metadata generation. Deleted bindings remain retired and cannot be
reassigned to another task, including an identical task.

Errors include `base_revision_required`, `stale_revision`, `invalid_external_id`,
`node_required`, `unknown_id`, `external_id_collision`, `unknown_external_id`,
and `external_id_retired`. Read failures include `invalid_identity_file`,
`identity_recovery_conflict`, `identity_file_missing`, `identity_locked`, and
`stale_identity_generation`. HTTP and one-shot CLI commands do not expose binding
operations. Feed the Flock uses fresh stdio processes with five-second timeouts
and bounded output (12 MiB stdout, 8 KiB stderr).

## Snapshots and claims

`document.snapshot` accepts item or section scope. An optional `baseRevision`
rejects stale source. `includeAttachmentData: true` adds `dataBase64` to attachment
descriptors; total returned image bytes are limited to 8 MiB. Images must be
supported regular relative files inside the document directory. Each unit contains
exact Markdown, text, outer task state, durable identity, and attachment hashes.

Feed the Flock projects direct items into `muster_tasks` using saved identities.
Its explicit `muster_queue` section order has priority; items inside each section
follow the file, regardless of the native FIFO/LIFO setting. An individual Feed
now claim contains only that task. A queued section admits newly saved direct
items. Global one-by-one/batch and section/all-section modes are reused. Native
queues remain available separately; switching sources requires stopping feeding.

A claim binds external task IDs, captures all selected units, then begins an
immediate SQLite transaction. It rereads source identity/revision and revalidates
each unit's content and image hashes. It checks current section selection and
competing claims, atomically marks the tasks claimed, and stores the immutable
input as a `prepared` claim. The accepted input is the last successfully validated
snapshot committed by that transaction. Source and asset changes after validation
affect future claims. SQLite cannot lock external editors or image writers; this
is optimistic validation, not a distributed filesystem transaction. A stale
claim is rejected and the worker pauses with a refresh/retry message.

Claims freeze complete Markdown subtrees and image bytes, including every item in a batch. Delivery retains nested paragraphs, code fences, and list context rather than using the shorter summary text. Images also pass Feed the Flock's 8,192-pixel dimension and 40-million-pixel limits. A resumed
prepared claim uses the stored payload, never a rebuilt prompt. Claims are capped
at 100 tasks, 512 KiB prompt text, and 8 MiB images. Oversized batches require a
smaller scope. Submission records `submitting` before transport. A process lost in
that state becomes `uncertain`; transport errors also require explicit review.
**Allow retry** retains the old attempt and returns tasks to pending. It warns
that the previous attempt may have reached the target. There is no exactly-once
transport guarantee. Delivery locks and claim checks prevent overlapping native
and linked submissions from overwriting active state.

## Lifecycle and writeback

Unchecked outer checkbox items and ordinary bullet/ordered items are eligible
when pending inside the queued scope. Initially checked items are excluded.
Manually checking a pending task removes eligibility without inventing a receipt.
Unchecking a completed task does not resend it; **Reset** creates a new execution
generation while retaining history. Nested items stay context in all cases.

Successful transport becomes active. Completion follows the existing Feed the
Flock lifecycle: observed departure from `working`, disappearance of the target,
or twelve seconds without observing work. This records lifecycle completion,
not verification of the agent's answer. The separate twelve-second prompt
transport timeout is an uncertain submission outcome.

Completed claims create separate pending writebacks. For checkbox tasks the
adapter resolves the durable external binding at the current revision and checks
that claimed content, nested context, and attachment hashes still agree. Only
the outer checkbox changes. Its existing checked state is idempotent; a changed
child checkbox is a context conflict. Unrelated document edits are allowed after
a fresh revision read. Ordinary bullets remain bullets with completion stored in
Feed the Flock. Deleted/replaced tasks cannot redirect writeback to another item.

A writeback error keeps the receipt and pauses linked feeding. **Retry checkbox
update** never invokes transport. **Keep source unchanged** resolves the writeback
without changing the document. Reset requires stopped feeding and resolved active
claims, unchecks the outer checkbox, increments execution generation, and
supersedes old writebacks. Reset and writeback are serialized in SQLite. Pending
writebacks resume during worker or target polling, including after reopening the
workspace. Completion does not require a visible editor.

## Opening, native handoff, and retained history

One Import picker routes `.muster.md` files to linking and ordinary Markdown to
the existing bucket importer. **Open workspace** keeps the native editor.
**Open in Mustermark** on a native workspace creates a private folder under
`Downloads/Mustermark/<UUID>/` (or `FEED_THE_FLOCK_EXPORT_DIR`) with a linked
`workspace.muster.md` and copied managed images. It validates task boundaries and
snapshots, retains external task IDs and completed state, and records an immutable
archive of original records and receipts. The original native content remains in
SQLite and is protected against edits; it is excluded from capture and native
queues. The linked file becomes authoritative. Handoff requires stopped feeding
and capture, and preserves at least one native workspace for capture. It is
bounded to 1,000 tasks, 2 MiB Markdown, and 8 MiB images.

A failed handoff leaves the native workspace unchanged and removes newly generated
output files before returning the error. Returning an archived workspace to native editing is
not implemented. Unlinking keeps files, assets, retained records, and receipts.
Relinking the same document restores its source identity and execution history;
completed tasks do not silently become pending. Keep the state database and the
source/sidecar/assets in backups. Source identity tombstones and execution
history have no automatic garbage collection in this version.
