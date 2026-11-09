# Product contracts

These requirements supplement [AGENTS.md](../AGENTS.md). Read the relevant
sections before making changes; protocol details remain in the linked documents.
TUI interactions here target Python/Textual, not mandatory FTXUI parity.

## Language

- Application text and generated UI content are in English, including objective
  and implementation plans, step/substep titles and explanations, diff explanations,
  progress labels, review proposals, summaries, recaps, session titles and conclusions,
  even when the user's request is in another language.
- Preserve exact identifiers, paths, commands, quotations and user-provided content
  where needed. Do not translate stored history or rewrite historical snapshots.

## Discussion and plans

- Sane Discussion stays full-width, without a permanent plan panel. Below the last
  assistant message of each completed turn, a collapsed drawer retains the plan
  active at that time. Add nothing to intermediate messages, Slop turns or turns
  without a plan. Switching modes never hides or changes historical Sane drawers.
  Slop turns have no drawer; returning to Sane carries the last active plan even
  without a new publication. Closing an implementation completes its objective: retain
  historical drawers, but stop carrying it onto new turns or supplying it as the
  model's current plan. A new publication starts a new objective; this boundary
  persists after resume.
- The title is `Objective plan`, or exactly `Objective plan · updated` for a turn
  publishing a version, including after resume/pagination. Title and arrow use Sane;
  expanded content has a green stripe and subtle green background.
- Publish only complete versions, through replacement or targeted patch, retaining
  previous versions. Browsing history does not change the model's current plan.
- Centered `Implementation plan` and `Implement (Auto plan)` buttons start from the
  displayed version and remain at the bottom before launch. Auto plan first asks
  Step by step / Everything, then opens the steps; its generation button has a
  spinner. A finalized plan starts construction without additional acceptance.
  Failure/cancellation stops the spinner and allows retrying with the chosen pace.
- After launch, replace bottom actions with `Implementation`, right-aligned on the
  toggle line and accessible without opening the drawer. An already accepted plan
  opens its existing branch. Hide actions and Implementation access in Discussion preview.
- The unaccepted implementation plan stays to the right of chat with a Sane divider,
  including after restoration. Its accepted version appears in Sane chat with
  Implementation access. `Implement` is centered below the plan; the chat control
  remains Sane, or Cancel during generation, and never generates a plan.
- One Implement tab groups planning and construction. After acceptance, select the
  first step. The left-aligned, indented `Implementation plan` entry above steps
  shows the frozen plan in the diff area with Changes entirely hidden as soon as
  navigation arrows return to it, without another Enter, just like steps. The
  initial plan and revisions use a full-width row with identical vertical spacing,
  hover highlighting and blue selection. `Implementation plan` and `Conclusion`
  share this wide, left-aligned, bold row with message background (`content-background`).
  The selected blue background covers the entire row without gray side margins.
  Navigating to `Conclusion` shows its content directly, without Enter; hide Changes
  there too. Selecting a step restores Changes and inspection; plan view excludes construction.
- Plan drawers, panels and buttons do not take focus from the composer.
- During generation, Discussion is read-only without detaching/canceling work.
  Throughout construction, including between steps, it stays frozen without a
  generation indicator; its composer asks the user to finish implementation.
  Preserve page and position on returning. Once every step is built and validated,
  returning to Discussion directly closes implementation and makes the composer
  editable. Leaving a completed archive also returns to real Discussion, not its
  locked preview, unless another implementation remains active. Historical
  implementation access remains on the old message.

## AI Check

- `/permissions` offers Ask before, AI Check and Danger, independently of Sane/Slop.
  AI Check shows `API: …`, then `Model: …` on two lines and the threshold as soon as
  selected, without activation before Save. Do not show a threshold migration
  paragraph. Without a configured key or usable draft model, hide API/Model lines
  rather than presenting defaults as active configuration. Ask before and Danger
  retain direct selection.
- Percentages estimate `allow` probability, not the provider's `confidence` field:
  authorize at or above the threshold. Retain saved values, document their meaning
  and the absence of safety guarantees or equivalent calibration across models.
  At 0, do not call the verifier; Sane/review restrictions retain priority.
- Save applies draft model, threshold and key together, then closes after
  acknowledgement. Errors leave settings editable. Closing without Save discards
  the draft and clears its temporary key.
- Change model opens a popup: choose OpenAI Decisions or TypeSafe, then enter full
  endpoint, free-form model and masked key. Offer official endpoints and preserve
  historical OpenRouter settings; no daemon call before Save in the parent.
  Use model only returns to the parent with the draft.
- Escape in fields returns to API selection and clears the entered key; Escape
  there cancels the popup. A saved key is never read back into the form. Reuse
  requires the same protocol and endpoint; changing only the model may retain it.
  Secrets stay in protected auth.json, never chat.
- Require `permission_check_version: 1` to configure AI Check. An older daemon must
  not silently ignore the model or new threshold semantics. Histories remain
  immutable, with their original recorded scores.

## Textual interactions

- `wait` observes an external operation through a monitoring command, with the same
  permissions as `bash`. Tests, builds and installations stay in `bash`, regardless
  of duration. Without a native waiting command, the model supplies bounded polling
  with explicit success/failure conditions. Intermediate output does not wake the
  model; process exit, timeout or cancellation returns control. Default timeout:
  one hour. Stopping the local watcher does not cancel the remote operation.
- During active `wait`, Activity keeps its spinner and shows a status line beneath,
  after content when expanded, aligned with Activity's A. The model supplies
  `status`, at most five words (preferably four or five), often `Waiting…` but
  without a required prefix or trailing dots. `.`, `..`, `...` cycle in stable
  space. A slow, clearly white shimmer crosses only the text left to right,
  without lighting dots or background. Stop animation at result/interruption;
  never animate completed history.
- Tool titles are not bold. For bash, show `mainCommand` in gray between status
  and any exit code; this descriptive label changes neither execution nor
  permissions. Preserve labels and grep/glob patterns in lightweight summaries,
  without reading old bodies.
- Title paths, read rows and file headers use one line, middle-shortened with
  beginning/end retained and recalculated on resize. Summaries gray, content DEPTH.
- Indent Activity's inner arrows by two columns. Expanded content begins two
  columns after the title's first letter, including Reasoning, tools and Compaction.
- Modals: Enter confirms, Escape returns/closes, without redundant
  Confirm/Connect/Cancel/Close buttons. Distinct actions remain visible;
  left/right selects an action, confirmation/Save selected by default. Preserve
  left/right editing inside fields.
- Up/down controls the last visible active list used, even from a field/action;
  without a list, retain normal navigation. In Implement, up/down always controls
  steps, even from Changes or review. Show appropriate keyboard help.
- Chats follow messages only when the user is at the bottom; never move someone
  who has scrolled up in history.

## Version and updates

- Show only `dev` or `vMAJOR.MINOR.PATCH` at top right on the tabs line. Version is
  embedded at build time through `SOS_VERSION`, defaulting to `dev`, or the release tag.
- A `dev` build never queries the update backend. A stable release checks the
  published version at startup, in the background without blocking, using the
  installer's backend. Network failure is silent.
- New version: English popup, `Not now` selected by default, Escape closes. Warn
  that all the user's SOS TUIs and daemon will be closed.
- After agreement, exit Textual and hand over to an independent Bash process in
  the same terminal. One confirmation only; prepare/verify downloads before
  stopping other processes. Installation survives SOS/daemon shutdown, then
  relaunches only the initiating instance in its workspace, with its socket.
  On failure, retain the previous launcher and show the error in the terminal.
  Reject any version different from the one confirmed.

## Construction and inspection

- A step expands only once built. Clicking inspects, without Cancel or validation.
  Enter on the active step triggers Cancel with confirmation, or Continue if
  interrupted. Reopening interrupted/failed construction selects the working step
  with Continue; do not force selection live or in archives.
- On reopening, immediately show Loading implementation and a spinner in the diff
  area until state arrives; stop waiting and report errors.
- Preserve selection and diff during live updates. Returning to a substep restores
  its diff position (file/hunk), remembered only in the TUI per version/substep,
  never in the session. Keep selection visible with minimal scrolling, without
  movement when selection is unchanged. The plan/steps/Conclusion menu uses one
  outer scroll, not an inner step scroll.
- Historical diffs remain navigable in the full file. Initial display does not
  await full syntax highlighting; prepare it off the UI thread using whole-document
  lexical state. Share measurements and wrapped lines among hunks of identical
  historical content and width, without moving scroll when colors arrive or
  reading the current checkout.
- Only added/deleted lines in the current hunk have their MISC gutter on the same
  gray background as step Version arrows (`#3A4655`), never context lines. Do not
  change `+`/`-` signs or green/red code backgrounds. Two spaces after the sign
  belong to the code background, not the gutter; align context and continuations too.
- Above the diff, show its explanation by default in a small panel with a divider.
  `E` hides/restores it only when chat is closed; show `E Toggle explanation` to the
  right of the filepath on the same line, not in bottom help. Preserve the choice
  during navigation; omit the panel on plans and step summaries. Old diffs without
  text explicitly indicate that no explanation is available.
- During Implement execution, `edit`, `write` and `apply_patch` require a nonempty
  explanation captured with the mutation, without another tool or model call.
  The model may reuse suitable text for similar edits. After review, rewrite a
  summary of the entire current diff, including retained earlier changes. A
  correction replaces the affected hunk's text; a merge retains other hunks'
  explanations separated by a newline. Preserve these texts in historical versions.
- PgUp/PgDown scroll the displayed content (diff, step text, plan, conclusion),
  regardless of focus; if review is opened via Tab, target its chat instead.
- Show all numbered planned substep titles from the start; activity/explanation
  only when available or for current work. Content titles/descriptions retain
  normal colors, without `[x]`/`[ ]` markers or validation green.
- Below the step description: completed/total at the left of a text `[###---]`
  bar, stable width and margins, in MISC. Count actual finalizations, not model
  promises; explicitly dismissed substeps count. The bar stays full during repair.
- Activity timers follow real execution even offscreen. Display daemon active
  duration, extended locally, without restarting on navigation.
- During technical validation: `Validating` above, with a fast synchronized white
  shimmer back and forth across word/bar, stopped on completion/interruption.
  After substeps, show unnumbered `Validating the step` in MISC, with Activity but
  no description.
- Reserve tests, builds, linters and verification inspections for technical
  validation, not construction substeps; implementation reads remain allowed.
  Validation repairs target original substeps. Capture ownership at mutations/
  boundaries without final global hunk redistribution or loss of unchanged
  contributions. If a validation capture fails leaving unowned changes, a valid
  `correcting` declaration may attach the pending interval to its explicit target,
  including after resume. Do not reassign captured contributions; mutation and
  completion without a target remain refused until that interval is recovered.
- Keep validation proportionate to step changes and explicit plan requirements.
  Reuse still-valid successful results; no global rereading or repeated checks
  without a concrete uncertainty or regression risk. After repair, rerun affected
  checks. The last step does not trigger another whole-implementation audit.
  Required checks remain mandatory.
- Validation remains a required boundary, not a requirement for new checks. If
  none are needed, explain why and complete the step. The plan specifies necessary
  checks or why none are needed, without inventing a verification substep or a
  systematic final audit.
- During normal construction, completing/dismissing a substep automatically
  activates the first remaining substep, or validation if all are finalized.
  Persist this transition atomically with finalization, on the same snapshot,
  without attributing mutations to it. Also activate the first substep before
  execution without a model call; resume preserves the existing boundary.
  Repair/revision targets remain explicit. Activation proves neither that editing
  has begun nor human validation.
- Size plans/tests to the request and concrete risks, without multiplying steps
  or hypothetical edge cases merely because of Sane. Prefer existing tests and
  short checks; do not create a simulation framework to replace an unavailable
  browser without explicit need. Completions/conclusion reuse captured results
  without redundant recap, while reporting actual limitations.
- Submit `complete_implementation_step` before a normal end. Successful submission
  ends the turn without a model callback or later tool. Reuse latest
  completed/unnecessary explanations unless explicitly revising them; no systematic
  diff inspection for captured-ownership construction. Group ordered
  announcements/tools when no decision depends on an intermediate result.
- Finish missing in-scope work, repair and recheck. Blockage, refusal or
  cancellation remains an explicit interruption, never fictitious completion.
  Unrun required checks, unexplained failures and regressions block completion.
  An unrelated failure demonstrated on the preexisting baseline may remain
  documented in step/conclusion without blocking technical completion; do not
  claim it passed or use it to authorize unrelated repairs.
- A substep without a final diff automatically leaves diffs requiring validation;
  retain its activity/explanation. A struck-through substep shows a generic reason
  that previous changes make it unnecessary, without fabricating work.
- Cumulative duration and aggregate throughput appear only below menu titles,
  not in content. Throughput = output tokens / model-call time; absent if the
  provider supplies no tokens. Statistics muted, or TITLE yellow on blue selection.
- Active step in MISC: line below title with `[completed/total]`, the first two
  current-substep words, `...` and spinner. Validated steps Algae, future steps
  muted, built/unvalidated steps white. Validation green only in menu/Human
  validation recap. Construction Validate/Cancel use DEPTH; chat Cancel uses Sane,
  with black centered Sane label outside generation.

## Changes tree

- Read direct children of the root and terminal directories containing changes.
  Add ancestors without loading intermediate neighbors; read other directories
  only on explicit expansion. No recursive daemon/TUI inventory.
- Always read the historical snapshot, never the current checkout. Compact
  nonbranching chains while retaining branches/last open directory; shorten chains
  in the middle if needed, never filenames.
- Each terminal directory shows five items total, including every change and its
  required branches even if this exceeds five. `...` only for hidden neighbors,
  not clickable. Changes remains mouse-operable without taking step focus.
- With hidden neighbors or shortened paths, a scroll-accessible final `Show all`
  reveals neighbors and full structure. Open only change ancestors by reading
  direct children, not other directories recursively. `Compact` restores the
  compact projection. Preserve live choice; reset on step/snapshot change.

## Review and corrections

- Review only on the first built, unvalidated step and its substeps, at either
  pace. Elsewhere, Tab or `/` reports unavailability without blocking inspection.
- From the menu, `/` opens/focuses review and inserts `/` without replacing the
  draft. Suggestions: skills and only `/connect`, `/model`, `/permissions`, `/mcps`;
  explicitly reject other native commands. Skills do not change permissions.
- Each step retains its conversation, construction context stopped at its end,
  and precise prior-review summaries. Each question includes the viewed file/hunk.
- Composer remains editable during response; retain the next draft without starting
  another turn. When strictly empty and focused, left/right navigates diffs without
  closing review or moving focus; with text, preserve normal editing.
- A proposal focuses Accept; left/right chooses Accept/Decline, even from empty
  chat, never diffs. Acceptance closes review and immediately selects the corrected
  step with Preparing during asynchronous operations.
- Show one complete implementation version at a time. `← Version N →` is above
  Implementation plan in the left menu, centered bold label, composer background
  (`input-background`) on the strip and arrows disabled at boundaries. With multiple
  versions, select the label by mouse or ↑ from the plan; ←/→ then changes version
  without navigating diffs, and ↓ returns to the plan. The main panel shows centered
  bold `Version N` while selected. With one version, the entry is not focusable.
  Arrows have a slightly lighter background than the composer (`#3A4655`);
  `Show all` / `Compact` use normal popup-button background. All turn blue on hover,
  without permanent blue background. The selector controls that version's plan,
  steps, substeps, Activities, explanations, technical validations and snapshot diffs,
  including subsequently removed steps. No Previous results, interleaved numbered
  plan, old struck-through step or per-step selector. Content is top/left-aligned,
  even for the initial version alone; Implementation plan is left-aligned.
  After correction acceptance, immediately show the new version with Preparing,
  then normal progress. Global summary for initial work, accepted request for
  correction. Old versions stay read-only and stable live; browsing restarts
  nothing and changes neither the version awaiting validation nor real execution.
- Only acceptance integrates the current review segment into construction, as
  history without replay or permissions. Persist its boundary; keep exchanges
  visible but stop injecting them separately into subsequent review. Later-step
  summaries cover only the unintegrated segment.

## Summaries and compaction

- Prepare review summaries at the next step's first message, then persist/reuse
  them. Normal generation, Activity and cancellation, without popup, validation or
  special announcement. Selected model, dedicated context, original exchanges
  before/after compaction, never compaction or construction messages.
- Bound sources by chunks and recursive merging. Review summaries retain two corrections
  followed by an emergency synthesis. Ordinary compaction requests MAXIMUM 1% of the
  effective input window with a 2,000-token budget floor, less whenever possible,
  with essential continuation state only. Accept up to 5% internally with the same
  2,000-token floor, without advertising this tolerance to the model. Both budgets
  are capped by the model output limit; the floor is not a minimum summary length. Allow at
  most three attempts per synthesis (initial call and two size corrections), keeping the
  shortest candidate. If still too long, truncate at a UTF-8 boundary with a warning;
  never discard original history or hide provider failures/cancellation.
- Display `Compaction` and its spinner without a counter on the initial call of each
  chunk or merge. Only size corrections show `2 / 3` or `3 / 3`, without a bullet or
  the word `Attempt`. Ordinary chunk/merge calls do not increase the retry counter.
- Check construction + summaries + question before injection; compact construction
  first if needed. Review also compacts during responses.
- Each actual compaction: red `Compaction` drawer, arrow aligned with Activity,
  spinner during execution, no bar, between Activities. Same rule in construction
  and validation; initial compaction may precede the first Activity. On success,
  show the next Activity and spinner without awaiting its first message.
- After compaction, retain the latest exact phase announcement after the Sane/Slop
  reminder; never reactivate old execution authorization during review.

## Human validation and conclusion

- Human validation shows only the substep recap, not the global description.
  Available only on the first unvalidated step; Escape closes confirmation.
- Import before globally validating the step. Disable Validate during import and
  prevent double submission. After confirmation, collapse the step and select the
  next summary with titles/substeps and Preparing; do not advance before
  confirmation or on conflict.
- Conclusion follows the last step and is available once all steps are built.
  The last `complete_implementation_step` supplies a global recap slightly fuller
  than explanations, without exhaustive repetition, with checks/caveats, retained
  in the archive. No Close before final human validation or in archives.
- After final validation, open Conclusion and immediately focus the standard Close
  button at the content bottom, even offscreen, without requiring automatic scroll.
  Selected, it is DEPTH blue; Enter returns to Discussion, closes Implement and
  freezes the archive.

## Workspaces and isolation

- `worktree` lists/creates/selects within the active repository; `workspace` moves
  the conversation to an existing directory, with/without Git, including another
  repository. Create/switch only on direct request or clear instruction from an
  explicitly invoked skill. A discovered file is insufficient. No extra popup;
  allowed in Sane, not during Implement/review. Listing remains available.
  Commit/merge/push remain separate Bash actions.
- Skills/MCPs stay inherited from the source workspace; AGENTS follows the active
  directory. No configuration or local-change copying on move. `/resume` restores
  workspace; a new session starts in the current directory without automatic
  creation. `/undo` stays tied to original paths; Implement imports into the active workspace.
- User-created workspaces start from HEAD under
  `$XDG_DATA_HOME/sos/workspaces/<repo>-<identifier>/<name>/`, branch `sos/<name>`.
  Identifier derives from canonical shared Git directory, shared across worktrees
  but distinct across clones. Do not move/rename existing worktrees or clean them
  automatically. A PR triggers no special switch or warning.
- `/worktree` selection and deletion are Slop-only. List and `New…`: Enter
  selects/creates, Escape returns/closes. Select default on left, Delete on right.
  Exceptional deletion confirmation: branch/location only, Cancel selected on left,
  Validate on right; left/right chooses, Enter executes, Escape cancels.
- Forbid deleting the main checkout and destinations used by integrations or
  attached/loading sessions; show Current / Opened in another session in coral.
  Recheck and reserve in the daemon. Do not force dirty/locked removal or delete
  branches. Closed sessions do not block, but a missing destination blocks resume
  without silent fallback.
- Implement uses `$XDG_DATA_HOME/sos/worktrees/`, separate from user workspaces.
  Baseline reproduces local state only on the SOS branch, without stash or user
  branch/index changes. Storage commits have no hooks/signing. Human validation
  imports only the step into files, without a user commit; conflict blocks the
  step with the existing Slop banner. Interruption retains checkout; Close removes
  temporary checkout/branch but retains archives and pinned objects for inspection.
- A Git repository without a first commit starts Implement automatically with an
  empty SOS-only base commit, then copies the local state into its checkout.
  The user's branch remains unborn and its index is preserved, including on import.
- Without Git or a Git ancestor, use a private repository outside the workspace,
  no user `.git` or remote. Respect `.gitignore`/global excludes; always exclude
  `.sos/` even if reincluded. Traverse nested repositories at any depth as ordinary
  files without their `.git`. CoW copy if available (`clonefile` / `FICLONE`), else
  ordinary copy, never hardlinks to originals; then capture baseline. This is
  neither a sandbox nor a guaranteed instantaneous copy.

## Skills

- Discover `<workspace>/.sos/skills/` then `$XDG_CONFIG_HOME/sos/skills/`, workspace
  winning on name collision; never `.claude/` or `.agents/`. See [skills.md](skills.md).
- `/` is shared with native commands; native names reserved. Exclude conflicting
  skills from suggestions and report errors without blocking native commands.
- Suggestions: alphabetically sorted matching skills first, Algae, command on left,
  bold `Skill` on right; sorted natives next, MISC, help title without `/`. Tab
  selects, Escape hides before closing/canceling anything. Complete at cursor
  anywhere without changing surrounding text; a prose reference is not a
  structured invocation.
- Show literal `argument-hint` after names in suggestions, not help title; gray
  composer hint only before argument text in the initial chain. Do not strip angle
  or square brackets. Up to six consecutive leading skills each receive the same
  final raw text, without TUI binding/validation.
- `arguments`: name list or space-separated string, positional aliases only.
  `argument-descriptions`: optional name → description mapping, numbered in argument
  order; descriptions without aliases allowed. No required, `{name}` or named
  object. Ignore other frontmatter without diagnostics or mechanism activation.
- Single substitution of `$ARGUMENTS`, `$0`, `$1`, `$ARGUMENTS[N]` and aliases,
  shell quoting without execution. Missing indices literal, missing aliases empty;
  without a placeholder receiving text, append `ARGUMENTS: ...`.
- Activity: `skill · name · status` live/resumed, without opening arguments to read the name.

## MCP and providers

- Codex retains `system`/`developer` messages in input order: do not extract late
  announcements into initial `instructions`. That required field stays constant;
  appending a message does not rewrite a sent prefix. This neither removes explicit
  compaction boundaries nor guarantees cache hits. In both modes, send `system`
  messages as EasyInputMessage without `type`, accepted by Codex; adaptation affects
  only the network copy, not order, role, content or stored history. Ordinary
  OpenAI API retains its existing format.
- Daemon MCP client: stdio and Streamable HTTP, not legacy HTTP+SSE. JSON
  `mcpServers` in `<workspace>/.sos/mcps.json` or `$XDG_CONFIG_HOME/sos/mcps.json`;
  workspace entry fully overrides a global namesake. `url` or string `command`
  with array `args` / public `env`; no required version. Reject older formats
  without rewriting. Enabled by default, separate approval required per new config.
- `/mcps` manages only Connect/Disconnect and Enable/Disable; add/edit/delete through
  files, no `/add`, Refresh, Credentials or separate Approve. Compact list:
  Server / Scope / Connected / Enabled, actions on one line, no Close. First server
  and Connect/Disconnect selected; help for up/down server, left/right action,
  Enter, Escape. Brief configuration guidance in model context, without secrets.
- Connect chains approval, necessary automatic authentication and connection.
  Approval/token use separate popups, token field masked. OAuth only in this
  explicit flow: popup above list, real link and Open in Browser as soon as URL
  discovery succeeds, never invented; Escape cancels and returns to list.
  Do not enlarge the list.
- Connected and Enabled are independent: disabling hides/refuses new calls without
  closing connection or canceling active calls; explicit connection allowed while
  disabled. Only `readOnlyHint: true` makes a tool eligible in Sane, under remaining
  permissions; absent/false means potentially mutating, not a security guarantee.
- Choices persist immediately by name, globally in
  `$XDG_CONFIG_HOME/sos/mcp-preferences.json`, never per session/workspace. Restore
  desired connections even while disabled with workspace config/credentials/approval.
  No invented server for an absent name, no automatic OAuth.
- Restore in background without waiting for handshake in `integration.configure`.
  Connecting in list; a turn's tools await restoration in Activity. Failure in
  list + banner without changing preference or automatic retry in this daemon.
  Disconnect persists. Without preference, honor file and connect enabled/approved
  servers on first need. Global preference overrides enabled without editing file.
- No resources/prompts or advanced OAuth/secret management in TUI; advanced options
  through files. No autonomous model resource reads or notification-triggered
  injection. Secrets separate per [AGENTS.md](../AGENTS.md) and [mcp.md](mcp.md).
- One authentication/method per provider; reconnect replaces only its entry.
  `/connect` selects/remembers the first compatible model of the last successfully
  connected provider. OpenRouter: `sort=newest`; OpenAI API: created descending,
  missing last; Codex: priority ascending, missing/invalid last; stable ties.
  Textual selects the first result on opening and each search, never randomly.
- Model/reasoning/context maximum form a persisted per-conversation triple; last
  explicit choice also defaults new conversations in `config.json`. `/resume`
  restores without changing defaults/other sessions/historical metadata. Chosen
  maximum is at least 32,768 tokens and at most the provider's known maximum.

## Experience references

Related approaches include [AI Code Walkthrough](https://github.com/mmuminovic/AI-Code-Walkthrough),
[Kiro](https://kiro.dev/docs/specs/), [Superpowers](https://github.com/obra/superpowers)
and [Cline](https://docs.cline.bot/features/checkpoints). They prove neither Sane's
uniqueness nor future adoption; spontaneous feedback from a real task is stronger
evidence than stated interest.
