# SOS Neovim client

Requires Neovim 0.10+ on macOS/Linux.

## Automatic development installation

With Neovim on PATH, configure from the repository root with `cmake -S . -B build`,
then run `cmake --build build -j8`. The default build installs both the existing
TUI and the native plugin. Restart Neovim and use `:SosNew`; no init.vim changes,
plugin manager or setup call are required. Provider credentials must already be
configured through the TUI. Python 3 is needed for automatic daemon startup.

CMake queries a clean Neovim for `stdpath('data')`, respecting XDG_DATA_HOME and
NVIM_APPNAME. The plugin goes in `site/pack/sos/start/sos` beneath that directory,
with a matching daemon in `bin/sosd`. Each build refreshes it. Missing or older
Neovim skips installation without blocking the TUI. No editor configuration or
mappings are overwritten. Remove any earlier manual runtimepath entry to avoid
loading a second development copy. Close existing SOS integrations before using
an incompatible rebuilt daemon; installation never restarts an active daemon.

Use `-DSOS_INSTALL_NEOVIM=OFF` to disable this, or
`-DSOS_NEOVIM_INSTALL_DIR=/absolute/destination` to override the plugin directory.
For agent verification, use only build-ai/, for example
`-DSOS_NEOVIM_INSTALL_DIR="$PWD/build-ai/nvim-data/site/pack/sos/start/sos"`.
The `install-neovim` target can also be built on its own and works independently
of `SOS_BUILD_TEXTUAL`.

## Floating dialogs

SOS choices, confirmations and text input use centered, rounded floating windows,
independent of `vim.ui` overrides and without requiring Telescope or Plenary.
Choice dialogs have a fuzzy search field and a highlighted results list: type to
filter, use Up/Down or Ctrl-N/Ctrl-P to navigate, Enter to select, and Escape or
Ctrl-C to cancel. In Normal mode, j/k navigate and q cancels. Empty results never
confirm an action. Text input uses Enter to submit and Escape to cancel.
The live step selector uses the same floating frame. Dialogs resize with the editor
and restore the originating window and editing mode unless an action changes focus.
Provider credentials still belong in the TUI, never in chat or these dialogs.

## Manual source loading

Alternatively, add `integrations/neovim` to your `runtimepath` (or use your plugin
manager's local-directory support), then:

```lua
require('sos').setup({
  socket = '/tmp/sosd.sock',
  daemon = '/absolute/path/to/sosd',
  auto_start = true,
  timeout_ms = 10000,
  python = 'python3',
})
```

Connection commands are `:SosConnect`, `:SosDisconnect`, and `:SosReconnect`.
Use `:SosNew` to detach into a fresh draft (the daemon creates the durable session
on its first message), `:SosResume` to choose a saved session or retained unsent
draft, and `:SosToggle` to hide/show the vertical conversation split. Opening with
the toggle focuses the composer without changing the editing mode. Locked contexts
remain read-only. Hiding does not cancel generation.
The first opening connects/configures the daemon to display the saved model,
thinking level and context budget before any message or durable session exists,
as in the TUI. Last-turn usage remains unknown until a provider response arrives.
Loading the plugin alone does not connect; reopening after a disconnect still
requires `:SosReconnect` rather than silently reconnecting.
`:SosSend` explicitly sends the multiline composer;
`:SosCancel` requests cancellation and `:SosMode` toggles Sane/Slop only after
daemon acknowledgement. `:SosReasoning` cycles daemon-supported thinking levels
without consuming the composer draft. Switches that cancel active work require confirmation.
The transcript is protected; edit the bottom composer in Insert mode and read
in Normal mode. Drafts remain in memory until editor exit, including drafts from
previous sessions. Existing file windows are left intact.

The chat opens with a five-line composer and gives the transcript the remaining
height (subject to Neovim's available space). Both splits remain normally
resizable, without fixed heights or widths; rendering and context changes do not
reset a manual resize. Its composer header follows the TUI's AgentInfoBar:
model and thinking level on the left, last-call context usage and percentage
centered, and a `Sane` or `Slop` badge on the right: bold black text on a green
or pink-red background, respectively. Like the TUI button, the badge is one row
high and 15 columns wide, with a centered label and no right margin. It shrinks
only when the panel cannot fit it. Only the badge has a colored background.
Unknown context
is `-`, not an estimated token count. Narrow panels hide the context counts,
thinking level, percentage and finally the model, in that order, preserving the
mode label. The input keeps the editor's normal background, without a decorative
rule. The transcript header shows `Discussion` in a white-on-blue active tab;
entering implementation keeps a muted, disabled `Discussion` label beside the
active `Implement` tab. The muted, non-bold application version stays on the right
(`dev` when running from source). An empty conversation has no welcome label.
Installed builds embed the same `SOS_VERSION` as the TUI. Mode, idle/progress
labels and workspace paths are not repeated in this header.
Locked contexts remain read-only. Neither chat window shows a buffer-name
statusline, and the composer has no line numbers or fold/sign gutter.

Long drafts scroll and wrap inside the field instead of growing it. Native resize
commands, window balancing and the user's layout plugins control both splits.
In `sos_input`, `Tab` toggles Sane/Slop in Normal and Insert mode, preserving focus,
editing mode and draft. The displayed mode changes only after daemon acknowledgement;
locked/non-Discussion contexts keep the existing mode restrictions. This local
default takes precedence over completion's Tab mapping in the composer only.
`Shift-Tab` and all transcript Tab mappings retain their native or user-configured
behavior. Use normal window navigation (`Ctrl-W`) between the splits.
From the transcript, `i` focuses the composer and `q` hides the chat.
Closing either chat split closes its partner without canceling work or losing
the draft. No global statusline/layout options or normal Insert/Escape/send
behavior are changed.

Live editing-tool results (`write`, `edit`, `apply_patch`) trigger native
`checktime` only for already-loaded buffers matching their reported absolute
paths. Successful whole-step imports check the affected original-workspace files.
Terminal execution events also check eligible buffers in the reported workspace,
covering shell writes and partial effects on failure/cancellation. Checks coalesce
on the editor event loop and never open files, switch focus or set `autoread`.
Only ordinary, modifiable, unmodified file buffers with `autoread` enabled are
eligible; dirty buffers and inspection-owned evidence are never checked/reloaded.
There is no extra conflict UI or automatic save: native write-time warnings remain
in charge of unsaved editor/disk divergence. Historical transfers do not trigger checks.

Sending a message positions the latest user turn at the top immediately, before
daemon acceptance. Short replies fill the space below it; longer replies follow
the bottom only while you have not scrolled away. The accepted echo does not
reposition the turn. Activity growth and timers alone never pull the view down,
but keep follow intent for the next reply. Returning to the bottom resumes it.
Wrapping and closed folds count as screen rows, including navigation within a
single long paragraph. From the composer, `PageUp` / `PageDown` scroll the
transcript in Normal or Insert mode without changing the draft, focus or mode.
The focused transcript retains native cursor/scroll control during incoming
updates. Only changed text rows are replaced; reading positions survive context
switches and hiding/reopening the chat. Scroll settings are local to the transcript.

Activity timers update only their header rows and highlights, without projecting
the conversation or rebuilding its folds. Fold expressions use buffer-scoped Lua
tables directly; full fold reconstruction is reserved for structural changes or
context switches. Incoming daemon events update session state immediately but
coalesce chat rendering until the next event-loop turn. Local actions remain
synchronous. `tests/performance.lua` exercises long transcripts, targeted timer
writes, native fold preservation and batched redraws, and records measurements
under `build-ai/neovim/`.

Normal-mode `<Plug>(SosNew)`, `<Plug>(SosResume)`, `<Plug>(SosToggle)`,
`<Plug>(SosSend)`, `<Plug>(SosCancel)`, `<Plug>(SosMode)` and `<Plug>(SosReasoning)` actions have no default
user shortcuts. Optional setup `mappings = {Toggle = '<leader>ss', Send = '<leader>se'}`
installs only unused Normal-mode mappings. Loading creates no connections or
windows and does not change cwd. Never use the human `build/` for verification.

Supported composer slash commands (no arguments) are `/help`, `/new`, `/resume`,
`/mode`, `/cancel`, `/model`, `/reasoning` and `/permissions`. Use native omni
completion (`<C-x><C-o>`) in the composer. `/model` lists authenticated catalogs,
then asks for a context budget; `/reasoning` cycles daemon-supported levels.
Live permission requests show details in the transcript and ask for Deny, Allow
once, or Always for the displayed scopes when available. Dismissal of the prompt
does not grant permission; `:SosPermissions` (or `<Plug>(SosPermissions)`) reopens it
from any context, including read-only construction, previews and archives.
`/permissions` also reopens it from a composer. Unsupported slash commands
are rejected locally and remain in the draft.

`:SosContext` selects Discussion, implementation planning or eligible whole-step
review. Each context retains its own draft and reading position. Entering Implement
preserves Discussion's history, draft and reading position, but disables returning
to it during planning and construction, including pauses between steps. The context
selector also marks Discussion unavailable rather than reopening it. Once every step is built and
validated, returning to Discussion explicitly closes implementation through the
daemon. Context, mode, original workspace and execution workspace remain labelled.

Objective Plans keep normal Markdown body colors, with a muted title and separator
that turn green for updated plans, as in the TUI. There is no side stripe or inline
implementation button. `:SosImplement` and `<Plug>(SosImplement)`
use only the Objective Plan attached to the last assistant message in the latest
Discussion, not the newest published version or an older message under the cursor.
The popup offers `Implementation plan` and `Implement (Auto plan)`; automatic
planning then asks for Step by step / Everything. Canceling either popup sends
nothing. Missing plans produce `An Objective Plan is required in the last assistant message`.
After manual planning finishes, use the same `:SosImplement` action again to validate
the implementation plan: it opens Step by step / Everything directly, and selecting
a pace accepts the plan and starts construction after daemon acknowledgement.
The automatic path chooses its pace before generation and needs no second validation.
The final step's file import remains the separate `:SosValidate` action; plan
validation does not import files.
Busy turns, previews, archives and older/loading history cannot launch implementation;
changing the Discussion while a popup is open invalidates its selection. Existing
branches reopen without regenerating their plan or restarting execution. There is
no default physical key; use `setup({mappings={Implement='<leader>si'}})` if desired.

`:SosPlan` selects a historical Objective Plan to read or open its planning branch.
`:SosGeneratePlan` explicitly regenerates a manual plan. `:SosImplementationPlan`
reads the finalized/accepted plan; `:SosStart` remains an explicit alias for manual
plan validation through Step by step / Everything.
`:SosAutoPlan` asks for the objective and pace before automatic planning;
`:SosRetryPlan` retries its saved pace after a failure or cancellation. Archives are
read-only and remain reachable through the context selector's branch list.

When construction starts after daemon acknowledgement, the chat switches to the
Implement review history, initially empty. Discussion and planning histories are
retained separately; construction messages never enter the review transcript.
The input stays visible but cannot be edited until an eligible step is selected.
During construction it shows a spinner and:

```text
Building step 1 |
:SosSteps to select a step to review
```

`:SosSteps` opens a floating selector containing only steps. Moving the selection
updates a read-only frame on its left with that step's numbered substeps and
preserved explanations, without changing the open inspection or review context.
Ctrl-D / Ctrl-U scroll the preview while the selection stays on the step list.
Enter selects a step, switches to its
independent review history, and opens its preserved code and explanation. The
window used to invoke the command determines placement: from the transcript or
input, inspection opens in an additional vertical split; from another ordinary
editor window, inspection replaces that window's displayed buffer. The original
buffer stays loaded, including unsaved edits. Closing inspection restores its
buffer, reading position and window options. Destructive `bufhidden` policies
use a separate split instead of risking the original buffer. Esc or q cancels the
selector without opening code. Selection uses the original window, not the popup.

Use `:SosVersions`, `:SosImplementationPlan` and `:SosProposal` for their separate
views; Conclusion opens after final validation. Live updates preserve the selector's
cursor and focus. It is opened only on request, not as a permanent
sidebar. Selected code and review drafts remain stable while other steps build.
Review input becomes editable only on the first built, unvalidated step; future,
interrupted, superseded and archived results remain inspectable but read-only.
An upper-left nonfocusable progress indicator appears only when the chat is hidden;
set `progress = false` to disable it. `:SosContinue` confirms resuming an
interrupted build; `:SosCancel` confirms interrupting construction, or cancels the
selected review independently. These controls have matching `<Plug>` actions and
slash forms `/context`, `/plan`, `/generate_plan`, `/implementation_plan`, `/start`,
`/auto_plan`, `/retry_plan`, `/steps` and `/continue`. Review selection and questions
do not validate or import changes.

## Review and import

`:SosReview` opens the eligible selected result's independent review conversation.
When invoked from inspection, it retains the real-substep and file-local hunk
context. Opening review keeps the inspected code and explanation visible beside the chat.
Questions remain read-only; asking for a correction creates a proposal, not an edit.
When a correction supersedes a result, its old review becomes a labelled read-only
view with its draft intact. Select the corrected step and use `:SosReview` to open
the new generation's independent conversation. `:SosIndicator` toggles only a selected real-substep review indicator.
It never imports files. `:SosValidate` is the separate final step action: it always
confirms importing into the original workspace, including how many unchecked
indicators will be accepted. No selection or review advancement is assumed until
the daemon confirms the current revision. Import conflicts and stale revisions
retain the selection and evidence; read the error before retrying.
While an import is pending, the review input displays Applying changes and prevents
double submission. Only its correlated successful acknowledgement selects the
successor's review context and construction status. After the
last human validation, Conclusion opens with an explicit Close implementation
action selected. Enter on Close returns to the preserved Discussion and its draft;
completion or inspection alone never closes or imports anything.

`:SosProposal` reads a pending correction or revised-plan proposal.
`:SosAcceptProposal` confirms correction execution, or first opens the independent
revised-plan discussion when the proposal changes the plan. `:SosRevision` returns
to that discussion, with its candidate plan and independent draft. Acceptance
there requires a second explicit confirmation before executing the revised plan.
`:SosRejectProposal` confirms dismissal and resumption of paused work.
Review and proposal views also expose Enter-selectable Accept/Decline actions.
Acknowledged correction preparation opens the corrected step's review context;
prior versions remain separately inspectable.
`:SosCancel` in either review discussion cancels that response, not construction.
Preparation failures remain visible and can be retried explicitly; interrupted
construction resumes through `:SosContinue`. Acceptance uses the current revision
token and never silently retries an uncertain operation.

`:SosVersions` lists current and preserved correction-archive versions. Archives
retain their accepted plan, conclusion and immutable inspection; drafts, selection
and navigation stay separate from the current implementation. Archive views never
offer indicators, validation, proposal acceptance or editing. Choosing Current
restores live state; Discussion remains unavailable until implementation is finished.
Native `<Plug>(SosReview)`, `<Plug>(SosIndicator)`,
`<Plug>(SosValidate)`, `<Plug>(SosProposal)`, `<Plug>(SosAcceptProposal)`,
`<Plug>(SosRejectProposal)`, `<Plug>(SosRevision)` and `<Plug>(SosVersions)` have no
default mappings. Editable composers also support `/inspect`, `/review`,
`/indicator`, `/validate`, `/proposal`, `/accept_proposal`, `/reject_proposal`,
`/revision` and `/versions`. Read-only views use Ex commands rather than input.

Selecting a step in `:SosSteps`, or using `:SosInspect`, opens native inspection
using the originating window rules above. It restores the last substep/diff, or
the first changed substep initially. Incomplete or empty boundaries show No changes.
No second substep picker, quickfix or location list is required.
`:SosNextSubstep` / `:SosPreviousSubstep` navigate real boundaries and the explicitly
labelled Whole step view. `:SosNextChange` / `:SosPreviousChange` navigate individual
hunks, including multiple hunks in one file, and stop at the current boundary's
edges without wrapping. Empty substeps retain their explanation and `No changes`.
Positions are remembered separately for each result revision and substep, including
the selected file/hunk when new changes arrive. `:SosNextDiff` / `:SosPreviousDiff`
are aliases of the Change commands, with matching `<Plug>` actions.
Matching `<Plug>` actions are available. Inside owned inspection buffers, use
`]s` / `[s` for substeps, `]c` / `[c` for changes, `gx` to collapse/reopen the
explanation and `q` to close inspection. `:SosExplanations` toggles the explanation
panel; `:SosExplanation` remains an alias.
Normal motion, search and window commands remain native; focus is not moved on updates.
Explicit next/previous actions focus the corresponding code location without
reopening the chat or a selector. During review they also update the question's
substep/file/hunk context, retaining each substep's draft without sending anything.
An independent two-line header immediately above the code keeps the counters,
current substep title and filename visible even with a global statusline:

```text
Step 2/5 | Substep 1/3 | Diff 2/4 | Store metadata
src/metadata.py
```

Changes follow preserved `hunk_order`. The upper panel is titled only `Explanation`
and contains only the selected hunk's `hunk_explanations`, without repeated titles,
counters, summaries, indicators or shortcut hints. Older results without hunk text
show `No explanation available for this change.` Empty substeps without a diff
retain their preserved explanation. Step/substep summaries are in the selector preview.

Code is always shown in the read-only, unlisted `sos-inspection` scratch buffer,
with `buftype=nofile` and `filetype=sos_inspection`, never a normal file buffer.
It contains the preserved result even when the isolated file still matches it;
successor writes and native reloads cannot replace reviewed evidence. Inspection
does not open a real file or claim its path. Modelines and automatic reload are disabled.
Language styling is separate from the internal filetype: an available Tree-sitter
parser uses the user's language mappings, runtime queries and highlight groups,
with Vim runtime syntax as fallback. Existing `nvim-treesitter` highlight enable,
disable and additional-regex settings are respected. SOS does not impose code colors.
Language `FileType` and file-read hooks do not run for inspection, so language
ftplugins and ordinary LSP attachment are not triggered. Custom styling that exists
only in a language `FileType` callback is not replayed; use runtime syntax, queries
or highlight groups for styling shared with inspection.

Deleted lines appear as virtual lines above
their original replacement location, without changing the file. `gb` or
`:SosBefore` toggles the complete preserved before/after image; deleted files show
before-text and binary changes show a marker rather than invented source.
Inspection never validates, imports or edits files. Read-only buffer flags are an
editor safeguard, not a filesystem security boundary.

Inspection buffers do not attach language servers as if they were live files.
Search, tags and language-server roots are not silently
redirected; language-server behavior still depends on the user's configuration.
No file-tree integration is driven. Hiding the conversation leaves inspection
windows open, and navigation commands do not force the chat to reopen. `q` in an
inspection buffer closes inspection; `:SosInspect` restores the retained selection.
A window repurposed to a user buffer is left alone.

Discussion and planning submissions immediately show a local user turn with a
running Activity, including while the daemon starts or checks the request. The
daemon's accepted user message replaces that provisional identity without a
second message or a timer restart. Rejection removes the provisional turn and
restores its text before any newer draft.

Sending during a Discussion or planning turn appends to one local `[Pending]`
message below the response and clears the composer. Multiple submissions are
joined with newlines and sent as the next turn after successful completion;
newer composer text stays untouched. Cancellation, failure or disconnect restores
unsent text to the draft, and reconnect never automatically resends it. Pending
text is not part of persisted session history and is hidden on older pages.
Review and revision requests retain their separate workflow controls.

`:SosOlder`, `:SosNewer` and `:SosLatest` navigate bounded daemon history pages;
matching `<Plug>` actions are available. Live events do not move an older page.
Sending from older history first reconciles the latest page, preserving the draft
if that read fails. Activity groups, nested tool/reasoning details and historical
Objective Plans use native folds (`zo`/`zc`, `za`, `zR`/`zM`). Opening a group loads
only its lightweight rows; opening one nested detail loads only that detail.
Successful historical read bodies are not fetched. Loading/error placeholders
stay in the fold; close and reopen to retry a failed load. Submitting purges old
Activity details and closes their folds without rewriting stored history.
Retained records preserve open folds and reading anchors across page updates.

Configure provider credentials with `/connect` in the existing SOS TUI first.
The plugin reuses daemon authentication and never accepts credentials in chat.

An existing daemon needs no Python. Automatic startup uses Python 3's standard
library for the same per-socket `flock` as Textual; no third-party Python package
is required. It never removes a regular file at the socket path. It uses the
launching editor's environment, XDG paths and existing daemon credentials.
Configure `daemon` explicitly to override discovery. Automatic installation uses
its bundled `bin/sosd` first, then `sosd` on PATH and the installed TUI daemon.
Development lookup also checks `build-ai/bin/sosd` and the TUI development copy.
The Lua client itself never builds or installs a daemon. Close old integrations
before replacing an incompatible daemon. Logs use `$XDG_STATE_HOME/sos/sosd.log`
with the standard home-directory fallback.

Connection loss never causes automatic reconnection or request replay.
`:SosReconnect` explicitly reconnects and, if a session ID is retained, reattaches
and refreshes implementation state. Reattachment can cancel/drain active work
as defined by the daemon. Draft data remains local and uncertain operations are
retained for inspection, not retried. Attachment restores the bounded transcript;
Activity bodies remain lazy. The Lua client exposes `on_event`, `request(frame, terminal_type, callback)`
and separate `conversation`, `uncertain` and transport state. A request's terminal
type is explicit because correlated intermediate events need not mean completion.

## Development loading

To load directly from `integrations/neovim` instead of the automatically installed
copy, disable installation with `SOS_INSTALL_NEOVIM=OFF`, remove any previously
installed copy, and add that absolute source directory to `runtimepath` before
calling `require('sos').setup`. Otherwise, the default CMake build copies the Lua
files into the development installation described above. Restart Neovim after Lua
changes or rebuilding the installed copy. When adding the path to an already running editor, also execute
`:runtime plugin/sos.lua` to register commands.

For a development daemon, configure `daemon` with the absolute path to
`build-ai/bin/sosd`, or connect to an already running compatible daemon. Building
does not replace a daemon already listening on the configured socket.

## Automated checks

Run the foundation checks from the repository root:

```sh
nvim --headless -u NONE -l integrations/neovim/tests/run.lua
```

Tests use local Unix sockets and controlled peers, with artifacts in
`build-ai/neovim/`. Only transient Unix-socket endpoints and their startup locks
use private, uniquely allocated `/tmp/sos-nvim-*` directories, because macOS limits
the entire socket path, not just its filename. Scoped cleanup removes those
directories on success and failure after controlled daemon shutdown. Logs, fixture
executables, disposable projects and isolated XDG data remain under `build-ai/`.
No provider credentials or running production daemon are used.
With `build-ai/bin/sosd` built, run `python3 integrations/neovim/tests/smoke.py`
for a real-daemon session smoke check with an isolated local HTTP provider.
Run `python3 integrations/neovim/tests/smoke.py --lifecycle` for the controlled
planning/construction/inspection/review/correction/import scenario. Its disposable
repository checks that indicator toggles do not import, original files change only
after explicit final validation, and its pre-existing staged index stays intact.
It also checks navigation without import, preserved archive text after correction,
and native refresh of the original file buffer after confirmed import.
The session smoke loads the public plugin with a clean Neovim configuration and
uses `:SosConnect`, `:SosToggle` and `:SosSend`. It exercises real write/edit/patch
results and shell completion with loaded clean and dirty file buffers.
`tests/file_refresh.lua` also attaches a UI to verify that the native write-time
confirmation still protects a dirty buffer and that reloads preserve Insert mode.
These are headless integration checks, not a human assessment of interactive
layout, terminal rendering or mappings.
Each smoke run retains diagnostics under its printed or reported
`build-ai/neovim/{real,lifecycle}/run-*` directory, including `daemon.log`.

## Conversation appearance

The transcript uses the editor's normal text and background, without role headings.
Chat offsets follow the TUI's `app.py` and `turn_activity.py`, measured from the
left edge of the transcript: assistant text starts at column offset 1, the user
band at 1 and user text at 3, Activity arrows at 3, tool arrows at 5, successful
read labels at 7 and expanded details at 9. There is no extra fold/sign gutter
shifting the entire transcript. User bands fill one whole terminal cell (`█`),
green for Sane and pink for Slop, retaining the mode recorded when sent and
repeating alongside wrapped lines. Assistant messages have no role color or band.
Only subsequent user turns introduce an extra blank row; Activities and text do
not each add their own trailing spacer. Preserved model/reasoning/duration metadata
appears in a muted, right-aligned generation footer when available.
Like the TUI, separators precede subsequent Activities within one user turn and
Objective Plan drawers, not every user message. Rules follow the panel width.
Markdown keeps structural emphasis without a custom prose palette or background.
Common Markdown delimiters are removed in the display
projection, never in stored messages. HTTP(S) links open with `gx` under the link.
This is a lightweight built-in renderer rather than a complete CommonMark engine;
nested/complex Markdown may retain source formatting. An Activity remains visible
even without tools or reasoning, including for plain answers; expanding an empty
Activity never requests a nonexistent historical payload.
Activity headers use the public grey `SosActivity` group and
the TUI's colored type indicators (tools, reasoning, reads, skills, refusals),
step count, elapsed time and aggregate model throughput (`tok/s`). The colored
squares are plugin-provided type indicators, not signs from your editor config.
Live elapsed time advances between daemon events and freezes on completion,
failure, cancellation or disconnection. Saved timing and model statistics are used
when reopening history. Throughput is total output tokens over total model-call
seconds, excluding tool time; unknown usage, unsafe arithmetic or zero model time
hides the rate rather than estimating it. Both open and closed folds retain these styles
and offsets. Tool titles fit a single row and use the TUI's tool/status/hint order.
Tool bodies follow `tool_view.py`: `glob`/`grep` show the bare path and pattern,
then a rule and results, without invented `Path`, `Pattern` or `Result` headings.
Shell calls show `command`, `timeout` and optional `workdir`, a rule, exit status
and output. Tool titles, arguments, paths and outcomes retain the TUI's semantic
colors without recoloring ordinary assistant text or the composer.

Objective Plan bodies and stacks retain normal Markdown colors. Only updated
titles and separators use the green accent; other titles are muted, as in the TUI.
There is no gutter stripe or custom background. Their folds are read-only:
Enter only expands/collapses them.
Use `:SosImplement` for the last message, or `:SosPlan` to browse historical versions.
The floating step selector uses semantic state colors and blue selection.

Public highlight groups are `SosNormal`, `SosDraft`, `SosUser`,
`SosActivity`, `SosDetail`, `SosPlan`, `SosSeparator`, `SosNotice`, `SosHeading`,
`SosBold`, `SosCode`, `SosSane` and `SosSlop`. Defaults are restored on ColorScheme
without overwriting groups defined by your theme. For example:

```lua
vim.api.nvim_set_hl(0, 'SosSane', { fg = '#05b07d' })
vim.api.nvim_set_hl(0, 'SosSeparator', { link = 'Comment' })
```

Additional semantic groups are `SosMuted`, `SosPath`, `SosLink`, `SosEmphasis`,
`SosQuote`, `SosCodeTitle`, `SosObjectivePlan`, `SosObjectivePlanUpdated`, `SosPlanBody`, `SosValidated`, `SosError`,
`SosSelection`, `SosAdded`, `SosRemoved`, `SosTool`, `SosReasoning`, `SosRead`,
`SosSkill`, `SosArgument`, `SosAction`, `SosUnnecessary` and `SosValidationSweep`.
Code-inspection annotations use native
`DiffAdd`/`DiffDelete` groups, leaving their colors to the editor theme.
Composer badges use `SosModeSane` and `SosModeSlop`, independently of transcript bands.

In the transcript's Normal mode, Enter or `za` toggles a fold; `zc` closes it,
`zo` opens it and `zM` closes all. Disclosure arrows are part of the displayed
header rather than a separate fold column.
Short Activities remain foldable regardless of inherited `foldminlines`.
Opening a live tool does not fetch already-received details again. Historical
Activity details are still loaded only on explicit expansion.

The header identifies pending permission decisions. Dismissing the selection
leaves permission pending; `:SosPermissions` reopens it even when the composer is read-only. Deny remains the safe first
choice and cancels execution, while Allow once explicitly grants the request.
A locally submitted denial is retained in the cancellation notice. An otherwise
unexplained daemon cancellation is not attributed to a permission decision.

## Troubleshooting

- Connection/startup failure: verify the configured executable and socket, inspect
  the daemon log, then use `:SosReconnect` explicitly. Never remove an occupied
  non-socket path automatically. Request uncertainty is not success or permission
  to repeat an import.
- Socket path too long: configure a shorter absolute path (at most 103 bytes on
  macOS, 107 on Linux, including the directory). UTF-8 names count in bytes.
  The plugin refuses before startup; it never truncates or silently relocates
  a configured endpoint. The daemon must listen at the same configured path.
- Authentication/model failure: configure a provider with TUI `/connect`; never
  paste credentials into a composer. Then select the model with `/model`.
- Frozen composer: inspect the persistent context label. Use `:SosContext` or
  `:SosVersions` to leave a preview/archive; an active implementation keeps its
  parent Discussion frozen until the daemon permits closing it.
- Proposal or import refusal: read the preserved error, refresh via `:SosSteps`,
  inspect the current evidence and retry explicitly. Resolve original-workspace
  conflicts outside the read-only inspection buffers without discarding user work.
- Inspection shows preserved code, not a live file. Search/LSP still use your
  original editor project and may not describe the preserved result version.
