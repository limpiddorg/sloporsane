# Daemon protocol — first session implementation

Transport: Unix stream socket, one JSON object per line (JSONL). The daemon sends
`{"type":"daemon.ready","protocolVersion":7}` on connection. This does not create
a session. Incoming frames are limited to 16 MiB. Malformed requests close the
connection; valid requests that fail return `request.failed` with an error string.
MCP field validation failures instead return a generic `request.failed` without
echoing credential-bearing parse diagnostics or closing the connection.

## Workspace switching

The model's native `workspace` tool takes an absolute `path` to an existing
directory and moves the same conversation there, with or without Git. It can
enter another repository, another repository's worktree or their non-Git parent
task folder. It uses the same event-queue transition, snapshot boundaries,
permission root and `session.updated` notifications as `worktree`; it does not
create a directory, move files or start a new conversation. Skills and MCP stay
inherited from the original configuration workspace across all moves and resume;
project instructions follow the active directory. Relative paths, missing paths
and files are rejected without changing the active workspace.

The same explicit-request/explicitly-invoked-skill rule, Sane availability and
Implement/review restrictions apply to both native tools, without another
approval popup. `workspace` is a model tool, not an additional Textual slash
command or socket request. Internally it submits `switch_directory` through the
workspace handler. `worktree` retains its Git-only semantics and creation/selection
still moves the conversation, using the repository of the current workspace.

The model's native `worktree` tool lists, creates or selects Git worktrees and
moves the attached principal conversation in place. A successful tool switch publishes `session.updated` with
`info.workspace` and `info.configuration_workspace`; Textual updates the visible
workspace without resetting chat. Both paths are persisted in session metadata
and restored by `session.attach`. Old sessions without `configuration_workspace`
use their `workspace` as configuration scope.

The daemon handles the switch through its event queue while the agent waits,
persists the new workspace/snapshot boundary, then resumes the worker with the
updated tool directory and permission root. Source configuration stays inherited.
The native tool has no extra approval step; its instructions require a direct
user request or a clear instruction in a skill/workflow explicitly invoked by the
user. Suggestions in files or workflows discovered by the model do not authorize
creation/switching; ambiguous intent requires asking first. Native creation/switching
are allowed in Sane on explicit request, but forbidden while Implement is active
or during review. Commit/merge/push remain ordinary Bash calls. See
[agent workspaces](git-snapshots.md#agent-workspaces).

Textual `/worktree` uses these additive protocol-7 client requests:

- `worktree.list` returns `worktree.listed`, `worktrees`, the current `workspace`, and `info`.
- `worktree.create` with `name` creates from committed HEAD and moves the conversation.
- `worktree.switch` with absolute `path` selects a registered worktree of the same repository.
- `worktree.delete` with absolute `path` removes a clean, unlocked, unused worktree, keeping its branch.

Rows include `primary`, `current`, `opened`, and `deletable` flags (missing flags are false).
`opened` covers all connected integrations, attached sessions and sessions being loaded
in this daemon. The primary checkout and opened worktrees cannot be deleted.
Deletion reserves the destination while Git runs so concurrent attach/switch/configure
requests cannot start using it. It returns `worktree.deleted` with the refreshed
`worktrees`, current `workspace`, and `info`. The client confirms deletion first;
the daemon rechecks usage and Git refuses local changes or locks, without force.

Create/switch return `worktree.switched`, `workspace`, `configuration_workspace`,
`sessionId`, `info`, and a notice; every response echoes `requestId`. They require
Slop, no active generation, summary, BTW or Implement. Listing is available in
Sane. Before the first message, listing uses the configured launch workspace;
a successful mutation creates a durable empty conversation without a model call.
Existing conversations retain their identity and history. Failures return
`request.failed`; UI errors do not close the picker or discard the entered name.

## Implementation execution

These additive requests use protocol 7 and the attached Implement `sessionId`.
Ordinary `message.send` remains planning-only until execution starts; afterward
questions use `implementation.review`. The client cannot grant writable authority
by supplying a phase in `message.send`.

- `implementation.start`: `pace` is `step_by_step` or `everything`; requires a finalized plan and an idle active branch.
- `implementation.auto_plan`: same `pace` and attached branch `sessionId`. Generates the implementation plan in Sane, then automatically accepts only the finalized result and starts construction with that pace. An already-ready plan starts directly. It rejects archives, active construction, invalid paces and concurrent BTW work. Step review/import remains human-controlled.
- `implementation.state`: returns durable construction and review state; also transfers the existing review conversation when present.
- `implementation.resume`: explicitly resumes an interrupted construction item. It never bypasses current permissions.
- `implementation.validate`: zero-based `step` and `substep`; real-substep indicators are independent until finalization. `substep == number of real substeps` finalizes the next step and accepts all its indicators (the client confirms unchecked indicators first).
- `implementation.unvalidate`: same selection; clears only the selected real-substep indicator. Finalized steps cannot be reverted.
- `implementation.review`: selection plus `text`, optional `path` and zero-based file-local `hunk`, and optional `skills` (one to six raw `{name,input}` invocations). Starts or continues the selected step's independent read-only review conversation. Only the first unvalidated, completed step and its substeps are eligible, in either pace. The daemon validates the selected file/hunk against preserved diffs. All skills load before branch mutation/generation; their exact tool calls/results are persisted with `user_skill` provenance. Skills do not grant execution permissions or leave review mode.
- `implementation.review.cancel`: cancels only the review answer, not construction.
- `implementation.rework`: accepts the correction or revised-plan proposal using its integer `revision` token, in either pace. Pauses/drains active construction before restoring verified isolated state. Requires finished review, a current unvalidated completed step and no import intent from that step onward. Starts corrections immediately; permissions still apply.
- `implementation.rework.reject`: dismisses the proposal using its `revision` token and resumes paused construction without changing the plan/results.
- `implementation.revision.open`: accepts opening revised-plan discussion using `revision`, not execution. Returns `revision_planning: true` and status `revision_planning`; construction remains paused.
- `implementation.revision.review`: `text` continues the independent read-only candidate-plan discussion. Returns `implementation.revision.review.accepted` then BTW transfers marked `revision_planning`. The discussion exposes only the plan-proposal tool, not local rework or editing tools.
- `implementation.history`: `step` selects previous results by stable identity; `all: true` includes all previous results, including removed steps. Returns `implementation.history` with `versions` (each has `revision`, `request`, `plan`, `state`). With `all: true`, `implementations` also contains one complete immutable workspace view per accepted correction archive, oldest first, excluding the live version. Each view includes its original plan/steps, Activities, diffs, repository baselines, `accepted_plan_markdown`, `version_request`, and `status: archived`. These are projections of persisted evidence, not a checkout or replay.

`implementation.updated.implementation` includes `revision`, `proposed_rework`
and `accepted_rework` strings (empty when absent), `proposed_revision`,
`accepted_plan_markdown`, `proposed_plan_markdown` and `history_count`.
`version_count` counts the initial implementation and each committed correction,
including local corrections without plan changes. `version_request` is the accepted
request introducing the live version (empty for the initial version).
Steps expose stable `identity`, execution `generation`, `revising` and `history_count`.

Auto planning publishes `info.auto_plan_pending`, `auto_plan_running`,
`auto_plan_pace` and `auto_plan_error`. Pace/error are saved in branch metadata;
empty strings represent absent values. While pending, Textual opens the steps
workspace with no speculative steps and a spinner beside `Implementation plan`.
The Objective Plan's `Implement directly (Auto plan)` first asks Step by step /
Everything, then opens the selected objective's branch and sends `implementation.auto_plan`.
The ordinary `Implementation plan` action retains its manual plan-review path.
Planning cancellation, failure, or completion without a finalized plan does not
start construction. The UI stops the spinner and offers retry using the saved
pace, including after `/resume`; the daemon never silently restarts on restore.
Successful automatic acceptance returns `implementation.updated` and queues the
normal first step. No second client start request or plan-confirmation popup is required.
`display_revision` identifies a fresh presentation pass independently of captured
ownership. `progress_boundary` excludes retained/replayed evidence from its progress
counter, without removing that evidence from the final diff. Each revised pass must
finish or explicitly skip its current substeps before new technical validation.
`previous_versions` provides lightweight superseded step/substep titles and their
`revision`/`plan_number`, excluding immutable validated prefixes. Textual no longer
renders these as struck-through menu rows; its global selector uses the complete
`implementations` history views instead.
`revision_branch` identifies the separate candidate-plan discussion. `plan_versions`
contains numbered immutable accepted versions (`number`, `beforeStep`, `plan`,
`markdown`); `beforeStep` is the stable identity at which that plan took effect.
Textual does not interleave plan-version links with steps. The first version is the
initial plan. No new version is added during proposal
or discussion. A plan-bearing `implementation.rework` is refused until
`implementation.revision.open` was accepted; final acceptance then appends the
version and starts correction. Local step corrections do not add plan versions.
The review's `propose_step_rework` or `propose_implementation_revision` result
publishes a proposal only after successful turn
completion. Cancellation, failure or a changed result does not publish it.
Validate/unvalidate requests should echo `revision`; after a proposal has changed
the revision token, missing or stale tokens are refused. Untouched older runs
retain compatibility with requests without that field. Validation is blocked while
a proposal awaits acceptance/dismissal or while rollback is in progress.

State version 7 preserves accepted requests/plans, identities, execution generations,
original diff boundaries, reconciliation sources and previous results. Only version 7
is accepted; older versions fail without rewriting state. Bulk history/source
evidence is omitted from live state and available on demand. `revision_pending`
pauses scheduling; `revising` journals rollback before committing the new plan.
Everything then re-examines retained built successors without auto-validation;
Step by step waits for human validation after each corrected step. A plan proposal
includes `request`, complete `plan`, `retained_steps` (new position → old position
or null), and `substep_owners` (one old-to-new substep mapping per new position,
empty for new steps). The daemon checks the immutable validated prefix. Worktree
rollback/replay intents are persisted separately and can be retried after interruption.

New `implementation.start` requests immediately persist the accepted run and return
`implementation.updated` with `status: "running"`, `preparing: true` and the first
step marked `preparing: true`. Textual opens construction and selects that step
before the isolated checkout is populated. Preparation is not a planned substep
and does not advance progress. `checkout_pending` persists the outstanding checkout
creation so an interrupted or failed preparation can be resumed explicitly.
The next construction event creates an isolated linked Git worktree outside
the user workspace. Existing Git projects require a committed SHA-1 HEAD; plain
directories use a local private repository in SOS storage, without a `.git` in the
source. Copying honors `.gitignore` rules and always excludes `.sos/` directories.
`info.workspace` remains the
original identity; `info.execution_workspace` is the private tool/review directory.
Final step validation imports only that step's preserved changes into the original
files, without touching the user's index or committing on their branch. Conflicts
return `request.failed` with affected paths, without advancing review or applying
any files; Textual uses its existing full-width Slop error notice. The worktree is
kept across interruptions and removed at `implementation.close`, while archives
remain independent of live files. See [isolated implementation](git-snapshots.md#isolated-implementation).

After successful import and persisted review, `implementation.validate` sends its
correlated `implementation.updated` confirmation before queuing preparation of
the next step. Textual selects the successor before rendering that state, showing
its known titles/substeps and `Preparing` without waiting for agent/turn setup.
While import is pending, Validate is disabled (`Validating...`); failures leave the
selection on the unvalidated step and allow retry. Preparation failures after
confirmation belong to the successor, not to the already imported step.

Preparation also covers agent/turn initialization, and ends when the model turn
starts. A preparation failure or cancellation publishes an interrupted run with an
error and Continue available; no model call executes before isolation is ready.
Cancellation may wait for an in-flight checkout operation, but prevents model
execution afterwards. Current state requires `checkout_pending` and the other
structural fields emitted by the writer.

State replies/events are `implementation.updated`, with `implementation` null before
execution or an object containing `plan`, `steps`, `pace`, `status`, `error`,
`work_step`, `work_substep`, `review_step`, `review_substep`, and `review_branch`.
Each step has `substeps`, `whole`, `validation`, `repository_root`, and
`repository_tree` (the original baseline tree ID, empty before capture). State
publication does not enumerate repository paths. Textual requests the root's direct
children and the direct children of final folders containing changed files. Other
folders are read only when explicitly opened; intermediate ancestors do not load
neighbors automatically. Loaded directory entries are reused for the same baseline.
The display compacts unbranched folder chains, preserving branches and full file
names. Final folders show five entries including every changed child or branch
leading to changes, exceeding five when required; `...` denotes hidden neighbors.
Ellipses are not clickable. When entries or path components are hidden, a
`Show all` / `Compact` toggle appears after the tree's last row, inside its scroll.
Show all removes the limits and path compaction and opens only changed-file
ancestors, requesting their direct children if not already cached. Other folders
stay closed and load only on explicit opening, never recursively. The choice
survives live updates of the same step and resets on step or baseline changes.
`implementation.directory` accepts `sessionId`, `step`, `tree`, and relative `path`
(empty for root). The reply has the same type, `requestId`, `sessionId`, `tree`,
`path`, and `entries`: objects with `name` (one component) and `directory` (boolean).
The daemon validates the baseline against the selected step in the live or a persisted
revision archive, including steps removed from the live plan. Reads use nonrecursive Git tree queries,
not the live checkout, and are allowed in frozen archives. A missing baseline,
unsafe path or unreadable directory returns `request.failed`; preserved changes
remain visible. Clients discard late replies for another baseline. An error does
not trigger background retries; reopening the folder explicitly retries it.
Items carry `complete`, `reviewed`,
`explanation`, and `diffs` (`path`, `patch`, `before`, `after` historical text). Binary files carry empty text contents. Snapshot internals are not transferred. `work_substep` remains zero for whole-step execution; `review_substep` reports the first unchecked indicator of the next unfinalized step.
Statuses are `running`, `waiting`, `completed`, or `interrupted`. Completed describes
construction, not final review. `info.implementation_execution` distinguishes this
workspace from the planning chat.

Substeps additionally carry `started`, `announced`, `unnecessary`, and `activity` (ordinary message
objects with relative `elapsed_ms` timestamps). The agent's
`report_implementation_progress` tool announces `started` / `completed` / `unnecessary` with a
zero-based `substep` and a concise result `explanation` for completed/unnecessary work. These reports capture file boundaries
synchronously, before the next edit, but do not finalize an item or authorize human validation.
For new runs, ownership comes from these boundaries, not final hunk assignments. Even adjacent
contributions merged into one final hunk keep their original owners.

After all planned substeps are announced completed or unnecessary, `status: "validating"`
with `substep: null` starts the technical validation phase. It normally only runs the
planned tests/build/checks. Before any repair, `status: "correcting"` declares its original
substep target; `completed` closes that repair. Explicit file-edit tools refuse edits without
an active construction/repair target. Unattributed captured changes prevent completion.
All check and repair activity goes to the step's `validation.activity`, not the target
substep's construction activity. `validation.started` and `validation.complete` distinguish
the technical phase from human review. In Textual, this appears last as **Validating the step**,
unnumbered, in MISC, with activity but no description. The human-review menu entry is
**Human validation**; its summary omits the global step description, which stays on the step.

`complete_implementation_step` supplies the global explanation, an ordered object for each
substep, and the implementation conclusion on the last step (empty on earlier steps).
`{}` or an empty `explanation` reuses the latest completed/unnecessary
explanation, including validation repairs; an explicit explanation replaces it.
Hunk assignments are rejected. The daemon builds corrected intermediate states from captured line
ownership, incorporating repairs into their declared targets without redistributing
unchanged contributions. Binary changes retain their exact snapshot evidence and
file-level ownership. Completion is refused if the final snapshot differs from the
submitted boundary. Successful completion ends the model turn directly, without another
provider call or prose reply. Later tool calls in the same response receive not-executed
errors; a rejected completion lets the model continue normally. The daemon then finalizes
snapshots, diffs and storage before human validation becomes available. A progress
announcement alone does not end execution. The conclusion is persisted with the step and
remains available in the archive. Cumulative diff inspection is optional,
not a completion prerequisite. Models may group progress and work calls in execution order
when the next action does not depend on an intervening result.
`unnecessary` requires a concrete explanation of why current code makes the work
no longer useful; the UI shows this immediately, struck out and non-selectable.
It remains provisional: a later started/completed report clears it, as does final
presence of actual changes in that substep.

Implementation storage version 7 uses an append-only boundary log in `implementation.sqlite`,
using an independent worker connection. Boundaries, ownership, validation progress and
activity survive cancellation/restart; resume keeps the original baseline, including
verified edits outside Git. All runs use captured ownership; hunk-assignment
completion submissions are rejected. Snapshot internals and boundary-log contents are not
transferred in `implementation.updated`.

Review acceptance is `implementation.review.accepted`. Review messages use the
existing chunked `btw.started` / `btw.chunk` / `btw.finished` transport, distinguished
by `branch.review: true`, and never appear in the principal conversation. Execution
still publishes ordinary agent activity and permission events. `execution.cancel`
interrupts construction; review remains separately cancellable.

State version 7 stores a `review_branch` per step and frozen construction contexts
in `implementation.sqlite`'s `review_construction` table, keyed by stable step
identity and execution generation. New completions freeze their context before
Everything can start a successor. Historical fallback reconstruction ignores
messages and compaction checkpoints beyond the next step's start boundary.
Review transfers include `branch.review_step`; Textual separates their histories
and rejects late transfers for another review cursor. Final validation waits for
an active review response, preventing a summary from racing its final answer.
Each new review context includes precise persisted summaries of preceding step
reviews, not their raw conversations. Missing summaries are generated, without
tools, on the review worker at the next step's first question using its selected
model/reasoning. They remain internal context: the ordinary Activity and cancel
action cover preparation and response, with no separate UI milestone. A failed
or cancelled preparation never activates an incomplete summary. Completed
summaries are reused after restart. Summary failures fail the review turn rather
than silently dropping context.
The source is the original review exchanges (including tool evidence), never
its compaction checkpoints or construction-selection system records. Large
sources are reduced in bounded chronological chunks, then recursively merged
with the selected model in a tool-free synthesis context. Non-converging,
empty, cancelled, or tool-producing summaries fail explicitly.
Review preparation checks construction + summaries + the pending question
against the selected model budget before injection. If needed, construction
compacts first; generation then continues with the injected summaries. Ordinary
review tool loops use the same automatic compaction threshold as principal
execution. Phase/inspection instructions are restored after compaction.
Compaction retains the latest exact harness phase announcement, including when
it precedes the retained user turn. Restoration appends that phase after the
generic Sane/Slop reminder; execution authorization therefore survives without
letting historical execution override a newer read-only review or planning phase.
This is preserved through repeated compactions and checkpoint restoration.
Execution instructions require technical validation to finish in-scope work and
submit `complete_implementation_step` before ending normally. A prose audit or
promise to continue is not step completion. Genuine blockers, denied actions and
cancellation remain interruptions, not permission to fabricate completion.
Review journals preserve original exchanges and a separate `active_context`
with `context_message_count`, keyed by `context_construction_key` (step identity
and construction generation). Resuming continues from that active context;
reconstruction after a correction invalidates it, not the original history.
Accepting a revision incorporates only the current review segment into the build
journal as historical JSON text, with the accepted request. It never replays tool
calls, approvals or the review agent's read-only role. The transfer is idempotent;
`review_segment_start` persists the first exchange not yet incorporated (default 0
for older journals). Acceptance clears the active review context and cached summary.
The next review starts from the corrected step construction and its new question,
not a second copy of the incorporated exchanges. Summaries for subsequent steps
use only this non-incorporated segment. Original exchanges remain visible, followed
by a `review-integrated-` system marker rendered as a visual segment separator.
Textual focuses Accept when a proposal becomes actionable. Left/right select
Accept/Decline instead of diffs, including from an empty review input. Accepting
closes review immediately and selects the reviewed step with Preparing while the
isolated checkout is prepared asynchronously. Failed requests restore the review
or revised-plan page. Step views expose `construction_history` segments containing
previous intentions, explanations and Activities. Textual uses a single previous/next
selector above Implementation plan in the left sidebar, labeled `Implementation #N`.
It switches the entire plan, step menu, Activities, validation and snapshot diffs,
without a separate Previous results dialog or per-step version controls. The initial
step shows its global summary; a corrected step shows the accepted request that
introduced it (`correction_request`, empty for an unchanged initial step, in both
live and archived views). Acceptance immediately selects the new version
with Preparing. Historical browsing is read-only, never restarts execution, and never
changes the version being validated. Live updates preserve the historical selection;
a new accepted correction selects the latest version. Historical results and final
diff ownership remain distinct.
In review transfers and implementation Activities, system records whose IDs
start with `compaction-status-` carry `started`, `failed`, or `cancelled` as the
first text line. The completed record is the ordinary assistant `compaction`
checkpoint. These display-only status records never enter the model context.
Textual renders these as a red expandable Compaction line without a border,
with a spinner only while running, splitting the preceding and following
Activities. This includes technical validation and a review's initial reduction.
Each supported implementation stores a separate review journal per step;
shared journals from previous implementation formats are not imported or split.
Review file tools read a temporary private materialization of the completed
step's preserved tree, prepared only on their first use and removed at turn end.
Original source and execution paths resolve into this historical tree; external
links/reads cannot escape to the live workspace. This is separate from Changes'
lazy directory protocol and never changes the construction checkout.

## MCP management

`/mcps` uses the attached conversation's configuration workspace, or the configured
integration workspace without a conversation, not a path supplied in the management
request. No provider authentication or conversation is required.
Requests carry the usual `requestId`. These additive commands use protocol 7 and
require a rebuilt daemon; an older daemon does not implement them.

- `mcp.list`: list effective server configurations, states, tool counts and issues.
- `mcp.save`: `name`, `scope` (`workspace` or `global`), `config` using the schema in
  `mcp.md`, and `identity` when editing. A new name must not already be effective.
- `mcp.trust`: `name`, `identity`, `accepted` boolean; separate from saving/enabling.
- `mcp.enable`: `name`, `identity`, `enabled` boolean. Controls model tool availability
  independently of connection lifetime; running calls are not cancelled.
- `mcp.connect`, `mcp.disconnect`, `mcp.remove`: `name` and reviewed `identity`.
- `mcp.token`: `name`, `identity`, `token`; HTTP token authentication only. This
  replaces credentials, not trust. Secrets never appear in management responses.
- `mcp.secrets`: `name`, `identity`, `environment` string map; replaces the local
  server's complete secret environment without returning it or modifying config.
- `mcp.oauth_start`: `name`, `identity`; approved HTTP `auto` or `oauth` configuration only.
  Returns `mcp.oauth_started` with `url` and `instructions`. One active flow per
  integration; no tools/conversation/provider required.
- `mcp.oauth_poll`: returns `mcp.oauth_pending` or saves completed credentials after
  identity/trust validation and returns `mcp.listed` with a `notice`. Failures return
   `request.failed`, without token/code values. Client polls while showing authorization.
- `mcp.oauth_client`: `name`, `identity`, `client_secret`, `method`
   (`client_secret_basic` or `client_secret_post`); approved HTTP `auto`/`oauth` with configured
  client ID only. Replaces stored client credentials/tokens, never shareable config.
- `mcp.oauth_revoke`: `name`, `identity`; disconnect and revoke tokens using the
  persisted RFC 7009 endpoint. Keeps configuration approval and client registration.
  An absent endpoint or remote failure is reported, not treated as revocation.
- `mcp.catalogue`: `name`, `identity`; connected approved server only. Returns
  `mcp.catalogued` with `name`, `identity`, `entries` (kind, name, description, target,
  arguments). Kinds are `resource`, `template`, `prompt`.
- `mcp.preview`: `name`, `identity`, `kind`, `target`, optional `arguments` string map
  for prompts or fully expanded `uri` for templates. Revalidates catalogue membership,
  returns `mcp.previewed` with `name`, `identity`, `text`, `images` (mime/data), `addable`.
  Unsupported binary/audio previews are not addable. No conversation is modified.
- `mcp.cancel`: cancel connection work or authorization; discard and drain the flow. Responds
  with `mcp.cancelled`; saved configuration changes are not rolled back.

Except for the explicitly listed response types, successful operations return
`mcp.listed` with `workspace`, `servers` and `issues`.
Each server includes `name`, `scope`, `config`, `identity`, `trusted`, `state`,
`error` and `tool_count`. No tool results or credentials are returned. Errors return
`request.failed`; callers can refresh to retrieve the latest connection state.
An authentication challenge during `mcp.connect` instead returns `mcp.listed` with
the updated `needs_auth` state and guidance, without starting OAuth in the daemon. In the explicit TUI Connect journey, the
client continues with token entry or OAuth and connects after authorization.
Configuration save/remove, resources/prompts, secret-environment management and
advanced OAuth operations remain daemon APIs but are not exposed in the TUI.
Server configuration is file-based; the TUI only controls connection and enablement.
The wire `config` keeps explicit `type`, command arrays and `environment` for
existing UI consumers; disk files use `mcpServers`, string `command`, `args`, `env`.
`mcp.save` accepts either entry syntax, defaulting to enabled and HTTP `auto`.
Workers perform filesystem/network operations away from Asio, and integration
closure cancels pending work. State is shared with agent tools through the daemon's
workspace connection registry. Management is a direct user action, not a model tool.

Connect/Disconnect and Enable/Disable persist global choices by server name in
`$XDG_CONFIG_HOME/sos/mcp-preferences.json`, not in a workspace config or session.
They affect all loaded workspaces in the daemon, while every configuration retains
its own trust and credentials. `config.enabled` reports effective global enablement
when overridden. `integration.configure` restores wanted connections on a worker;
failures remain visible in `mcp.list`, without interactive OAuth or automatic retries.
An idle Textual MCP dialog polls the list to reflect other integrations' changes.
Listing itself performs no connection attempt. Disconnect survives daemon restart;
shutdown does not change preferences, and a failed restoration keeps the wanted state.

Daemon-side constants live in `src/daemon/config.hpp`: `PROTOCOL_VERSION`,
`DEFAULT_SOCKET_PATH`, `DEFAULT_MODEL`, `DEFAULT_REASONING_EFFORT`, `MAX_FRAME_BYTES`
and `MAX_SESSION_TITLE_WORDS`. `protocolVersion`
versions this JSONL application protocol, not the underlying Unix socket transport.
Version 4 adds per-item lazy details to the paged display projection: activity
groups contain lightweight rows, not full tool payloads. It uses the bounded
transfer introduced in version 2. Clients must reject other protocol versions at the
handshake; update the daemon and TUI together. Existing session files are unchanged.

Protocol version 5 introduced image input and its fail-closed preflight. The current
session schema 6 includes a dedicated, content-addressed image table. Earlier
internal session schemas are not supported.

Protocol version 6 introduced persistent read-only BTW side conversations. The current schema includes
the `btw` table, with an immutable frozen snapshot and separate mutable branch
history. Principal messages, history projections and compaction are unchanged.
Rebuild the daemon and TUI together.

Version 7 permits new BTW branches in a nonempty idle principal conversation.
Branch metadata includes required `standalone` and `pageAnchor` fields in its JSON
body. Missing placement fields are rejected rather than inferred from `anchor`.
Update daemon and TUI together.

AI Check and exact manual replay extend version 7 with optional metadata.
The model configuration contract is advertised by `info.permission_check_version: 1`;
Textual requires it before editing AI Check to avoid silent partial saves with an
older daemon. Older clients may omit model/threshold fields and retain saved values.
The wire field `confidence` is retained, but new checks use allow probability.

Undo/redo adds `session.undo` and `session.redo` to protocol 7 and upgrades SQLite
to schema 6 (`undo_turns`). Rebuild the daemon and Textual integration together;
older daemons reject these requests and cannot open schema 6.

## Undo and redo

`{"type":"session.undo","requestId":"undo-1"}` and
`{"type":"session.redo","requestId":"redo-1"}` operate on the attached session.
They require an idle principal and no active BTW branch. They never invoke a
provider or rerun a tool. Unknown legacy turn states and file conflicts return
`request.failed` without changing active history; preflight conflicts also leave
all files unchanged.

Success reuses the bounded `session.attach_started` / `session.history_chunk` /
`session.attached` transfer with `purpose: "navigation"`. The first record is the
original user message of the navigated turn, including full image payloads. It is
a draft record, **not part of display history**. Remaining records form the latest
page of the newly active history. `messageCount` includes this draft record; page
anchors refer only to actual history records. Clients must wait for the complete
transfer before replacing history. Undo restores the draft; redo clears the input.
Navigation transfer failure closes the connection rather than leaving a usable
stale client view after files/history have already changed.

The redo tail is persistent until the next accepted user message. Failed or
cancelled executions may be undone too. See [Git snapshots](git-snapshots.md) for
Git versus editing-tool coverage, conflict policy, pending-navigation recovery
and limits after abrupt crashes. Current model, reasoning and context-limit
preferences are not rolled back.

## SOS skills

The catalog merges workspace `.sos/skills/` and user `$XDG_CONFIG_HOME/sos/skills/`
(default `~/.config/sos/skills/`). Workspace folders shadow user names. Each
metadata entry includes `source: "workspace" | "user"`. No Claude/agents directories
are scanned.

Runtime `info.skills` advertises support and provides a list of
`{name, description, arguments, source, scripts, references, assets}`. `arguments`
is an ordered array of positional alias names, not a schema. Optional
`argument-hint` is literal display text shown after the skill name, never sent as
input. Optional `argument-descriptions` is an SOS string-to-string object for the
numbered preview list, ordered by `arguments`, then description-only labels.
It documents input without binding or validation. There are no required flags
or `argument_order`. This replaces
the former SOS mapping format and named invocation objects.

Entries may also carry `license` and `compatibility` strings and a `metadata`
string-to-string object. Optional descriptive fields appear in display and model
catalogs and in the loaded skill's `Skill attributes (JSON data)` section.
Missing fields are omitted; they do not affect execution or permissions.
Unknown skill frontmatter fields are ignored without diagnostics or catalog
exposure; they never activate hooks, agents, model overrides, or allowed tools.
Resource categories list sorted skill-relative paths recursively, without reading
or executing contents (at most 1000 files per category, no symlinks). A failure
produces `<category>_error` instead of that list; other categories remain visible.
Empty categories are empty arrays. Resource lists are display-only.
`info.skill_errors` contains discovery diagnostics. These fields refresh while
idle and remain stable during execution. Clients must not offer skill commands
without an advertised catalog.

`message.send` accepts an optional invocation with `name` and optional raw `input`:

```json
{"type":"message.send","requestId":"skill-1","text":"/check-api src/http","skill":{"name":"check-api","input":"src/http"}}
```

Omitted input defaults to empty; serialized calls include it explicitly. Non-string
input, unknown fields (including the removed `arguments` object), or an unknown
skill fail validation. The daemon loads the skill before title generation or
acceptance. Missing or extra argument values do not fail. Textual does no binding
or quote parsing. The loader expands `$ARGUMENTS`, `$N`, `$ARGUMENTS[N]`, and declared
`$name` aliases; only indexed/named placeholders tokenize input using shell-style
grouping without execution. Malformed grouping fails before generation. See
[skill substitutions](skills.md#positional-and-raw-substitutions).
Input is preserved in the rewritten provider text, the synthetic call, and the
real loader result, where it is recorded as `Input (JSON string data)`.

For a leading chain, `message.send` accepts `skills: [<invocation>, ...]` instead
of `skill`, with one to six entries. Both fields together, an empty array, or more
than six entries are invalid. Textual sends the same final raw input to each.
The daemon loads all from one catalog snapshot before acceptance and appends
ordered, distinct call/result pairs. A loading failure rejects the entire request
before title or model generation. Each call retains independent `user_skill`
provenance, even for repeated skill names.
The original user text stays in the persisted message's `content` for display.
Optional `provider_text` replaces it only in model input with
`Call the skill "<name>" with input (JSON string data):` and the raw JSON string.
Images are preserved. This also applies on replay, context rebuild, and compaction.
Old persisted calls/results remain readable without reloading against the new format.

Before generation, the daemon appends a synthetic assistant `skill` tool call
and a matching tool result containing the exact loaded content. These are ordinary
persisted call/result messages, not another execution of the tool. The model sees
the rewritten user request followed by an already-completed call. No additional
skill system message is inserted. Both routes use the same loader; neither
executes scripts or grants permissions. Replay never reopens the skill file.

The assistant call carries display-only `user_skill: "<name>"`, also present in
its live `harness.message_arrived` event message. It contributes one `skill`
Activity item with the `Triggered by user command` label and no model-call usage.
Its history row is a normal `kind: "tool"` row with `user_skill`; lazy details
return the original arguments and result. Provenance metadata is not sent to
providers. System messages are hidden, not projected as skill activities. The
current synthetic tool-call sequence retains its `user_skill` display provenance.

Project instructions and catalog snapshots are ordinary persisted system
messages. Compaction retains the latest exact message per instruction path and
the latest catalog. No SQLite schema change is needed. See [skills](skills.md).

## BTW conversations

`btw.send` takes `sessionId`, `requestId`, `text` and an optional `branchId`.
Without a branch ID it creates `btw-1`, `btw-2`, etc., unique within the session,
whether the principal agent is executing or idle. An empty principal conversation
cannot start a BTW. With an existing ID it continues that branch even when the
principal is idle. An already running branch rejects
another message rather than overlapping its own turns. BTW input is text-only;
images already in the principal snapshot are inherited.

Each new branch freezes the principal agent's entire active context, including
its latest user request and completed activity. Outstanding principal tool calls
are balanced with explicit unknown/pending results, never executed by the branch.
Separate system instructions explain the informative role and frozen context.
Only branches created during execution include the notice that the latest
principal request was being processed. This distinction is frozen with the branch.
Subsequent BTW turns use that same snapshot plus their own history. They never
append to the principal conversation. Each exchange uses the principal conversation's
provider/model/reasoning selection; portable replay avoids copying provider-bound
reasoning into a different model context.

The branch registry exposes only `read`, `glob`, `grep`, `view_image`, `webfetch`.
A runtime whitelist also rejects invented write/command calls. Reads outside the
integration's authorized workspace are denied without opening a competing
permission dialog. Each branch has independent cancellation/retry state.
`execution.cancel` only affects the principal. Disconnect closes all branches,
drains their events and saves the interruption. Crash-left `running` branches
are shown as interrupted on reload; they are never restarted automatically.

Accepted sends emit `btw.accepted` with the request ID and allocated `branchId`.
`btw.list` streams all saved branches for the attached session; `btw.history`
streams one branch. Both finish with `btw.listed`. Live updates use the same
bounded transfer: `btw.started` carries `sessionId`, a transfer `requestId`,
`session: {id}`, branch metadata and `messageCount`; `btw.chunk` uses the existing
history message/part/last/data fields; `btw.finished` validates the final count.
Only displayable user/assistant texts are transferred, not the frozen snapshot
or image pixels. Tool results, reasoning and provider metadata remain persisted
for continued generation. `btw.retry` carries the branch ID and the normal retry
counter/countdown. No BTW event is a terminal event for the principal.

For a branch created during execution, `standalone` is false and `anchor` is the
principal user message ID. Its panel is nested before Activity. For an idle branch,
`standalone` is true and `anchor` is the last principal display record at creation,
which may be an Activity or compaction. Its panel follows that record, with its
arrow aligned to ordinary messages. `pageAnchor` is a navigable message ID used
to load the page containing `anchor`; the two need not be identical.
Accepting a BTW opens that panel and folds the others; sending
a new principal message folds all panels. Continuing an off-page branch returns
to its anchor. Pagination can discard widgets but not saved branch data. Resume
restores branches collapsed. Branch context is not automatically compacted in this
version; provider context-limit errors are reported in the branch, not the main chat.

## Images

`model.list` entries include `image_input`: `supported`, `unsupported`, or
`unknown`. OpenRouter and Codex use declared input modalities. OpenAI API uses
exact documented IDs; unlisted IDs remain unknown, not guessed by family.

Clients may optionally check capabilities separately with
`{"type":"message.check","requestId":"check-1","has_images":true}`.
The daemon returns `message.checked`, or `request.failed` with `code: "image_input"`.
The check includes active-context images. `info.has_stored_images` tells clients
to check even when a new draft is text-only. While a turn runs, a queued draft's
check is conservative; check again immediately before dispatch. `message.send`
independently repeats the check before title generation or appending the user
message, so clients cannot bypass it and model changes cannot use a stale result.
An unavailable catalog is not an unknown image capability: network and temporary
service failures retain their retryable error type. The image preflight retries
the catalog with the same ten-retry schedule and emits `execution.retry` even
before a session exists. Successful catalogs that explicitly exclude images are
unsupported; successful catalogs without a declaration remain unknown. Both still
fail closed with `image_input`. Malformed catalogs and authentication failures are
request errors, not image-capability results. Exhaustion before acceptance returns
`request.failed` with `code: "catalog_unavailable"`; the message has not been sent,
so the TUI restores its draft and attachments instead of claiming it was kept in
the session. Cancellation and disconnect also interrupt catalog backoff.
The TUI instead sends `message.send` directly after mounting an optimistic local
turn and starting its Activity spinner. On rejection it removes that provisional
turn and restores text and attachments alongside any new draft, with the error.
This visual feedback does not change daemon acceptance or persistence semantics.

`message.send` accepts an optional `images` array alongside `text` (which may be
empty for an image-only message). Each image contains `mime`, canonical base64
`data`, `name`, and `origin`. The daemon derives the SHA-256 `img_…` ID and dimensions
from the bytes, not client assertions. PNG, JPEG and WebP are accepted, with
header/dimension validation. Limits: 8 images per message, 5 MiB per image,
10 MiB of decoded pixels in total, 16,384 pixels per side and 40 megapixels.
These are local resource limits, not a promise of every provider's limits.

Images are real multimodal input parts, not base64 appended to the prompt text.
User messages and `view_image` tool-result messages carry an `images` array.
SQLite stores pixels once per session by content; message references and pixels
commit together. History pages and live display notifications contain references
and metadata without `data`. Reopening a session hydrates only its active context.

Compaction does not send old image pixels to the summarizer. Retained recent
messages keep pixels; summarized images keep IDs and metadata, and the original
pixels remain in SQLite. `view_image({"image_id":"img_…"})` retrieves pixels only
from the current session and reintroduces them into the active context. A tool's
text result is followed by a user-role multimodal input item for Responses-provider
compatibility. Image context cost uses a dimension-based local estimate rather
than treating base64 as text; it is not a billing formula.

Unknown and unsupported capabilities both block image input. Runtime checks also
cover tool images and `view_image`. These failures are shown by the TUI as a
non-focusable SLOP notice, never as an error instruction sent to the model.

## Integration lifecycle

The Codex provider receives Responses SSE internally. SSE framing is decoded
separately from `ResponsesStreamAssembler`, which orders completed output items
by `output_index`, reconciles final copies by item ID, and preserves final response
metadata. A final `response.completed` with an empty `output` does not discard
previously completed items. Failed/truncated streams and missing items are rejected;
integrations still receive complete provider messages rather than streaming deltas.

Each connected integration counts towards the daemon's lifetime, including one
without a session. On normal exit it sends `{"type":"integration.close"}` and
closes its socket; no acknowledgement is required. Socket EOF also closes the
integration if it could not send this message.

The daemon initiates a health check every 15 seconds:

```json
{"type":"integration.ping","pingId":"1"}
```

The integration must respond within 30 seconds with the same ID:

```json
{"type":"integration.pong","pingId":"1"}
```

Only the matching pong acknowledges the check. Other requests or stale pongs do
not extend its deadline. Pings share the normal serialized socket writer; their
deadline begins when they reach that writer, not while queued behind title work.
Pongs and explicit closes are handled directly by the reader, even while request
processing awaits a title or model metadata. The TUI answers pings in its network
receive loop and keeps that loop running while an external editor is open.

A missed pong closes that integration and cancels its active execution. The
existing session-draining path persists remaining messages before releasing it.
When the last integration has finished closing, the daemon stops accepting,
removes its socket and exits. At startup it allows 30 seconds for a first
connection. These durations live in `daemon/config.hpp`.

The next TUI launch starts a fresh daemon as needed; saved sessions remain
available. The TUI does not initiate heartbeat checks or reconnect automatically
during an established session.

## Requests

### Connect a provider

Provider authentication and selection are separate from chat messages. The Unix
socket is owner-only (`0600`). These requests remain available during execution.

| Request | Fields | Response |
| --- | --- | --- |
| `provider.list` | `requestId` | `provider.listed`, with `providers`: IDs, connection availability, `auth_method`, `model`, `name`, `custom`, and custom `base_url`; no secrets |
| `provider.create` | `requestId`, `name`, `base_url`, optional `key` | `provider.listed`, with updated `providers` and `created_provider` |
| `provider.delete` | `requestId`, custom `provider` ID | `provider.listed`, with updated `providers` |
| `auth.api` | `requestId`, `provider` (`openai` or `openrouter`), `key` | `auth.saved` |
| `auth.start` | `requestId`, `provider: openai`, `method: browser` or `device` | `auth.started`, with `authId`, `url`, `instructions` |
| `auth.poll` | `requestId`, `authId` | `auth.pending` or `auth.saved` |
| `auth.cancel` | `requestId`, optional `authId` | `auth.cancelled` |
| `provider.select` | `requestId`, `provider`, optional `model` | `provider.selected`, with updated `info` |
| `model.list` | `requestId` | `model.listed`, with `models: [{provider, model, auth_method, max_context}]` and `errors: [{provider, error}]` |
| `model.select` | `requestId`, `provider`, non-empty `model`, optional integer `context_limit` | `model.selected`, with updated `info` |
| `model.cancel` | `requestId` | `model.cancelled` |

The browser flow uses PKCE/state and a loopback callback on port 1455; the device
flow polls OpenAI while the integration continues answering heartbeats. OAuth
flows expire after ten minutes and are cancelled on disconnection. Errors use
`request.failed` without raw authentication response bodies. `auth.api` checks the
candidate key with OpenRouter `GET /api/v1/key` or OpenAI `GET /v1/models` before
writing credentials. Validation runs on a cancellable worker, without inference.
Failure/cancellation preserves the previous key. OpenRouter management keys are
rejected. Authentication success does not establish generation quota or access to
every model. Tokens are refreshed before expiry.

Credentials live in `$XDG_DATA_HOME/sos/auth.json` (default
`~/.local/share/sos/auth.json`), owner-only, atomically replaced and outside session
journals. This is plaintext storage, not encryption. Credentials are read only
from this file; API key environment variables are ignored. The non-secret selection
lives in `$XDG_CONFIG_HOME/sos/config.json` (default `~/.config/sos/config.json`).

Each provider has exactly one credential entry. Reconnecting replaces the entire
entry (including obsolete key/token fields), without removing other providers.
Its required `method` is `api`, `browser`, or `device` and survives OAuth refresh.
Missing or generic methods are rejected without modifying the file; reconnect
through `/connect`. `provider.list.auth_method` is null when disconnected, otherwise it reflects
that stored method; it is independent of the currently selected provider.

`provider.select` cancels any current principal execution, drains and saves its
events, then detaches the session and resets the mode to Sane. It creates no session
until the next message. Failure leaves the current conversation attached, though
its execution may already have been cancelled. `info.provider` and session
metadata explicitly identify the provider.

An omitted or empty `model` selects the first compatible model after authentication,
with no random draw. OpenRouter requests `sort=newest`; OpenAI API models are sorted
by descending `created`, with missing/invalid dates last and stable ties. Codex
models are sorted by ascending `priority`, following the official client, with
missing/invalid priorities last and stable ties. This is a provider ranking, not
a release-date ordering; synthetic fast variants remain beside their base model.
The last successfully connected/selected provider and the selected model are persisted; other
providers remain authenticated. OpenRouter uses text
modalities and `tools` support, Codex uses visible models from its dedicated catalog,
and OpenAI API uses known Responses/tool-compatible ID families because `/v1/models`
does not expose capabilities. Discovery runs off the Asio thread and is cancellable
through `auth.cancel` or disconnect. Errors do not commit a provider/model change.
Codex discovery uses compatibility version `0.155.0`; the obsolete `0.99.0` request
can return only hidden internal models, despite successful OAuth authentication.

### Custom providers

`provider.create` configures a named OpenAI-compatible Chat Completions instance.
The HTTP(S) `base_url` includes the API prefix (usually `/v1`); URLs containing
credentials, query strings, fragments, or a full generation/catalog endpoint are
rejected. The optional key uses Bearer authentication; absent keys send no
Authorization header. The daemon discovers `GET {base_url}/models` on a bounded,
cancellable worker before saving anything. The response must contain a `data`
array with at least one valid model ID. Failure, `auth.cancel`, and disconnection
before completion do not save the candidate. Catalog discovery is not an inference
or a verification of tool/image support.

Creation generates an immutable `custom-…` instance ID, saves `name` and `base_url`
under `config.json.custom_providers`, and stores a nonempty key under that ID only
in `auth.json`. Names must be unique among the configured providers. Creation
returns the list with the new ID and does not change the selected provider or
conversation. Lists keep built-ins first, then custom instances sorted by name.
For custom instances, `connected` means configured, not a live network connection;
`auth_method: api` describes the supported connection method, even without a key.

Selecting an instance uses the first discovered model unless an explicit model is
supplied. `/model` includes custom catalogs after built-ins, with `provider_name`
for their display name. Model IDs are not filtered by OpenAI family names and are
deduplicated within each instance. Context and image capabilities remain unknown;
the existing context-budget input provides an explicit effective limit.

Custom generation sends classic `messages`, `model`, optional `tools`, and
`stream: false` to `POST {base_url}/chat/completions`, without Responses-specific
or reasoning parameters. The runtime adapter converts portable Responses items
and normalized completions internally, so the existing tool loop, journals, titles,
compaction, and provider switching share the existing conversation representation.
Opaque Responses reasoning is not sent to custom endpoints. Truncated or filtered
completions are rejected before partial text/tool calls enter the conversation.

`provider.delete` removes only a custom definition and its credentials. It refuses
the currently selected instance: select another provider first. It never deletes
session journals or reuses deleted instance IDs. A missing instance cannot be
created as a fallback built-in provider. Textual shows Connect/Delete actions with
Connect selected by default; creating an instance is the last Custom provider row.

### Model picker

`model.list` concatenates models from authenticated built-in providers and configured
custom instances: OpenAI first, then OpenRouter, then custom instances, preserving
each catalog's order. IDs are deduplicated
within a provider, never across providers. It uses the same authentication and
compatibility filters as `/connect`. Each row includes the stored `auth_method`:
`api`, `browser`, or `device`. Stored credentials require an explicit method;
incomplete or generic `oauth` records fail with a reconnect instruction, without
rewriting secrets. The TUI displays Api key for `api`
and Codex for OAuth methods, alongside the capitalized provider in the right column,
with labels aligned on their first letter. A failed catalog appears in `errors`; successful
catalogs remain usable. No credentials or raw HTTP error bodies are included.
Loading runs on a cancellable worker; `model.cancel` or disconnect interrupts it.
Listing and cancelling do not change the saved selection.

The TUI `/model` dialog searches these rows locally by literal, case-insensitive
substring in provider/model text. Selecting sends the exact provider/model pair.
Before sending, the TUI opens a context-budget input, prefilled with the advertised
maximum (or the saved budget when editing the current model). Unknown maxima leave
the input blank. `context_limit` must be at least 32768 and cannot exceed the
nullable `max_context` from the latest `model.list` on this connection. Supplying a
budget requires that listing first. Codex advertises `max_context_window`, falling
back to `context_window`; OpenRouter uses `context_length`.
The budget is saved with the conversation alongside its model and reasoning. Explicit
changes also update the global default for new conversations, without changing other
conversations. Omitting it clears the override for legacy clients. Changing reasoning
preserves it; `/new` uses the global default and `/resume` restores the saved budget.
Escape returns to the model list
without saving. The chosen budget drives display, compaction input budget and
recent-turn retention; the existing safety reserve still applies.
`model.select` requires stored credentials for that provider and a valid model ID,
and persists the global selection without changing the reasoning preference.
It keeps the current conversation and mode. During execution the current provider
request keeps its model/reasoning; the next provider call in the same turn applies
the selected model, reasoning and context budget, rebuilding portable context.
If there is no next call, the next user turn uses the selection. Global `info.model`
changes immediately while `info.execution_model` identifies the in-flight model.
Historical messages retain their original metadata. Model commands do not require
an idle session. The TUI waits for acknowledgement before closing the picker.

### Configure permissions and mode

Before using a saved session, the integration identifies its launch workspace:

```json
{"type":"integration.configure","requestId":"configure","workspace":"/absolute/launch-directory","mode":"sane"}
```

Response: `integration.configured`, with `mode` and canonical `workspace`.
The optional `terminalSessionId` string (maximum 256 bytes) is an opaque terminal
identity resolved by the client, never from the shared daemon's environment.
Omit it when no reliable identity is available. It is grouping metadata, not an
authorization credential. Older clients may omit it.
This root is immutable for the connection. A first `message.send` may establish
it from its workspace if no explicit configuration was supplied. Attaching a
saved session requires prior configuration and does not authorize that session's
workspace implicitly.

```json
{"type":"mode.set","requestId":"mode-1","mode":"slop"}
```

Response: `mode.updated`, with `mode`. This changes only the connection's runtime
permission policy, not the conversation or the persisted `session.mode`.
`session.mode` records the mode used at the last user-message launch. `session.attach`
restores that saved mode into the permission policy and the TUI; `mode` is required
in stored session metadata. Workspace grants and remembered approvals remain
connection-local and are not restored with it. `message.send` also accepts an optional
`mode`, applied before running the message; otherwise it uses the runtime mode.
The initial system prompt explains both modes and announces the starting mode.
On subsequent user messages, a different mode appends a system announcement just
before the user message and updates `session.mode`. Identical modes and switches
without a message append nothing. Existing messages and tool definitions are not
rewritten. Loading older sessions preserves their original prompts unchanged.
No mode announcement is injected between tool iterations of an active turn.
Runtime mode changes still apply to subsequent permission checks immediately;
switching to Sane dismisses a pending forbidden mutation.
Already performed effects are not rolled back. `/new` restores Sane while keeping
the connection's root and remembered approvals.

### Permission policy selection

This policy is independent of the `sane` / `slop` execution mode:

```json
{"type":"permissions.get","requestId":"permissions-1"}
{"type":"permissions.current","requestId":"permissions-1","mode":"ask","has_key":false,"confidence":80}
{"type":"permissions.select","requestId":"permissions-2","mode":"auto","key":"<OpenRouter API key>","confidence":80}
{"type":"permissions.selected","requestId":"permissions-2","mode":"auto","confidence":80}
```

`mode` remains `ask`, `auto` or `danger` on the wire; Textual displays **AI Check**
without the old `(auto)` suffix. This policy is independent of Sane/Slop.
Selection applies to subsequent permission checks, dismissing and reevaluating a
pending Ask prompt without cancelling the turn. An already running tool is not undone.

Runtime `info.permission_check_version: 1` advertises configurable decision models
and allow-probability thresholds. Textual refuses AI Check configuration without
this capability, even if an older daemon advertises `permission_confidence`.
`info.permission_model` and `permissions.current.model_config` contain `protocol`,
`endpoint`, `model` and `has_key`. The key itself is never returned.

```json
{"type":"permissions.select","requestId":"save-check","mode":"auto","confidence":95,"check_version":1,"model_config":{"protocol":"openai_decisions","endpoint":"https://api.openai.com/v1/decisions","model":"gpt-6-luna"},"key":"<dedicated API key>"}
```

`model_config` in a selection contains exactly `protocol`, `endpoint`, and `model`.
It requires `check_version: 1`; do not send the read-only `has_key` field back.
`protocol` is `openai_decisions` or `typesafe`. Endpoints are complete URLs;
HTTPS is required except HTTP on loopback. URL credentials and fragments are rejected.
Model IDs are configurable, not a hard-coded catalog. TypeSafe's official endpoint
is `https://api.typesafe.ai/v1/systemone`. Each service must support its selected
protocol and model. New explicit configurations validate key syntax locally;
they do not make a paid decision request on Save. Legacy clients supplying only
an OpenRouter key retain their existing OpenRouter validation.

The historical `permission_verifier` credential maps only to protocol `typesafe`,
endpoint `https://openrouter.ai/api/alpha/decisions`, model `typesafe/jev-1.13`.
It is not migrated to either provider's official endpoint. Non-secret configuration
is stored in config.json; dedicated keys stay in auth.json with mode 0600.
Credentials are immutable revisions bound to protocol and endpoint; config.json
activates a revision only after its successful atomic write. A failed settings
write may leave an unreferenced credential revision but never replaces the active
key. An omitted/empty key can reuse the active credential only for the same
protocol and endpoint. Changing just the model can retain that key. Changing the
protocol or endpoint requires a key when the threshold is nonzero.

In Textual, highlighting AI Check shows the model and probability threshold.
`Save` applies the complete draft and closes only on successful acknowledgement.
`Change model` opens an API picker, then URL/model/masked-key fields. `Use model`
returns a draft to the parent without a daemon request. Escape in the fields
returns to the API picker and clears the key; Escape there cancels the child.
Closing the parent discards its unsaved model, key and threshold. Save errors keep
the draft editable. Ask before and Danger keep their direct selection behavior.

`confidence` is retained as a compatibility field name for the integer threshold
from 0 through 100, persisted as `permission_confidence` (default 80). Its meaning
is now **minimum estimated probability of allow**, not the API's `confidence`.
Existing saved numeric values are preserved, but their meaning changes; they are
not calibrated equivalences between models. For thresholds 1–100, an action passes
when `probabilities.allow >= threshold / 100`. No complement of `deny` confidence
or rounding is used. At 0, the verifier is not called, as in Danger, subject to
Sane/OS restrictions. At 100 the supplied allow probability must be exactly 1.
These are model estimates, not safety guarantees.

OpenAI requests use named `choice` questions in an array and structured `input`;
TypeSafe uses `state` and named questions with `criteria`. Both ask the same
allow/deny risk question using the same filtered context. Answers must contain
exactly the expected question, coherent choices and finite allow/deny probabilities
in [0,1] whose sum is within 1e-6 of 1. Invalid, refused or missing answers and
transport failures cannot authorize an action. Transport retains TLS verification,
no redirects, bounded responses, timeouts and cancellation.

The global selection is persisted outside sessions and read at each user turn;
other already-running integrations finish with their current policy. Changing
policy clears the connection's remembered grants. `ask` retains the existing
permission protocol below. `auto` evaluates actions that would prompt using
a configured decision API, with instructions to allow routine repository work and
refuse concrete harmful risks. Its allow probability is compared with the selected
threshold. A score below threshold, oversized input and service errors produce a
tool error, never `permission.requested` or a human `no` decision. The action is
not executed, but the turn is not cancelled: the model receives guidance to
continue without it, or ask the user to manually approve it in Activity and send
a new message. Retrying or bypassing the refusal without manual approval is prohibited.
This is model guidance, not a forced end of execution. Cancellation aborts the
verification. `danger` bypasses prompts,
but both policies still enforce Sane prohibitions before any approval.

Automatic decisions emit `permission.resolved` directly, without a preceding
human prompt, with `decision: "ai_accepted"` or `"ai_refused"`. These decisions
include `confidence` when available. For newly evaluated actions this compatibility
field contains the estimated probability of allow in [0,1], not the API confidence.
Historical snapshots retain their original recorded scores without reinterpretation.
It is absent on unavailability, never replaced with a fabricated zero. The same
recorded value is persisted and displayed.
These decisions
are persisted in tool-result `metadata.permissions` and retained in lightweight
history summaries. They are not valid client `permission.reply` values.
An automatic refusal still produces a normal tool error, not a cancelled run.

### Manual approvals

```json
{"type":"permission.manual","requestId":"manual-1","sessionId":"session-id","refusedMessageId":"tool-result-id","approved":true}
{"type":"permission.manual_updated","requestId":"manual-1","sessionId":"session-id","states":{"tool-result-id":true}}
```

The server validates the attached session and retrieves the exact action from a
recorded AI refusal, never from client-supplied tool arguments. Unknown targets,
non-refusals and legacy refusals without an authorization target fail with
`request.failed`. A successful reply follows durable journal append; changing a
approval never runs a tool. Identical repeated settings are no-ops. Matching identical
actions shares the latest manual state even if they have multiple refused call IDs.

Refused tool results retain immutable `metadata.refused_action` (tool, arguments,
working_directory, workspace) and `refused_message_id`. Manual changes are separate
system audit records with `manual_approval: {refused_message_id, approved}` and
empty content. They are excluded from active model context. At the next user turn,
a system report describes changes since the preceding user message; the policy
receives the session's current grants. Exact matches may run repeatedly in Ask/Auto,
never overriding Sane. Successful manual checks record `manually_approved` on the
new attempt; this is not a valid `permission.reply` value. Revocation removes the
manual grant on the next user turn. `/new` cannot inherit it; `/resume` restores it.

The system report also supplies `available_approvals`: opaque session-scoped IDs
and the exact actions authorized by the user. Active grants are reannounced at
each user turn so compaction or legacy reports cannot hide their IDs. The runtime
tool `rerun_approved_action({"id":"<approval-id>"})` accepts only that ID, never
overridden arguments. The daemon retrieves the stored tool and all arguments,
requires the same working directory and permission workspace, and checks that
the ID was available at the start of the turn. It rechecks persisted revocation
before every permission boundary. It bypasses Jev but not Sane, cancellation,
tool validation or OS restrictions. Grants are reusable until revoked, and no
approval click runs anything. Identical arguments do not guarantee identical
effects if files or scripts have changed. BTW agents do not expose this tool.

The underlying provider tool call remains `rerun_approved_action`. Its result
has `metadata.approved_action: {id, tool, arguments}` for display, including
failure/cancellation after resolution, and records manual permission decisions.
A manual `permission.resolved` preflight identifies the underlying tool for
the live UI. History projections return its actual `toolName` and exact arguments
on detail requests, without rewriting the provider transcript.

Activity results carry `refused_message_id` and a separately derived
`manually_approved` display flag, including lightweight rows. The TUI displays a
mutable `Refused tools` section above a Muted separator and leaves the lower
historical transcript untouched. Rows are Slop before approval, Algae after it;
their titles are `bash • Confidence 2%`, followed by ` -> Manually approved`
after approval. Both title and suffix turn green; the score stays historical.
Missing scores render `Confidence unavailable`, including old refusals.
an `Approve` / `Revoke approval` button exists only while the row is expanded.
Activity summaries include `refused` in `kinds` whenever a historical AI refusal
is present, regardless of its current approval state. This drives a persistent
Slop-colored marker without fetching the detailed actions.
Full request details are loaded lazily as usual.

### Permission requests

The daemon emits a request tied to an announced tool call:

```json
{"type":"permission.requested","permissionId":"1","callId":"call-1","tool":"write","permission":"edit","patterns":["file.txt"],"always":["*"],"details":{"filepath":"/workspace/file.txt","diff":"..."}}
```

`details` are tool-specific. For `external_directory`, they include `filepath`
and `parentDir`; for bash they include `command` and `workdir`. Runtime calls also
include the original `arguments` and default `working_directory`. The card is rendered
in the chat, outside Activity. The worker waits without blocking the Asio loop.

```json
{"type":"permission.reply","requestId":"reply-1","permissionId":"1","decision":"yes"}
```

Decisions are `yes`, `no` and `always`. The direct acknowledgement is
`permission.reply_accepted`. Applying the decision emits `permission.resolved`
with the same permission/call IDs and `decision`; the TUI then removes the card
and annotates the tool. Cancel, disconnect, or a newly forbidden mode emits
`permission.dismissed` while connected. Late/duplicate replies are rejected.

`no` cancels the entire execution, records the refused tool as not run, skips
remaining calls, and follows the normal `execution.cancelled` path. Automatic
Sane denials return tool errors without asking. Decisions are persisted as
`metadata.permissions` on tool results, with `id`, `permission`, `patterns` and
`decision`. Display metadata remain excluded from model input.

Remembered `always` approvals live only in the connection's `PermissionManager`: external
directory and descendants, the whole editing family (`write`, `edit`, `apply_patch`),
or the displayed bash scopes. The launch workspace and its descendants already
have directory access without a prompt; edit and bash approvals remain separate.
Sane restrictions override them. A once-approved external directory is reused
only within that same tool call. Approval state is not restored from conversation
history, unlike the explicit manual session grants above. Bash analysis uses Tree-sitter and conservative rules, not an OS sandbox.

### Conversation recap

`{"type":"session.recap","requestId":"recap-1"}` requires an attached, idle
session. It creates neither a user message nor an agent turn. The daemon emits
`recap.started`, followed by `recap.completed` with one saved message in `messages`,
or `recap.cancelled` / `request.failed`. Recap events include `requestId`,
`sessionId` and runtime `info`. `execution.cancel` and disconnect cancel the
worker. The same request exclusion rules as manual compaction apply.

The isolated, tool-free provider call uses the selected model and reasoning.
Its input is at most 24,000 UTF-8 bytes of recent user/assistant text from active
context, including a previous compaction checkpoint when available, but excluding
raw tool payloads, reasoning and previous recaps. The prompt requests a single
paragraph of at most two short sentences and 45 words in the user's language,
written for a human returning to the work: what we are doing, what it enables and
what they can try or what comes next. Routine test/build reports and internal
details are omitted unless they are the main task or an actionable blocker.

The saved message has `role: "assistant"`, ordinary text content and `recap: true`.
This flag denotes a display-only history entry, **not model input**. It is preserved
in SQLite bodies and summaries and in the paginated display projection. Each recap
has a distinct ID and its own chronological position; later recaps do not replace
earlier ones. Clients render it as a Conversation Recap card, not an assistant
response or Activity, and keep the flag when caching live records.

Recaps are excluded when reconstructing agent context, converting provider input
and preparing compaction. They do not carry generation metadata or alter the last
turn's context counters. A successful event is sent only after persistence; failure,
cancellation and disconnect leave no recap entry.

The TUI may issue this same request after five continuous idle minutes while it
is open, connected and attached to a session. This is a client-local monotonic
timer, never a daemon timer or a calculation from saved timestamps. Sending a
message or running work resets the idle interval; session changes and a fresh
TUI also start a fresh interval. Modal dialogs, history loading and the external
editor defer it. Each idle period allows only one attempt, including failure or
cancellation, and a manual recap also satisfies that period. New user messages
or work rearm the timer, but automatic requests additionally require
`info.last_history_is_recap` to be false. This runtime field is derived from the
last record of the persisted display index, not from the currently loaded page
or the raw journal tail. Invisible system events do not clear it. It survives
TUI/daemon restarts and session resume, preventing a second automatic recap when
the history still ends with a recap. Explicit `session.recap` requests remain
allowed in that situation: this additional guard applies only to the TUI's idle
trigger. The server still enforces the attached/idle preconditions.

### Compact a session

`{"type":"session.compact","requestId":"compact-1"}` requires an attached, idle
session. It does not create a user chat message or start a normal agent turn.
The daemon sends `compaction.started`, then `compaction.completed` with `messages`
containing one assistant summary, or `compaction.cancelled` / `request.failed`.
While summarizing, `compaction.progress` reports a one-based `attempt` and a `limit`
of 3 before each summary model call. Each chunk or merge starts at attempt 1;
only size corrections advance it to 2 or 3. Events carry `sessionId` and the
manual `requestId`, or `automatic: true` for automatic compaction. Textual shows
only `Compaction` and the spinner on attempt 1, adding `2 / 3` or `3 / 3` for
corrections. This transient counter is not saved in history and does not count
provider-internal transport retries.
The model is instructed to use at most 1% of the effective input window with a
2,000-token budget floor, less whenever possible, with only essential continuation
state. The floor permits a larger budget, not a minimum summary length. Internally,
summaries up to 5% are accepted with the same floor. Both budgets are capped by the
model output limit, estimated at four UTF-8
bytes per token. This tolerance is not disclosed in the prompt. Ordinary compaction
uses up to 75% of the estimated input window per chunk; oversized transcripts
still use bounded chunks and merging. After two unsuccessful size corrections,
the shortest candidate is truncated at a UTF-8 boundary with a warning.
Responses carry `requestId` and runtime `info`; compaction events carry `sessionId`.
`execution.cancel` and connection closure cancel the summarization worker.
Other state-changing requests received during compaction are rejected, except
`mode.set`; changing the runtime mode still does not announce a new session mode.

The summary message's `compaction` object has `retained_message_ids` (ordered IDs
from earlier history) and `mode` (the last session mode). Its ordinary `metadata`
contains the summarizer's model, reasoning and duration. The full history is not
deleted: `session.attach` returns all original messages, all checkpoints and later
messages, in chronological order. Clients render a checkpoint as an inspectable
compaction summary, not as a normal answer belonging to the preceding user turn.

Model context is rebuilt as initial system instructions, a synthetic user message
(`The earlier conversation was compacted.\nSummarize what we did so far.`), the
assistant summary, retained recent messages and a system reminder of the session
mode, followed by later messages. The synthetic user/reminder are not additional
chat history entries. Retained display messages keep their IDs and metadata;
provider-dependent reasoning/item metadata is stripped from their wire replay.
On success context usage resets to unknown until the next normal generation.
Manual compaction never automatically continues the task. Existing sessions
without compaction checkpoints remain compatible.

Manual/automatic compaction and step-review summaries share the same bounded
synthesis mechanism. Large transcripts are split into blocks and intermediate
summaries are merged recursively. Ordinary compaction allows two size corrections
after the initial call; step-review summaries allow two reductions and an emergency
summary. Both keep the shortest candidate and stop as soon as it fits. As a
last resort the shortest candidate is truncated at a UTF-8 boundary with an
explicit incomplete-context warning. Manual/automatic compaction internally accepts
`max(2000, effective context / 20)` tokens and requests a ceiling of
`max(2000, effective context / 100)` tokens, both capped by the model output limit.
The prompt does not disclose the internal tolerance and encourages shorter summaries.
Local size enforcement estimates four UTF-8 bytes per token; this is not exact
tokenization. Step-review summaries retain their separate byte budget (at most
8000 bytes and one eighth of the effective context limit).
Provider errors, empty answers and cancellation still fail or cancel synthesis;
they are not hidden by fallback.
Original chat history is preserved throughout this process.

### Automatic compaction

Before each discussion model call, the daemon checks the estimated active context.
It uses the last provider input + output usage at its recorded message boundary,
plus an estimate of subsequently added messages (UTF-8 JSON bytes / 4, rounded up).
Without a measured boundary, including after resume or compaction, it estimates the
whole active conversation and tool definitions. This is a heuristic, not tokenization.

The threshold is the model's maximum input budget, or its total context if no
separate input limit is known, minus `max(10% of that budget, 16384)`. For a shared
context window, the reserve also covers the known output limit. Unknown/zero limits
or a budget no larger than the reserve disable the automatic check. OpenRouter
supplies context and output limits; OpenAI currently leaves these limits unknown.

Automatic compaction emits `compaction.started`, then `compaction.completed`,
`compaction.failed`, or `compaction.cancelled`. These notifications carry
`automatic: true`, `sessionId`, `messages`, `error`, and runtime `info`, without a
compaction `requestId`. A successful completion includes the saved assistant summary.
They are not terminal execution events: clients remain busy until the subsequent
`execution.completed`, `execution.failed`, or `execution.cancelled` notification.

The current user turn, including all tool calls and results, is kept intact even
if it exceeds the usual retention budget of `min(25% of maximum context, 30000)`.
After persisting the checkpoint, the worker resumes generation with this context;
no synthetic continuation request is appended. Failure, cancellation, or an
estimated context still above the threshold stops execution without activating a
partial checkpoint. A request with no earlier history to summarize fails instead
of dropping the current turn. There is no retry loop for provider context errors.

### Send a message

Create a session on the first user message; the workspace must be an existing
absolute directory. The default model is `openai/gpt-5.6-luna` and the default
reasoning effort comes from `DEFAULT_REASONING_EFFORT` (`xhigh`), explicitly sent
to the provider when no global preference exists. Provider, model, reasoning and context
budget are saved per conversation. The last explicit selection is also persisted in
`$XDG_CONFIG_HOME/sos/config.json` as the default for new conversations.
An explicit `model` or `reasoning` in `message.send` changes the conversation's selection
and that global default; otherwise every turn retains the conversation's selection.

```json
{"type":"message.send","requestId":"1","workspace":"/absolute/project","model":"openai/gpt-5.6-luna","reasoning":"medium","text":"Explain this project"}
```

Response: `session.created`, with a `session` object containing the identifier,
title, workspace, model, reasoning effort, context fields and creation/update
timestamps in milliseconds since epoch.
Before creating the session, the daemon makes one dedicated LLM call using the
session's model and the first user message to generate its title. It requests
2–4 words, enforces a maximum of 6 words, and normalizes the result to one line.
This call has no tools and is not added to the discussion history.
`session.created` always includes the final title, already persisted with the
session; the discussion starts only after title generation. Other connections
remain responsive during this wait. Title generation failure returns
`request.failed` without creating a session, allowing the client to retry.
Disconnecting during title generation requests HTTP cancellation and does not
create a session. Subsequent turns and reattachments preserve the title.
Credentials are never persisted in the session journal.

Once attached, send another message with `message.send` and `text` only. The reply
is `message.accepted`. New messages during execution are rejected rather than queued.
An integration displays one session at a time; it may also retain that session's
Discussion / Implement pair. `session.attach` cancels and drains an active
principal before switching it.

After reattachment, the first send may emit the additive status event
`execution.preparing` with `requestId`, `sessionId`, and runtime `info`, before
`message.accepted`. This is not acceptance of the draft: the daemon is restoring
the active model context on a cancellable worker. Cancellation emits
`execution.cancelled` without appending the new user message; failure emits
`request.failed`. The TUI shows `Preparing conversation…` and retains its normal
pending-draft restoration behavior. Older clients may ignore this status event.

List metadata without loading agents:

```json
{"type":"session.list","requestId":"2"}
```

Response: `session.listed`, with `sessions`, ordered by `updatedAt` descending.
Each item may include `terminalLastOpenedAt` (Unix milliseconds) for the terminal
configured on this connection. It is absent when that terminal has never opened
the conversation. Listing and previewing do not count as openings. Successful
creation and attachment refresh the opening time without changing `updatedAt`.
Each conversation persists up to 100 distinct terminal identities with their
latest opening times in its SQLite metadata; the least recently opened is evicted.
New conversations start with no terminal history; the persisted metadata requires
the terminal history field. The full history is not included in the listing.

The Textual picker places matching conversations first, ordered by
`terminalLastOpenedAt`, under `Invoked from this terminal session`. Other
conversations retain global activity order after a separator, without duplicates.
The heading and separator are not selectable. Without matches, neither appears.

Preview the last user request without loading an agent or attaching:

```json
{"type":"session.preview","requestId":"preview-1","sessionId":"32-character-hex-id"}
```

Response: `session.previewed`, with `sessionId` and `text` (empty if there is no
user message). This reads persisted history without modifying it, ignoring an
incomplete trailing record. Clients correlate replies to their selected session
so a late preview cannot replace the current selection's text.
The last user message is located through a disposable persistent index; preview
may create or repair that index, but never truncates the authoritative journal.

Attach to an existing session, cancelling and saving an active principal if needed:

A Discussion with an active Implement branch resumes directly into that branch.
The transfer's `sessionId` then differs from the requested Discussion ID, and
`session.discussion_id` identifies its parent. Clients validate that relationship
before accepting the transfer. Branches are omitted from the ordinary session list.

For the idle attached Discussion, `implementation.open` takes `sessionId`,
`requestId` and optional `objectiveId`. It accepts that persisted Objective Plan
version and creates an independent branch; omission selects the latest version.
An unknown or foreign objective ID is rejected. The branch pins the selected
plan and stack explicitly in its provider context, even when later versions
exist in the inherited conversation.
`implementation.view` takes the same fields and optional `branchId`: empty selects
Discussion; a saved branch ID selects its active conversation or frozen archive.
Both use the ordinary bounded history transfer. Implement pages omit inherited
Discussion records without removing them from the provider context.
After construction starts, those pages stop at the first execution snapshot's
history boundary: the accepted plan stays the last visible message, including
in frozen archives. Step execution remains in the construction workspace and in
the provider context, not in the plan conversation.

`implementation.close` takes the parent Discussion `sessionId` and `requestId`,
while the active implementation branch is attached and idle. It requires all
construction and final step validations to be complete. It freezes the branch,
clears the Discussion's active implementation and transfers Discussion history
using the ordinary bounded transfer. Textual then removes its single `Implement`
tab; the frozen branch remains available in the implementation history. That tab
first shows plan discussion, then construction. Its `Implementation plan` action
above the steps shows the accepted plan in the diff area without leaving the steps
menu. Changes is hidden in this view and on Conclusion; selecting a step restores it.
During plan discussion, the current plan is only in the right-hand panel, including
after history restoration. `Implement` below that panel accepts it; the chat control
remains `Sane` or `Cancel`. Only accepted plan versions appear in chat history,
styled in Sane green, with an `Implementation` link for the accepted implementation plan.
The accepted Objective Plan links to its branch through `Implementation`. During
construction (including pauses between steps), Textual keeps Discussion read-only
without generation indicators. A non-editable composer asks the user to finish
the current implementation. Returning to Discussion preserves its loaded page
and scroll position; it does not detach or cancel construction.
After all steps are built and validated, selecting Discussion sends
`implementation.close` directly and restores its editable composer. Returning from
a frozen archive uses `implementation.view` without a branch ID instead of the
locked preview, unless another implementation is still active.

Runtime `info` includes `discussion_id`, `implementation_view`,
`implementation_archive`, `implementation_plan_ready`, `active_implementation_id`, and `implementation_branches`
(entries with `id`, `objective_id`, `frozen`). Viewing Discussion keeps the branch
active; its next `message.send` freezes that branch before execution. Archives reject
messages, BTW sends and session mutations. Implement is fixed to Sane; `mode.set`
cannot change it to Slop. See [implementation conversations](implementation-branches.md).

In Implementation views, `info.implementation_plan` contains the most recent
finalized plan message, or null before the first plan. Unlike readiness, this
display state remains available during subsequent discussion. Textual displays
it in a right pane beside chat, with a Sane-colored separator. This split is
specific to implementation planning; Discussion uses inline Objective Plan drawers.
It does not change plan generation or acceptance semantics.

While construction is running, implementation state optionally includes `active_duration_ms`,
the elapsed duration of the item selected by `work_step`/`work_substep`, including resumed
execution. Clients anchor their live timer on receipt, independently of view mounting.
This transient value is not persisted or included in historical versions.

Implementation state items optionally include `duration_ms` and `model_statistics`
(the `model_call` metadata shape: model, reasoning, duration_ms, optional output_tokens).
Durations accumulate execution time across interruptions/resumes, excluding human
validation waits. Step totals include substeps and technical validation. Model
statistics accumulate each response once; throughput is total output tokens over
total model-call seconds, not an average of individual rates. A missing token count
makes throughput unavailable; older items may have no statistics. Completed step
and substep titles show duration/throughput on their next line instead of progress.

Clicking a construction list row inspects it (and may expand a completed step),
without cancelling construction or validating a substep. Explicit Enter actions
and their confirmations remain available. The review input remains editable during
generation; another submission waits for idle, and accepting the first request
does not clear text typed subsequently. Metadata-only review messages update the
existing Activity rather than creating a new trailing drawer after the answer.

In Discussion, runtime `info.objective_plans` lists persisted Objective Plan
messages in version order, including `id`, text `content` and optional
`objective_stack`. Implementation views expose the parent Discussion's versions
for read-only previews. `objective_plan_ready`
means a version exists after the last objective-completion boundary, independently
of subsequent ordinary chat. Closing an implementation atomically clears its
active branch ID and journals a system message with `objective_completed: true`.
Its text tells the model that the old objective is done. The marker is retained
in lightweight summaries and stops objective readiness and turn inheritance,
without deleting prior versions, drawers or implementation links.
All Sane Discussion turns expose `submit_objective_plan` for a full replacement
and `patch_objective_plan` for exact, unique replacements in `plan` or `stack`.
Patch arguments are `updates: [{field, old_str, new_str}]`; replacements are
applied in order, atomically. Empty, unchanged, ambiguous or invalid plans are
rejected without creating a version. Each successful call is recorded immediately,
not deferred until successful completion of the model turn. Previous versions
remain immutable and survive resume. These tools do not edit workspace files.

Textual shows Discussion chat at full width. Runtime `info.objective_plan_turns`
contains `{message_id, objective_id, updated}` for each completed Sane turn that has
an Objective Plan, anchored to its last assistant text message. It is derived from
lightweight journal summaries, including after resume, without loading tool bodies.
The active turn is omitted until execution ends. A collapsed `Objective plan`
drawer appears below that message, never below intermediate replies or before the
first plan. Publishing one or more versions during the turn labels its drawer
`Objective plan · updated`; other turns retain the prior version without this label.
The drawer displays that turn's immutable final version and stack. Its centered
`Implementation plan` and `Implement (Auto plan)` actions use that historical
`objective_id`, or open its existing branch. Actions are hidden in read-only
Discussion previews. Opening a drawer or clicking its controls does not take
composer focus; browsing never changes the model's current plan.
Switching to Slop keeps historical Sane drawers visible and preserves their
expansion state. Slop turns never inherit a drawer. Returning to Sane carries the
last active plan onto new turns, even without an update. Once its implementation
is closed, that objective remains historical only; later turns have no drawer
until a new objective is published. Earlier drawers and their Implementation
links remain available.
The Discussion mode control remains `Sane` while idle and `Cancel` while busy;
it no longer triggers Objective Plan generation.

After the initial transfer, Textual immediately requests the plan in that branch:

```json
{"type":"message.send","requestId":"plan-1","text":"Implementation plan request","mode":"sane","implementation_request":true}
```

This request requires the exact synthetic text, an active Implement session, Sane
mode, and no images, skill invocation or `sane_request` flag. It preserves the input
draft in Textual. Ordinary branch sends remain ordinary discussion; they do not
expose the submission tool. A completed planning turn emits a final assistant
message with `implementation_plan: {"steps": [...]}` and canonical Markdown text.
Each step has `title`, `objective`, ordered `substeps` and `validation` strings;
each substep has `title`, `intention`, `changes`. Invalid or empty structures are
rejected. Failed or cancelled turns do not finalize their candidates. Live/history
records preserve the structure and the synthetic user's `implementation_request`
metadata. `implementation_plan_ready` reflects the last finalized plan after the
latest planning request, not the existence of any older plan in the journal.
The provider context and usage continue across the tab transition; compaction
remains available throughout Implement, without exposing inherited display records.

```json
{"type":"session.attach","requestId":"3","sessionId":"32-character-hex-id"}
```

Response: an ordered history transfer, all events carrying the original
`requestId` (empty when omitted) and `sessionId`:

```json
{"type":"session.attach_started","requestId":"3","sessionId":"32-character-hex-id","session":{"id":"32-character-hex-id"},"messageCount":1,"purpose":"page","groupId":"","page":{"first":"m1","last":"m1","hasOlder":false,"hasNewer":false}}
{"type":"session.history_chunk","requestId":"3","sessionId":"32-character-hex-id","messageIndex":0,"partIndex":0,"last":true,"data":"{\"id\":\"m1\",\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Hello\"}]}"}
{"type":"session.attached","requestId":"3","sessionId":"32-character-hex-id","messageCount":1}
```

The example abbreviates `session`; the start event includes complete session
metadata. Start and completion events include runtime `info`; chunks omit it.
Every history-transfer frame is at most 256 KiB, including the serialized JSON
envelope, string escaping, UTF-8 bytes and final newline. Pings may occur between
frames and must still be answered. Writes are awaited one at a time.

`data` is a fragment of one display record's serialized JSON, not a partial message
object. Fragments preserve UTF-8 boundaries. Concatenate their decoded strings
and parse only when `last` is true. `messageIndex` starts at zero and increments
after each complete message; `partIndex` starts at zero for each message and
increments for every fragment. Even a single oversized tool result is fragmented.
The sender serializes one message at a time, not the full history.

Clients must validate correlation, indices and the final message count. An empty
history has a start and completion event with count zero and no chunks. Only
`session.attached` marks the transfer complete; a truncated transfer must not be
treated as a restored session. The TUI keeps the old chat visible during reception,
shows progress, then replaces its one display window before enabling input.
Transfer failures close the connection so a stale client view cannot be used with
a partially switched session. The TUI discards partial transfer state on disconnect.

Attaching does not construct or start the agent. Unknown sessions and sessions attached to
another integration are rejected. Switching during execution cancels the principal
and saves its remaining events before the history transfer. Reattaching the current
session also stops an active principal and returns its history; switching to another
session saves the old one and loads the target before releasing the old reference,
so a load failure leaves the current session attached, but does not restart a
cancelled execution. `requestId` is echoed in
request replies when provided.

### History window

`Session` owns an incremental in-memory `SessionHistoryView` index referencing the
append-only journal. Navigation belongs to each client, not the session. The journal
and model context are unchanged. Initial attachment selects the latest 20 user and
visible assistant messages combined. System messages are omitted; technical-only
assistant messages contribute activities, not messages. A mixed assistant message
counts once, even when its text blocks are separated by activity drawers. Compaction
checkpoints are separate display markers outside the message budget. Projection
records omit provider replay data.

The same framed transfer serves these requests on the currently attached session,
including during execution:

```json
{"type":"session.history","requestId":"h1","sessionId":"32-character-hex-id","direction":"before","anchor":"m1"}
{"type":"session.history","requestId":"h2","sessionId":"32-character-hex-id","direction":"after","anchor":"m50"}
{"type":"session.history","requestId":"h3","sessionId":"32-character-hex-id","direction":"latest","clean":true}
{"type":"session.activities","requestId":"h4","sessionId":"32-character-hex-id","groupId":"m1/activity/0"}
{"type":"session.activity_detail","requestId":"h5","sessionId":"32-character-hex-id","groupId":"m1/activity/0","activityIndex":0}
```

`before` uses the current window's `first`, `after` its `last`. Anchors are stable
message IDs. Each navigation moves the 20-message window by 10 messages, retaining
an overlap of 10 (more at the beginning/end of history). The response is a complete
window snapshot, not a delta; the TUI reuses retained widgets. `latest` needs no
anchor. `clean: true` is still accepted but no longer reserves an empty slot for a
future submission: the latest snapshot contains up to 20 existing messages. Invalid anchors,
groups or unattached session IDs return `request.failed` before a transfer starts.
While an Implement branch is attached, its parent Discussion ID is also accepted
by these three read-only history requests, including during generation. This does
not switch the active session, its runtime `info`, permissions, or event subscription.
Textual uses a separate paged, read-only Discussion view while generation continues.
Start frames give `page: {first, last, hasOlder, hasNewer, start, end, total}`. Empty
bounds are empty strings. `start` and exclusive `end` are zero-based display-record
offsets within the accessible history; `total` includes message fragments, grouped
Activities and compaction markers, not individual tool calls. Branch inheritance and
frozen Implement-plan bounds restrict all three numbers. `messageCount` counts
transferred records, not the 20 budgeted messages.

`direction: "at"` accepts a decimal display-record offset in `anchor`, for example
`{"type":"session.history","requestId":"seek","direction":"at","anchor":"120"}`.
It selects a bounded window containing that position without loading earlier bodies.
Offsets must be nonnegative and less than `total`. IDs remain the anchors for
`before` / `after`; these existing requests are unchanged. Older daemons without the
additive extent fields retain the client's local-window scrollbar.

Activity records use `role: "activity"`, `content: []`, a stable `id`, `count`,
`kinds` (`tools`, `read`, `reasoning`), `terminal` and optional generation `metadata`.
An optional `activities` array contains lightweight rows with a stable, zero-based
`activityIndex`. Tool rows contain `kind: "tool"`, `callId`, `toolName`, lightweight
`arguments`, and optional `result` status. File tools retain `filePath` for
read/edit/write and display-only `filePaths` for apply_patch (move destinations
replace their source in this list); no patch text or file content is included.
Grep and glob rows retain `pattern` so their title is available without loading details.
Bash rows retain `mainCommand`, a required short display-only label on new calls,
including approved replays. It has no effect on shell execution or permissions.
Textual displays it in muted gray between the status and any exit code, without
bold tool titles. Older summaries missing either label remain unlabeled until
their details are explicitly opened; Activity loading never reads their original
messages solely to recover these labels.
Mutation results may retain `filepath` and a `files` list containing only path/type
fields, never diffs. Textual projects a multifile patch as independent file drawers,
while the logical call, permission target and Activity count remain unchanged.
Older summaries without these fields reveal their paths when details are loaded.
This status has
`isError` and only the metadata needed for the title: exit/timedOut, cancelled/started,
and permission IDs/decisions. Successful read output, other outputs, diffs, display
metadata and attachments are absent. A failed read includes its error output for
the tooltip. Reasoning rows contain `kind: "reasoning"` without their text.
A tool call and its result count once, even across raw journal messages.
Bash skill-script rows and live tool-call display events carry optional `skill_script`
(the skill name) and `skill_script_path` (the path relative to that skill's
`scripts/` directory, including subdirectories). Textual displays
`<skill> script · <relative path> · Completed` and preserves the ordinary status
and permission suffixes, including ` -> Accepted by AI (Jev)` when applicable.
The relative path is present in lazy history rows without fetching full details;
older daemons without it display only `<skill> script · <status>`.
The daemon classifies simple literal invocations under
`.sos/skills/<name>/scripts/` or the resolved user SOS skills directory, directly
or through python/python3/bash/sh, and stores
`skill` in the Activity aggregate so collapsed groups are green immediately on resume.
This display classification never changes the actual tool, permissions or provider
input. Candidate commands up to 8192 bytes are retained in journal summaries.
Activity aggregates are written with the current classification; reading them
does not rebuild classifications from summaries or full message bodies.
Each visible reasoning block counts once. Groups follow the existing drawers
between text blocks and are indivisible.

Snapshots preload only the last activity group in the entire session, if that
group belongs to the requested window. Its row list is included whole, even above 50
activities, and may be empty or in progress. Older windows do not substitute their
own last group. All other groups carry summaries only. Opening an unloaded drawer
requests only its group; its response uses `purpose: "activities"`, `groupId`, and
one activity record. The bounded framing also applies to very large individual
groups. Explicitly opened groups remain cached while retained in the display
window, until the next user submission purges activity details. The journal and
model context are not modified by this display-cache policy.

Expanding an imported tool or reasoning row sends `session.activity_detail` for
that group and index. The response uses the same bounded transfer, with
`purpose: "activity-detail"`, `groupId` and `activityIndex` in its header. It contains
one activity record with exactly one complete item (arguments, result, metadata,
attachments, or reasoning text). Unknown groups/indices are rejected before the
transfer starts. The TUI validates both identifiers, caches details in the expanded
widget, and discards them on purge or eviction; late replies cannot restore purged
data. Read rows are not expandable and never request a successful read's contents.
This projection applies to imported history; ordinary live agent events remain unchanged.

The TUI separates `HistoryWindow` (records, bounds, in-flight requests and stale-reply
rejection) from `HistoryViewport` (widgets, lazy drawers and scroll). Scrolling past
the upper/lower boundary slides this bounded window by 10 messages. A retained
visible widget anchors the scroll position, so reversing slightly does not hit the
opposite loading boundary. Retained open drawers keep their loaded contents when
their summaries are unchanged. Failed requests preserve the current window.
The TUI locks user scrolling from request dispatch through the final positioned
paint, stopping any active scroll animation. Layout and anchor restoration are
batched before painting the replacement. Wheel/key events and boundary callbacks
timestamped before completion are discarded rather than replayed. Failure and
cancellation release the input lock as well.
On each user submission, all old activity drawers close, their content widgets and
cached payloads are released, and outstanding activity loads are cancelled and
drained. Headers, markers, timing metadata and stable group IDs remain; reopening
a purged drawer requests its contents again. Live drawers use the same group IDs
as journal projection, including groups between text parts. The new activity then
fills directly from live events. From the latest window, this is entirely local:
the message is sent without a history request, loading indicator or snapshot rebuild. Only a
submission from an older window invalidates pending loads and reconciles a latest
clean snapshot before sending; a failed refresh keeps the draft for retry.
Accepted live user/assistant messages join the same window,
evicting the oldest messages and associated widgets beyond 20, with no separate
batch or empty slot. Multiple text parts from one assistant message still count
once. Incoming live messages do not move a window displaying older history.

This bounds page counts and wire frames, not bytes per message, explicitly loaded
activity groups, or the agent's active context. The daemon retains lightweight
index summaries, not a second full copy of all historical bodies. Live events, lists and previews
keep their previous framing.

Attaching restores the session's model, reasoning and context budget without changing
the global default. The agent is prepared on first use with this saved selection.
Legacy sessions without a saved budget use the model's default. Historical generation metadata stays intact.
When model/provider changes, old reasoning and provider-specific item metadata are
excluded from the outgoing context, while text, tool calls and results remain.
The journal keeps its original messages; `provider_context_start` records the
boundary so later reloads retain this distinction.

### Return to an unattached state

```json
{"type":"session.new","requestId":"new-1"}
```

Response: `session.reset`, echoing `requestId`, with the current global model/reasoning and
unknown context in `info`. Any principal execution is cancelled and its final events
are drained before the current session is saved and detached, not
deleted. No new session or agent is created until the next `message.send`.
Also valid when already unattached. The TUI's `/new`
clears the view after acknowledgement and keeps the current workspace (also returned
as `workspace` in `session.reset`). It does not inherit the old conversation, create
a worktree, or return to the primary checkout. A saved session resumes in its own
last workspace; a missing directory fails explicitly without falling back elsewhere.

### Select the next reasoning level

```json
{"type":"reasoning.next","requestId":"thinking-1"}
```

Response: `reasoning.updated`, echoing `requestId`, with the selected level in
`info.reasoning`. The daemon asks the provider for the model's available efforts
and cycles upwards, returning to the smallest selectable effort after the maximum.
The order is `low`, `medium`, `high`, `xhigh`, `max`, filtered to the model's
supported values; `none` and `minimal` are excluded from this shortcut.

OpenRouter supplies `reasoning.supported_efforts` in its model catalog. An explicit
null accepts all gateway efforts; an absent field, an unknown model or an empty
selection returns `request.failed` without changing the selected level. The
metadata client caches successful catalog retrievals and runs off the Asio thread,
independently of generation requests.

This immediately saves the conversation's reasoning preference and the global default,
even without another message. Without a session, only the global default changes.
New sessions inherit this default; resumed sessions restore their own saved settings.
During execution, this shortcut applies to the next message only: unless an
explicit model selection is made, provider rounds of the current run retain its starting
effort. `info.execution_reasoning` reports this active effort, or null when idle;
`info.reasoning` reports the conversation's selection for the next run, or the global default without a session.
`info.execution_model` similarly identifies the current provider call's model, or null when
idle, while `info.model` reports the conversation's selection, or the global default without a session. Changing provider/model
through `/connect` preserves the chosen reasoning preference rather than resetting it.
Unlike this reasoning-only shortcut, an explicit `/model` selection can update
the next provider call of the current turn, including its selected reasoning level.

### Cancel execution

```json
{"type":"execution.cancel","requestId":"stop-1"}
```

Cancellation is signalled immediately for this integration, including while its
first message is waiting for title generation. The reply is
`execution.cancel_requested` if a running or pending execution was targeted, or
`execution.cancel_ignored` when idle. Both echo the cancel request's `requestId`.
Acknowledgement is not completion: clients wait for the terminal execution event
before sending another message. The TUI allows draft editing during this wait.
If completion wins the race, `execution.completed` remains valid.

The agent aborts provider transfers through libcurl's progress callback (including
model metadata requests), terminates shell process groups and uses the tools'
cancellation checkpoints. This is interruptible execution, not a forced thread
kill or rollback; local indivisible operations may finish before cancellation is
observed. Remote generation/billing may continue after the local HTTP abort.

Before `execution.cancelled`, the runtime records results for announced calls:
completed calls retain their output, interrupted calls carry an explicit partial
effects warning, and calls not started are marked unexecuted. Cancelled tool
metadata contain `cancelled: true` and `started: true|false`. A system message
records the interruption and guides the next run to inspect uncertain effects
rather than automatically repeat operations. The same session accepts the next
`message.send` after cancellation is complete.

Cancelling initial title generation creates no session. Its terminal event carries
the original message request ID and no session ID; a cancel acknowledgement can
arrive before or after that event. The TUI keeps the unsent draft for retry.

## Notifications

Each notification contains `type`, `sessionId`, `messages` (new messages only)
and `error` (empty unless applicable):

- `harness.message_arrived`: user/system messages or tool results;
- `provider.message_arrived`: complete provider turn, including tool calls;
- `execution.retry`: nonterminal provider retry status (not persisted);
- `compaction.started`, `compaction.completed`, `compaction.failed`, `compaction.cancelled`
  with `automatic: true`: nonterminal automatic compaction notifications;
- `execution.completed`, `execution.failed`, `execution.cancelled`: terminal events.

For an accepted agent turn, generation transport/timeouts, broken or truncated
Responses streams, malformed Chat Completions output, and service errors (HTTP
408, 429 or any 5xx) trigger at most ten retries of the current model call. API
failures without a recognized code are also retried, including failed/incomplete
Responses without an explicit permanent cause. Delays before retries are
`0, 2, 5, 10, 15, 15, 15, 15, 15, 15` seconds. Completed tools and messages are
never replayed locally. Explicit quota/billing, authentication, permission,
input/context/capability errors, output-token limits and content filters are not
retried, even when paired with a retryable HTTP status. Tool failures and local
storage errors are not retried either. This policy does not restart a lost TUI/daemon connection or retry title,
recap or compaction requests.
The runtime's initial model-info lookup also uses this retry policy, before image
validation, including for text-only turns requiring catalog-based context limits.

`execution.retry` carries `retry: {"attempt": 2, "limit": 10, "seconds": 2,
"reason": "Connection problem"}`. Each countdown second emits an update; zero
seconds means reconnecting. `reason` is a safe generic label, not an upstream
response body. Attempt zero clears the notice on recovery. Cancellation interrupts
both requests and backoff, using the existing `execution.cancelled` path.
Exhaustion emits `execution.failed` with `code: "retries_exhausted"` and a safe
final explanation. The already accepted user message stays in the session.
The TUI shows the temporary status in bold Slop red before Activity; the terminal
error appears after Activity and stops the spinner, without restoring the sent
message to the composer. These notices are live-only, like execution errors.

Roles and content types distinguish text, reasoning, tool calls and tool results;
the notification type only distinguishes the origin. There is no token streaming.
Provider requests ask for `reasoning.summary: "auto"` in addition to the selected
effort and encrypted reasoning. When a reasoning item supplies both a readable
summary and readable content, its display text contains `Summary` and `Reasoning`
sections. If only one is provided, only that text is displayed; opaque-only items
remain without display text. Original provider metadata is replayed unchanged.

Tool results include a `metadata` object containing the tool's display details
(for example `diff`/`filepath`, `display` for numbered file reads, or shell
`exit`/`timedOut`). These details are persisted with the result and forwarded to
integrations. They are not included in provider input: the model continues to
receive only the normal output and supported attachments. Older saved results
without `metadata` load with an empty object. Clients correlate calls and results
using `callId` and select their rendering from the call's `toolName`.

## Runtime information

### User message mode

Each new user message carries `metadata: {"mode": "sane"}` (or `"slop"`), captured
when the message is sent. This display metadata is persisted in `session.sqlite`,
forwarded to integrations and restored on load, but excluded from provider input.
The TUI uses it for each message's color band, independently of the current session
or runtime mode. Legacy messages with missing/null metadata keep the previous UI
fallback: the session mode on resume, or the submitted mode during a live turn.
Their historical mode is unknown and is not backfilled.

### Generation duration

The final assistant message of each turn has a `metadata` object:

```json
{"model":"openai/gpt-5.6-luna","reasoning":"high","duration_ms":128042}
```

The daemon starts a monotonic timer when handling `message.send`, before title
generation if needed. It passes that start time through the session to the runtime.
The runtime records the duration when it receives the final provider response
without tool calls. The duration covers the entire turn, including tool execution
and intermediate provider calls. Model and reasoning are the settings used for
that turn, rather than whatever might be selected later.

These message-level metadata are persisted in `session.sqlite`, restored on load,
and excluded from provider input. Older messages may have no metadata. They are
distinct from the display metadata inside individual tool-result content items.
Terminal execution events also carry `metadata` at their top level; successful
completion uses the same duration as the final message. Failure/cancellation
without a final response report the elapsed duration in the terminal event.

The TUI starts its own monotonic timer at send time for a live display and replaces
it with the daemon's duration when available. A disconnected or rejected request
without server timing freezes the local elapsed time.

New assistant and tool messages carry an optional top-level `elapsed_ms`: a
monotonic offset from this turn's start. It is saved in the journal and its
lightweight index, never in provider input. Each new activity group closes the
previous group's interval; history activity records expose that intermediate
interval as top-level `duration_ms`. Terminal groups keep using generation
`metadata.duration_ms` for the total turn duration. Legacy messages without
offsets keep their unknown intermediate timings instead of fabricating values.

Successful conversation-model responses also carry top-level `model_call`:

```json
{"model":"openai/gpt-5.6-luna","reasoning":"high","duration_ms":4000,"output_tokens":100}
```

This duration measures only the blocking provider generation request, including
transport latency. It excludes tools, permission verification (Jev), title
generation and compaction. `output_tokens` comes from provider `usage.output_tokens`,
not input/context tokens; missing or invalid usage stays absent.
The message and lightweight journal index (version 3) persist these values.

Activity records expose the aggregate under the same `model_call` key, and
`generation: {"model": "…", "reasoning": "…"}` retaining each activity's settings.
The TUI shows a right-aligned, muted `model | reasoning | total time` footer from send time,
below the current turn's content. New activities and messages appear above it,
while a queued next request stays below it. The footer captures the launch settings;
server execution settings may correct them, but changes to the next selection do not.
The final response replaces this live footer using its saved generation metadata,
without a duplicate. Rejected pending turns remove their preview footer.
The live total uses the user request's monotonic start, not an Activity start,
and continues through text responses, tools, waiting and history navigation.
On completion the saved `duration_ms` fixes the total, including model generation;
failure/cancellation uses terminal metadata, or freezes the local elapsed time
when no terminal duration is available. Reloading uses the saved duration.
Activity headers show only timing
and throughput. Legacy responses without generation metadata have no footer.
Each response is counted once in the activity awaiting it, before processing its
text/reasoning/tool parts. The TUI appends ` - X tok/s`, calculated as total
output tokens divided by total model-call seconds, not the activity's wall time
or an average of individual rates. The rate updates after completed responses.
Unknown usage in any counted response, overflow, or zero model duration hides the
entire suffix, including its separator. Legacy sessions retain their known model
settings but never receive estimated token counts. Reopening activity details does
not add the aggregate again.

### Model and context

Every daemon message (including `daemon.ready`, replies and notifications) includes
an `info` object. Before attachment, it describes the defaults with unknown context.
Once attached, it describes the session. Example values:

```json
{"model":"openai/gpt-5.6-luna","reasoning":"medium","last_turn_used_context":1250,"total_context":128000}
```

- `last_turn_used_context`: `usage.input_tokens + usage.output_tokens` from the
  latest discussion provider response, including intermediate tool-calling turns.
  Replaced on each response, never accumulated. Cached/reasoning detail counters
  are not added again. The separate title-generation call is excluded.
- `context_limit`: the conversation's saved user budget (global default without a session), or null without an override.
- `total_context`: the user budget when set, otherwise the provider's advertised
  maximum (Codex `max_context_window`, with `context_window` as fallback, or
  OpenRouter `context_length`), not its output-token limit.
- Both context fields are nullable. A response with missing/invalid usage clears
  the latest count. A transport failure preserves the previous count. Unknown
  model capabilities remain unset; failed catalog lookups do not prevent generation.

The runtime retrieves capabilities on its worker before generation and carries a
context snapshot on each agent event. The session persists these fields in
`session.sqlite` and restores them into the runtime when loaded. `info.json` is a readable cache of these metadata. The TUI receives them
in the next event and displays `used / total`: this is an indicator for the last
call, excluding any additions since that call, not an exact tokenizer count of the
current conversation. Initially the TUI displays a single `-` until values arrive.
Legacy session metadata without context fields loads as unknown; generation settings
come from the saved conversation, with no budget override if missing. Context indicators reset when the model/provider changes.
The automatic compaction estimate is separate from this last-call usage indicator;
the UI does not present an estimate as a provider measurement. No exact tokenizer
is currently used.

## Persistence and shutdown

Messages are appended by position in `session.sqlite`. A transaction commits the
pending messages and session metadata together, including compaction checkpoints
and their context boundaries. `info.json` is a readable, atomically replaced cache,
refreshed at creation, turn boundaries, settings changes and explicit saves, not
every tool event. A cache publication failure does not invalidate a database commit.
Resume always reads authoritative metadata from SQLite; listing uses the cache,
recreating it if missing or malformed.

SQLite is statically linked into the daemon. WAL mode and `synchronous=FULL` request
durable commits, subject to filesystem and hardware guarantees. Back up through
SQLite's backup API or close the daemon before copying the session directory;
never discard live `-wal`/`-shm` files or copy only the main file during writes.

Only a new database (schema 0) or current schema 6 is accepted. Unsupported schemas
and corrupt databases are rejected without replacing their messages. JSONL files
and old metadata are never imported or modified. `info.json` cannot seed a database.

The current schema transactionally projects pending tools, qualified AI refusals,
approval events and current exact-action approval states. Recovery and permission
lookups do not load message summaries or tool-output bodies. Invalid approval
targets are rejected, not granted. There is no historical-schema migration.

The display projection is persisted in the message transaction.
Pages use indexed anchors and record ranges, without loading all message summaries.
Collapsed Activities read precomputed counts, kinds, durations and model usage;
late results update their original group. Opening a current database does not
reconstruct old aggregates or load bodies to fill absent skill names. The rendering and paging
protocol remain unchanged. Page selections are stable positions, not snapshots;
group contents may reflect newer results when subsequently read.

Individual body reads no longer parse entire JSONL batches. Large individual bodies
and expanded Activity lists (including the preloaded last group) are not yet
byte-budgeted. Details read source-message summaries by position, not per part.
Unresolved tool calls receive explicit error results, never automatic re-execution.

Active context restoration runs separately on a cancellable worker, before the
first send or manual compaction. The last compaction checkpoint references the
retained message IDs, so discarded bodies need not be decoded or replayed. Without
a checkpoint all model context is still restored, but merely browsing a session
does not pay that cost. Interrupted tools are never automatically executed.

On disconnect the daemon requests cancellation and retains the session until the
worker finishes and its queued notifications have been processed. Provider HTTP
calls are interrupted through the same cancellation mechanism. On a storage error execution is
cancelled and the error is logged; persistence is not claimed successful.

This version assumes one daemon per session directory; cross-process session
locking is not implemented. The TUI supports `/connect`, `/new`, `/resume`,
permission replies and reasoning changes through this protocol.
