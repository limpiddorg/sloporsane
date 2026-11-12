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
draft, and `:SosToggle` to hide/show the vertical conversation split. Hiding does
not cancel generation. `:SosSend` explicitly sends the multiline composer;
`:SosCancel` requests cancellation and `:SosMode` toggles Sane/Slop only after
daemon acknowledgement. Switches that cancel active work require confirmation.
The transcript is protected; edit the bottom composer in Insert mode and read
in Normal mode. Drafts remain in memory until editor exit, including drafts from
previous sessions. Existing file windows are left intact.

Normal-mode `<Plug>(SosNew)`, `<Plug>(SosResume)`, `<Plug>(SosToggle)`,
`<Plug>(SosSend)`, `<Plug>(SosCancel)` and `<Plug>(SosMode)` actions have no default
user shortcuts. Optional setup `mappings = {Toggle = '<leader>ss', Send = '<leader>se'}`
installs only unused Normal-mode mappings. Loading creates no connections or
windows and does not change cwd. Never use the human `build/` for verification.

Supported composer slash commands (no arguments) are `/help`, `/new`, `/resume`,
`/mode`, `/cancel`, `/model`, `/reasoning` and `/permissions`. Use native omni
completion (`<C-x><C-o>`) in the composer. `/model` lists authenticated catalogs,
then asks for a context budget; `/reasoning` cycles daemon-supported levels.
Live permission requests show details in the transcript and ask for Deny, Allow
once, or Always for the displayed scopes when available. Dismissal of the prompt
does not grant permission; `/permissions` reopens it. Unsupported slash commands
are rejected locally and remain in the draft.

`:SosContext` selects Discussion, implementation planning or eligible whole-step
review. Each context retains its own draft and reading position. Discussion is a
frozen parent-history preview during construction, including pauses between steps;
previewing never attaches away or cancels work. Once every step is built and
validated, returning to Discussion explicitly closes implementation through the
daemon. Context, mode, original workspace and execution workspace remain labelled.

`:SosPlan` selects a historical Objective Plan to read or open its planning branch.
`:SosGeneratePlan` explicitly regenerates a manual plan. `:SosImplementationPlan`
reads the finalized/accepted plan; `:SosStart` asks for Step by step or Everything.
`:SosAutoPlan` asks for the objective and pace before automatic planning;
`:SosRetryPlan` retries its saved pace after a failure or cancellation. Archives are
read-only and remain reachable through the context selector's branch list.

`:SosSteps` opens a temporary floating status selector. Enter reads a step summary,
the accepted plan or the conclusion; Esc closes it. Live updates retain its selected
stable identity without stealing focus. The compact transcript header distinguishes
preparation, building, technical validation, waiting, interruption and completion
using daemon state, never tool termination. `:SosContinue` confirms resuming an
interrupted build; `:SosCancel` confirms interrupting construction, or cancels the
selected review independently. These controls have matching `<Plug>` actions and
slash forms `/context`, `/plan`, `/generate_plan`, `/implementation_plan`, `/start`,
`/auto_plan`, `/retry_plan`, `/steps` and `/continue`. Review selection and questions
do not validate or import changes.

## Review and import

`:SosReview` opens the eligible selected result's independent review conversation.
When invoked from inspection, it retains the real-substep and file-local hunk
context. Questions remain read-only; asking for a correction creates a proposal,
not an edit. `:SosIndicator` toggles only a selected real-substep review indicator.
It never imports files. `:SosValidate` is the separate final step action: it always
confirms importing into the original workspace, including how many unchecked
indicators will be accepted. No selection or review advancement is assumed until
the daemon confirms the current revision. Import conflicts and stale revisions
retain the selection and evidence; read the error before retrying.

`:SosProposal` reads a pending correction or revised-plan proposal.
`:SosAcceptProposal` confirms correction execution, or first opens the independent
revised-plan discussion when the proposal changes the plan. `:SosRevision` returns
to that discussion, with its candidate plan and independent draft. Acceptance
there requires a second explicit confirmation before executing the revised plan.
`:SosRejectProposal` confirms dismissal and resumption of paused work.
`:SosCancel` in either review discussion cancels that response, not construction.
Preparation failures remain visible and can be retried explicitly; interrupted
construction resumes through `:SosContinue`. Acceptance uses the current revision
token and never silently retries an uncertain operation.

`:SosVersions` lists current and preserved correction-archive versions. Archives
retain their accepted plan, conclusion and immutable inspection; drafts, selection
and navigation stay separate from the current implementation. Archive views never
offer indicators, validation, proposal acceptance or editing. Choosing Current
restores live state; returning to Discussion follows the normal frozen-preview or
completed-close rules. Native `<Plug>(SosReview)`, `<Plug>(SosIndicator)`,
`<Plug>(SosValidate)`, `<Plug>(SosProposal)`, `<Plug>(SosAcceptProposal)`,
`<Plug>(SosRejectProposal)`, `<Plug>(SosRevision)` and `<Plug>(SosVersions)` have no
default mappings. Editable composers also support `/inspect`, `/review`,
`/indicator`, `/validate`, `/proposal`, `/accept_proposal`, `/reject_proposal`,
`/revision` and `/versions`. Read-only views use Ex commands rather than input.

Selecting a step with Enter in `:SosSteps`, or using `:SosInspect`, opens dedicated
ordinary code and explanation windows without replacing existing project windows.
`:SosNextSubstep` / `:SosPreviousSubstep` navigate real boundaries and the explicitly
labelled Whole step view. `:SosNextChange` / `:SosPreviousChange` navigate individual
hunks, including multiple hunks in one file, and stop at the current boundary's
edges without wrapping. Empty substeps retain their explanation and `No changes`.
Positions are remembered separately for each result revision and substep.
Matching `<Plug>` actions are available. Inside owned inspection buffers, use
`]s` / `[s` for substeps, `]c` / `[c` for changes, `gx` to collapse/reopen the
explanation and `q` to close inspection. `:SosExplanation` also toggles explanations.
Normal motion, search and window commands remain native; focus is not moved on updates.

Inspection buffers are owned, readonly/nonmodifiable snapshots of the daemon's
preserved text, never reused user file buffers. `Live verified · Isolated` means
the isolated file exactly matched that preserved after-text and no existing editor
buffer claimed its path. Disk changes are rechecked on editor events, state updates
and a short visible-window timer; a successor's contents never replace old evidence.
Differences, missing files, occupied buffers and archives use explicitly labelled
Historical contents. Deletions show preserved before-text; binary changes show a
marker rather than invented text. Highlights and local counters refer to the
selected preserved hunk. Inspection never validates, imports or edits files.

Both source modes use scratch buffers with syntax detection, not editable file
buffers. Automatic LSP attachment is not guaranteed; workspace-relative search,
tags and language servers still use the editor's existing project configuration,
not a silently changed cwd or a historical filesystem. No file-tree integration
is driven. Hiding the SOS panel also closes owned inspection windows; `:SosInspect`
reopens the retained selection. A window repurposed to a user buffer is left alone.

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

The plugin runs directly from `integrations/neovim`; CMake does not install or
copy its Lua files. Add that absolute directory to `runtimepath` in your Neovim
configuration before calling `require('sos').setup`. Restart Neovim after Lua
changes. When adding the path to an already running editor, also execute
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
It also checks navigation without import and preserved archive text after correction.
The session smoke loads the public plugin with a clean Neovim configuration and
uses `:SosConnect`, `:SosToggle` and `:SosSend`. These are headless integration
checks, not a human assessment of interactive layout, terminal rendering or mappings.
Each smoke run retains diagnostics under its printed or reported
`build-ai/neovim/{real,lifecycle}/run-*` directory, including `daemon.log`.

## Conversation appearance

The transcript uses the SOS palette by default, scoped to its own windows (including
256-color fallbacks). Turn separators, role headings, framed Activity headers,
mode labels and basic Markdown headings, emphasis and code have semantic colors.
This is lightweight Markdown highlighting, not a full Markdown renderer; source
markers remain visible. Empty Activity placeholders are not displayed.

Public highlight groups are `SosNormal`, `SosDraft`, `SosUser`, `SosAssistant`,
`SosActivity`, `SosDetail`, `SosPlan`, `SosSeparator`, `SosNotice`, `SosHeading`,
`SosBold`, `SosCode`, `SosSane` and `SosSlop`. Defaults are restored on ColorScheme
without overwriting groups defined by your theme. For example:

```lua
vim.api.nvim_set_hl(0, 'SosUser', { link = 'Title' })
vim.api.nvim_set_hl(0, 'SosNormal', { link = 'Normal' })
```

In the transcript's Normal mode, Enter or `za` toggles a fold; `zc` closes it,
`zo` opens it and `zM` closes all. The narrow fold column shows disclosure arrows;
clicking them uses Neovim's own mouse folding if you have mouse support enabled.
Short Activities remain foldable regardless of inherited `foldminlines`.
Opening a live tool does not fetch already-received details again. Historical
Activity details are still loaded only on explicit expansion.

The header identifies pending permission decisions. Dismissing the selection
leaves permission pending; `/permissions` reopens it. Deny remains the safe first
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
- Historical labels after successor edits are expected. Search/LSP still use your
  original editor project and may not describe the preserved result version.
