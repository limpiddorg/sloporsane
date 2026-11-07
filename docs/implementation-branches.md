# Implementation conversations

The persisted session foundation is routed through the daemon protocol and the
Textual Discussion / Implementation plan / Implementation - Validation tabs.
Implem Plan generates a structured plan; Implement runs the accepted steps and opens
construction inspection.

`SessionRegistry::createImplementation` opens an independent session journal from
an idle Discussion whose latest assistant reply is an Objective Plan. The branch
inherits the current conversation, including checkpoints or system updates after
the objective. Provider-specific context, its portability boundary and measured
usage are preserved; opening a tab is not a model switch or a context reset.
Subsequent messages belong only to the branch. Manual tool approvals are not copied.

The parent stores `implementation_ids` and `active_implementation_id`; the branch
stores `discussion_id`, `objective_id`, `inherited_messages` and `frozen`. All are
persisted in SQLite session metadata. Existing sessions default to no branches.
`inherited_messages` marks the boundary of the branch display history. Paging
never presents the inherited Discussion context in Implement.
Branches do not appear as independent entries in the ordinary session list.

Only one branch is active per Discussion; previous branches remain consultable.
After freezing it, another objective can start a new branch with a distinct ID.
The branch is fixed to Sane and cannot request a new Objective Plan. Discussion
cannot send a new message until its active branch has been frozen explicitly.

`freezeImplementation` freezes the branch before clearing the parent's active
reference. These are two journal commits, not a cross-database transaction. If
the operation is interrupted between them, retrying the freeze clears the reference;
`resumeTarget` already treats a frozen branch as a return to Discussion. No private
branch exchanges are merged into the parent journal or its provider context.

`resumeTarget` resolves a Discussion to its active branch, or to Discussion if no
writable branch remains. `session.attach` uses it, so `/resume` returns directly
to Implement when active. Terminal opening metadata belongs to Discussion.

`implementation.open` accepts the current Objective Plan and opens its branch.
`implementation.view` switches to Discussion with an empty `branchId`, or to an
owned branch with its ID. Switching requires idle principal and BTW conversations.
Viewing Discussion does not freeze Implement: the next valid `message.send` in
Discussion freezes it before launching that turn. An Objective Plan carries an
Implementation plan button for its saved branch. Frozen archives hide the composer;
Page Up/Down navigate history and Escape returns to Discussion. Draft text and
images are retained separately when switching tabs in the current TUI instance.

Branch messages and plans are never merged into Discussion's provider context.
The runtime metadata exposes only branch identities, their objective IDs and states.
After opening a new branch, Textual immediately sends `Implementation plan request`
with `implementation_request: true`, without consuming the unsent draft. The same
request regenerates the plan after discussion or interruption. It exposes the
`submit_implementation_plan` tool for that turn only. The plan contains ordered
steps (`title`, `objective`, `substeps`, `validation`); each substep has a `title`,
`intention` and planned `changes`. A step is an exercisable construction outcome,
not necessarily a complete user-facing feature. There is no fixed minimum number
of substeps.

A successful terminal event persists the typed plan as a final assistant message,
with canonical Markdown and `implementation_plan` metadata. Cancellation or failure
does not finalize a submitted candidate. Live and restored plans use the same purple
presentation. Implement is available only while the latest user turn requested a
plan and its latest substantive assistant message is a finalized plan. Otherwise
the action is Implem Plan. The finalized plan is also appended to the live model
context, so further discussion and automatic or explicit compaction continue from it.
Compaction checkpoints remain persisted and reconstructible after resume. Only new
branch exchanges are visible; paging never crosses the inherited display boundary.
Execution follows the accepted plan through the dedicated workspace.

## Execution and review

The first end-to-end implementation connects the following Textual experience to
the daemon scheduler and durable construction/review state.

### Execution modes

Implement opens a choice popup with exactly two options: `Step by step` and
`Everything`. It follows the Ask Choice presentation, with Up/Down to select,
Enter to confirm and Escape to return. There is no `Substep by substep` execution
mode and no redundant confirmation or cancellation buttons.

In `Step by step`, the model builds one complete step, then waits for its final
review validation. That validation automatically starts the next step; there is
no separate Continue action. In `Everything`, the model builds the whole plan
without waiting for review. Review still proceeds in order, one step at a time;
final validation advances review but does not gate ongoing execution.

Execution progress, the currently inspected item and review progress are distinct.
The step being built displays a spinner throughout execution and technical checks.
Step headings remain selectable for their global descriptions; unfinished steps
cannot be inspected. Spinner updates preserve list scroll and selection.

### Execution permissions

Implement execution is an explicit writable phase of the Sane branch, not a global
switch to Slop. The model may modify files and run necessary commands within the
assigned step, subject to the active permission policy and workspace boundaries.

- Ask before execution suspends the exact action and opens the normal permission
  confirmation popup. The action cannot run until authorized.
- AI Verified continues directly when the verifier accepts. A refusal, unavailable
  verifier or failed check suspends the exact action and requests human confirmation,
  instead of returning a refusal immediately for the model to work around.
- Danger does not request tool approval during execution.

Human refusal is not authorization to bypass the decision through another tool or
command. Review is always read-only, regardless of the permission policy, manual
approvals or execution mode. Concurrent execution and review must use separate
permission scopes so neither changes the other's authority.

The harness announces execution permissions when implementation starts and announces
read-only review before its first question. Announcements are repeated on subsequent
phase turns to preserve the current authority across compaction and resume.

`PermissionManager::setImplementationPhase` and phase announcements through
`Session::sendMessage` remain daemon-owned APIs, not client-controlled fields of
`message.send`. The scheduler runs one entire step per model turn, including its
substeps, technical validation and corrections. After checking the step, the model
calls `inspect_implementation_changes`, then `complete_implementation_step` with
one concise explanation per planned substep and an assignment of each final hunk.
The daemon rejects missing, duplicate or unknown assignments. It reconstructs
intermediate file states in semantic substep order and verifies that their final
state exactly matches the validated file. Corrections therefore belong to their
original semantic increment, not a later repair increment. Hunks are atomic: a zone
spanning several intentions belongs to the most appropriate substep; explanations
must not invent distinct edits for substeps with no separate changes.
Only a successful terminal event and finalized snapshot publish the step's substeps.
Ending without submission, cancellation or human refusal interrupts construction.
Review uses an independent read-only agent with the persisted principal context,
the selected item's exact diffs and a persistent review conversation. It does not
change the execution agent's permission scope or feed answers back into construction.

Construction state is stored in `implementation.sqlite` (version 5) in the branch
directory, independently from provider-context compaction. Each item retains its
explanation and exact patches with historical before/after text. The entire step
retains its original snapshot boundary across retries. Whole-step diffs compare
that boundary with the final validated state, not concatenated repair patches.
Git and non-Git text changes use three context lines; binary contents remain in
snapshot blobs and have a binary notice/patch rather than a text-code view.
Version-1 unfinished runs retain a combined boundary for whole-step resumption;
completed historical reviews remain readable.

Restart never automatically reruns an unfinished step. Resume is explicit and
preserves the original diff boundary across attempts. If a crashed file mutation
has no verified outcome, resumption is blocked rather than inventing review evidence.
Existing unrelated edits are part of the initial snapshot and are not shown as new
construction. Filesystem changes concurrent with construction are not attributed to
an individual actor by snapshots.

### Workspace layout

The left column contains the steps and their indented substeps in its upper part,
and the changed-file tree in its lower part.
Real substeps open directly on their first change zone. Left/Right navigates
change zones without wrapping at either end, with no introductory description view.
The current/total counter is centered and bold on a full-width background line above
the file path. The file path stays at the top left: repository-relative for files
inside the repository, absolute otherwise. Substep descriptions appear only in
Validate and the finalized step summary, not below the code. The model/thinking/context
bar sits inside the diff column immediately below its keyboard hint, including when
chat is open; confirmations use this same local bar, not the hidden Discussion bar.
Added lines have green `+` signs, removed lines red `-` signs. Text additions use
ALGAE, removals BOLD, and line numbers the muted palette.

Changes shows all file paths from the step's initial Git snapshot (excluding ignored
files), plus changed/new/deleted files. Before changes are available, root files are
visible and all folders collapsed. Selecting a built step opens only ancestors of
its changed files. Selecting a diff highlights and centers that file in the tree.
File labels carry green `+` / red `-` signs according to their additions/removals.
The baseline is historical, cached from preserved snapshots rather than today's
worktree. An unavailable baseline is explicitly labelled in the tree.
Zones with overlapping three-line context are grouped; distant changes in the same
file are distinct keyboard positions but share a single file node in Changes.
The code view uses immutable historical contents, including removed lines, rather
than the current worktree. It initially renders a window around the selected zone
and progressively extends rendered context when scrolling toward either edge.
Step headings before finalization show only the global name and description,
centered vertically and horizontally, with text wrapping to the available width.
The summary is centered as one left-aligned block, keeping all bullet markers in a column.
Descriptions for new runs request 2-4 short, concrete bullets rather than long prose.
The final Validate entry retains the complete descriptive summary.

The step page also shows live substep announcements with expandable Activity
(tools, reasoning, intermediate messages). A completion announcement provides a
provisional description, not diffs or early validation. Final descriptions and
diff assignments still follow whole-step checks. Successful substep validation
advances selection to the next selectable substep, then Validate; unchecking does
not advance. Historical code uses language-aware syntax colors, added/removed
backgrounds and wrapped long lines with a stable line-number gutter.
Live step updates preserve mounted Activity widgets and their expansion/focus.
Substeps are numbered; a reported unnecessary substep remains visible, struck out,
with its context-specific reason, without waiting for final completion.

Tab opens or closes a side-by-side chat; there is no Discuss button or row.
Opening chat focuses its composer, preserving Left/Right for text editing.
The side chat shares Discussion's styles, UserMessage, ResponseMarkdown, tool views,
reasoning drawers and Activity spinner. Review transfers retain tool and reasoning
content, not just assistant text. Review remains a separate read-only agent.
Its animated Cancel action appears on the model/thinking/context row only while
review is running and cancels only that review. Construction has its own Cancel
button below all displayed step text, including the current substep description,
requiring explicit modal confirmation. Keep running is the first/default choice.
When construction is interrupted, Continue replaces it in the same location.
Neither construction action appears in frozen archives.

The tabs are Discussion, Implementation plan and Implementation - Validation.
Plan and Validation are local views of the same branch; switching between them does
not reload a session. The Plan view preserves planning history and hides its composer
once structured execution has begun. Resuming the parent Discussion selects Validation
when a persisted implementation run exists.

### Discussion and context

Discussion is available for every inspectable step or substep, not just the item
currently eligible for validation. Review remains read-only, but can propose a
rework or a revised plan for the first unvalidated built step in either pace.

The chat continues the Implement planning and execution conversation, rather than
starting an unrelated context. Each question identifies the inspected step or
substep so that the model understands which change the user means, independently
of the execution spinner or review position. Exact diffs for that selection must
remain accessible even if compaction has removed their original context; retained
conversation text alone is not sufficient.

### Review and finalization

Each expanded step contains its real substeps and a synthetic `Validate` entry,
colored MISC. Enter toggles a real substep's visual validation independently;
Escape never removes an indicator. These indicators do not trigger execution.
Complete substeps without distinct file changes are disabled, dimmed and struck through.
Navigation skips them; summaries omit them and finalization does not request their indicators.

`Validate` shows the global name and description, then each substep's status and
concise description with separators, without file diffs. A single Validate button
or Enter finalizes the step. If indicators remain unchecked, a confirmation popup
asks whether to validate all substeps and the step. Cancelling changes nothing.
Finalization accepts all remaining indicators and is irreversible. Steps finalize
in planned order. In Step by step it automatically starts the next step.
After finalization, `Validate` disappears and the heading carries the same complete
summary, without a button. Real substeps remain inspectable.

### Review-driven rework

Review-driven corrections work in both `Step by step` and `Everything`. Technical
consequences for future steps do not alone make a plan inaccurate. If intentions or
planned work become inaccurate, the review proposes a complete revised plan instead
of substituting a local correction. Read-only review forbids direct edits, not these
structured proposals, and never requires leaving Implement for a new conversation.

The independent review agent exposes `propose_step_rework` and
`propose_implementation_revision` only for the first unvalidated, fully built step
with captured construction ownership. Questions alone do not authorize work. Only
successful review turns publish proposals; cancelled/failed turns and obsolete
result tokens cannot start corrections. A successful proposal pauses active
Everything construction and preserves its verified partial result.

The proposal uses the same inline card as Ask-before tool permissions, with
`Accept` / `Decline`, a short summary and a bounded scrollable preview. There is no
confirmation modal. Accepting a step correction starts corrections/checks directly;
Decline dismisses the proposal and resumes paused construction. Proposal requests
ask the model for one or two concrete sentences, not a procedural explanation.

A plan proposal card only offers to open a revised implementation. Accepting it
opens a separate Implementation plan page with discussion on the left and candidate
on the right; it does NOT authorize execution. Discussion uses an independent,
persisted read-only conversation and may publish updated complete candidates.
Only `Implement` on this page accepts the plan and starts correction. Escape or
`Back to steps` leaves it pending; `Resume revised plan` reopens it. Its candidate preserves the validated
prefix exactly and maps retained step identities and substep contributions. Future
steps can be added, removed or reordered. Only acceptance changes the execution plan.
The current Cancel/Continue controls handle interruption. Each corrected step still
requires human validation; Step by step waits for it before continuing, Everything
continues construction independently without auto-validating any step.

Each accepted plan revision creates `Implementation plan #2`, `#3`, etc. in the
steps menu immediately before the stable step identity that triggered it. The
initial `Implementation plan` remains above all steps. These entries show their
own immutable accepted version, not the latest candidate or the construction.
Only final plan acceptance creates the numbered entry and selects the corrected
step. Numbers, versions and insertion positions survive session restoration.

Corrections operate on existing code in the isolated checkout, never in the user
workspace. The first corrected step retains its original baseline. Captured substep
ownership is preserved or explicitly mapped to revised substeps; corrections declare
their current target before editing. Final diffs show the cumulative corrected
result, not repair chains or a global redistribution of hunks. Review indicators
are cleared. Each accepted rework starts a fresh visible construction pass: no
old explanation, completed-substep flags, Activity or Validating animation is
reused. Its progress starts at zero and every current planned substep is re-examined
before new technical validation. The original ownership evidence and baseline stay
intact underneath; this is correction of existing code, not a blind rebuild.
Old activities remain in archived results and aggregate statistics stay cumulative.
The steps menu shows superseded unvalidated steps and their old substep titles in
muted strike-through, followed by the new pass. Like any other step, the new pass
stays collapsed during construction; its indented substeps appear only after it
is complete and the user expands it, before the following steps.
Old rows cannot validate anything. Existing validated prefixes are
not duplicated. Revised-plan links remain between the versions they introduced.
Previous plans and
results are archived durably. `Previous results` opens historical step diffs;
`Revision history` in the plan view also includes removed steps. Left/Right selects
past results, Escape returns without changing current review or validation.

Human-validated steps remain immutable. Revision refuses any import intent from
the reviewed step onward. A durable rollback intent preserves historical snapshots
before invalidating downstream seals. Proposals carry monotonic revision tokens;
stale acceptance/validation is rejected. Interrupted corrections retain their
accepted request and baseline; restart does not rerun them automatically.

Accepting an Everything revision returns current/later steps to MUTED, corrects the
current step, and re-examines retained successors in order against their corrected
predecessors. Contributions replay per original substep using three-way text merging
and explicit state checks for binaries, symlinks and tool-journaled ignored files.
Conflicts leave current code intact and provide preserved evidence to the responsible
correction step, never silently dropping a contribution. Re-examination can require
no edits, but still runs checks and returns to human validation. New future steps
resume normal construction afterward. Downstream diffs use the corrected predecessor
as baseline. Rollback/replay intents are persisted before mutation and safely
retryable; execution generations separate new ownership evidence from history.

### Keyboard behavior

- Left/Right navigates change zones without wrapping when chat is closed; the hint omits these keys on step/Validate pages and single-zone views.
- Up/Down always controls the steps list, including from Changes or the chat composer.
- Enter in Changes toggles a folder or selects the file's first zone available in the current substep.
- Enter on a heading expands/collapses it; expansion selects the first complete substep.
- Enter toggles a real substep before finalization, or confirms Validate.
- Ctrl+Left/Right navigates views while chat is open; plain Left/Right edits text.
- Enter in chat only sends a question; Escape cancels chat activity, not validation.
- Tab opens or closes chat. Page Up/Down scrolls code or the open chat.

A muted hint below the view reflects the available shortcuts.
