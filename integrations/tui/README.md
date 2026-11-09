# TUI

Textual TUI with code in `src/sos_tui/`. Development uses uv; the CMake build
produces a standalone application installed as `sos`.

From `integrations/tui/`:

```sh
uv sync
uv run tui
```

Application text and generated explanations are in English, even for requests in
another language. This includes plans, step/substep and diff explanations,
progress, review proposals, recaps, summaries, session titles and conclusions.
Existing stored content is not translated or rewritten.

Scrollbars across the TUI share dark hover/drag styling: thumb `#5A6672`,
background `#131616`. Idle colors are preserved.

### SOS skills

Skills from `.sos/skills/<name>/SKILL.md` and `$XDG_CONFIG_HOME/sos/skills/`
(default `~/.config/sos/skills/`) appear in `/` suggestions. Each row shows the
name followed by its literal `argument-hint`, if present:
`/check-api [directory] [note]`. The `workspace` or `user` origin stays in the
help card; workspace wins on name collision. Completion inserts only the name,
not the hint. Tab, Shift+Tab, arrows or clicking changes selection.

The help card occupies half the screen: name without hint, description, then
Scripts, References and Assets with sorted recursive relative paths, without
reading their contents. Empty categories are hidden; listing errors remain
visible. License follows the title in muted color, origin stays right-aligned.
Compatibility appears below the title in muted color; Metadata ends the card,
one `key: value` pair per line. Missing fields and empty Metadata are hidden.
Optional `argument-descriptions` (name → description mapping) restores a numbered
Arguments section before Scripts, in `arguments` order; described labels without
aliases follow in catalog order. Without descriptions, the section is hidden.
No required/optional marks or value validation.

The hint is visual help, never validated or sent. The composer shows it in gray
until argument text begins, only for the initial skill chain. Once text is typed,
it disappears entirely; it does not calculate remaining arguments. Without a
declared hint, none is invented. Its text is displayed literally, including angle
and square brackets: punctuation is the skill author's choice, not stripped by
TUI.

The TUI sends final raw text without argument binding or splitting. The old SOS
schema (`description`, `required`, `{name}` and named invocation object) is
removed. `arguments: [directory, note]` or `arguments: directory note` defines
only positional aliases `$directory` and `$note` in declared order. The loader
also substitutes `$ARGUMENTS` (full text), `$0`, `$1` and `$ARGUMENTS[N]`
(shell-style quoted tokens, without execution). Use `$ARGUMENTS` for an entire
unquoted sentence. Missing indices stay literal; missing declared aliases become
empty. If no placeholder receives text, `ARGUMENTS: ...` is appended. Inserted
values are not reinterpreted. Missing/extra arguments do not block sending; an
old SOS mapping is rejected with a migration diagnostic. Personal skills are not
automatically rewritten. Extra frontmatter fields are ignored without error:
they activate no hooks, subagents, permissions or preapproved tools.

Native commands and skills share `/`; native names are reserved. Conflicting
skills are excluded from suggestions and reported without blocking the native
command. Completion replaces only the cursor's word anywhere in the message.
A prose reference remains free text when sent. Up to six consecutive leading
skills load in order: `/caveman /check-api src/http` passes `src/http` to each.
The first unknown word, or seventh skill, and the rest become shared text.
All load before generation: a load error rejects the whole request and retains
the draft. Each accepted skill has its own Activity. During an active turn,
invocation joins the message queue.

Loading supplies instructions, location, raw text and script paths without
reading or executing scripts. Ordinary tools remain subject to permissions and
Sane/Slop. See [the format and AGENTS.md loading](../../docs/skills.md).

### `/btw` conversations

During a main turn, `/btw Why this choice?` creates a parallel informational
conversation named `btw-1`, then `btw-2`, etc. Its panel opens between the main
user message and Activity. Its arrow aligns with Activity; content is indented
two more spaces and bounded by two MISC rules. Header, user stripe and identifier
use MISC lagoon blue. The identifier precedes model, reasoning and duration at
the bottom right of each response, allowing `/btw btw-1 Explain this point`.
After each accepted BTW send, the empty composer is prefilled with `/btw btw-N `,
cursor at end. Removing this prefix returns to the main thread. A draft already
typed while awaiting acceptance is not overwritten.

Creation and continuation also work after the main turn ends. A branch created
while idle appears after the thread, with its arrow aligned with ordinary
messages. It retains frozen main context, but its BTW system instruction does not
announce an ongoing main task. Position and this distinction survive `/resume`.
An empty main conversation cannot create BTW. Sending to a busy branch is
rejected while preserving the draft. On a branch's first send, the main user
message is placed at chat top to show context; for an idle-created branch, its
BTW panel header is used instead. Later sends place the branch's last user
message at top. Received answers do not force that position after manual scrolling.

A new branch collapses other panels; a new main message collapses all, without
stopping generation. Double Escape cancels only the main turn. There is no manual
BTW cancellation in the app; closing/disconnecting the TUI interrupts its branches
and saves received results.

Each branch keeps a frozen copy of main active context at creation, including
images, plus its own history. Nothing is injected into the main thread. Model
and reasoning are those selected at each exchange's launch. The daemon permits
only `read`, `glob`, `grep`, `view_image` and `webfetch`, never Bash or writing,
even in Slop/Danger. Reads outside the authorized workspace are refused.
Entered BTW messages are text-only.

Context, messages, tools, metadata and branch state persist in `session.sqlite`
(schema 5). `/resume` restores panels and allows continuation; interrupted
generations do not restart automatically. BTW context is not automatically
compacted in this version: exceeding the window is reported in the branch.
Protocol moves to **7**: rebuild TUI and daemon together, then close old integrations.

### Provider connections

On first launch, use `/connect`: OpenRouter with API key; OpenAI with API key,
ChatGPT/Codex browser login or device code. `Open browser` opens the authorization
URL and remains active during OAuth polling. The URL is a colored, underlined,
clickable link, also keyboard-accessible through Tab then Enter; it remains
visible for opening on another machine. Browser connection uses a local callback
on port 1455; device code does not require it. Escape or Cancel from method/key
steps cancels the current operation, clears fields and returns to providers.
Escape or Cancel from the provider list closes the popup.

Entered keys are masked and saved by the daemon in `$XDG_DATA_HOME/sos/auth.json`
(default `~/.local/share/sos/auth.json`), permissions `0600`, outside conversations.
The file contains plaintext keys/tokens. OAuth tokens refresh automatically.
After authentication, the first compatible model from the last successfully
connected provider is selected: OpenRouter requests `sort=newest`, OpenAI API
sorts by descending `created`, and Codex by ascending `priority`, like the official
client. Selection is deterministic and remembered between sessions. The popup has
no model field; the chosen model appears in the information bar. `/connect`
requests new authentication, replacing the provider's saved authentication.
OpenRouter opens key entry directly without method selection, even if connected.

The popup shows `Checking API key…` before saving: OpenRouter uses
`GET /api/v1/key`, OpenAI uses `GET /v1/models`. No generation call is made.
Refusal, network error or cancellation retains the old key. This validates
authentication and endpoint access, not available credit or all-model access.
Authentication uses only `auth.json`, with no environment-key fallback.

Multiple providers may be connected simultaneously, with one saved authentication
per provider. Reconnection fully replaces only that provider's authentication.
`Connected` appears on each connected provider and its active method row when
methods are offered, never on headings or key input. OpenRouter shows it only on
the provider. The last method is remembered/preselected without starting login.
Old OAuth entries without a precise method mark only the provider; reconnecting
identifies browser/device rows without guessing.

Shift+Tab or ← from methods returns to providers. On buttons, ←/→ traverses
visible enabled actions; in key input, it moves the cursor. Input border stays
muted, even focused. OpenRouter catalog requires text input/output and tools.
Codex uses its dedicated catalog and visible models. OpenAI API `/models` does
not supply capabilities: filtering retains known Responses families and their
snapshots, excluding audio, embeddings and chat-only aliases. Catalog failure
retains the current conversation and permits retry.

Confirming a provider starts a new conversation; the old one remains in `/resume`.
Provider/model/reasoning selection is saved in `$XDG_CONFIG_HOME/sos/config.json`
(default `~/.config/sos/config.json`), separate from credentials. Sessions retain
historical metadata but resume with the current global selection.

The TUI connects to `/tmp/sosd.sock` or starts its bundled daemon if none responds.
`--socket /another/path.sock` chooses another socket. No session exists before
the first message. The daemon is shared while any integration remains connected.
The TUI announces closure; after the last integration leaves and pending saves
finish, the daemon exits and removes its socket. It also pings every 15 seconds
and closes connections without pong within 30 seconds. Responses are automatic,
even during `$EDITOR`. It inherits its launcher's environment; a key change
requires restarting it. Close all integrations and relaunch `sos`. Simultaneous
starts are serialized by a lock. Logs are in `$XDG_STATE_HOME/sos/sosd.log` or
`~/.local/state/sos/sosd.log`. For `uv run tui`, build `build-tui` first to provide
the package's development copy of `sosd`.

### Discussion and scrolling

The Discussion tab occupies a top row. The last row shows workspace on the left
and daemon-supplied title on the right; before session creation, title is `-`.
The daemon generates an LLM title of at most six words from the first message,
returned with `session.created`. The TUI displays each submitted message and its
animated Activity immediately, without waiting for daemon acceptance, including
the first during title preparation. Message echo confirms this display without
duplication or timer restart. Failure, cancellation or disconnection before
acceptance removes the provisional display and restores text before any new
composer draft. The palette is in `src/sos_tui/config.py`.

On send, the question aligns at chat top if it fits; otherwise the view follows
its end. Previous exchanges remain accessible by scrolling up. The last turn has
at least the chat's height: if it fits, scroll stops with question at top and empty
space below; if longer, scrolling reaches the content bottom, including expanded
Activities. Space adapts to visible content and terminal size. Every assistant
text, intermediate or final, triggers following if the user was waiting at the
bottom. Activity does not move the view, but its growth does not break that wait:
the next text reaches the new bottom. Scrolling up stops following, including
during response mounting; returning to bottom resumes it. Evicting old widgets
preserves reading position while still within the window.

### Pending messages and retries

During a turn, Enter puts text into one local next message below the whole active
assistant block, with light-blue `[Pending]` (`MISC_MD`) above it. New text,
Activity and permissions insert before this block. Further submissions append to
the same message with newlines and clear input for another draft. The block is
not a history message; it survives pagination and is visible on the latest page.

Only `execution.completed` sends it, not intermediate text or automatic compaction.
The Sane/Slop mode is the one at actual departure. It then becomes a normal user
turn without clearing a draft started since, even in `$EDITOR`. `/…` commands are
not queued.

During an accepted turn, temporary model connection/transport/stream failures
trigger up to ten retries: immediately, then after 2, 5, 10, 15 seconds, then
15 seconds for remaining attempts. Only the current model call is retried, never
executed tools. A bold red Slop line before Activity shows, for example,
`Connection problem · Retry 2/10 · Retrying in 2s`, counting down each second,
then `Reconnecting…`. It disappears on recovery. Escape still cancels while
waiting. After ten failed retries, the spinner stops and the final error appears
under Activity; the submitted message remains in chat, not the composer.
Permanent errors are not retried. Titles, recaps, compactions and TUI/daemon
disconnections do not use this mechanism. Retry/failure notices are live-only,
not added to model context or persistent history.

Catalog reads also retry: a network outage shows `Connection problem`, not an
image-support error. A fetched catalog without capability declaration remains
blocking for caution; an explicitly non-image model is rejected without retry.
If catalog access still fails before message acceptance, draft text/images are
restored because the message has not been sent or saved in the session.

When pending text exists and exit confirmation is inactive, one Escape removes
it and restores text before the current draft, separated by a newline. This key
is consumed and resets interruption confirmation: stopping execution needs two
new Escapes. Execution failure, cancellation or disconnection also restores text
without automatic retry. This queue exists only in TUI memory, unsaved until sent.

### Conversation recaps

`/recap` in an idle session generates a short human orientation: current objective,
recent progress and next step if known. It is a separate, tool-free call with the
selected model/reasoning. It requests one paragraph, at most two short sentences
and 45 words, in English: what we are doing concretely, its purpose, and what the
user can try or what comes next. Test/build results and internals are omitted
unless they are the main topic or a blocker needing user action. Visual wrapping
depends on terminal width. Source is recent active-context text, capped at 24,000
UTF-8 bytes, without raw tool output or reasoning; a compaction checkpoint may
provide earlier context.

Each result adds a `Conversation Recap` card: bold title and `MISC_MD` (`#81D8D0`)
left stripe, `CONTENT_BACKGROUND_COLOR` background (subdued composer), plain text.
Cards persist in SQLite at the same position through `/resume` and pagination.
They are never injected into agent context, later recaps or compaction. Multiple
recaps coexist without replacing each other.

The card appears on launch with title/stripe; a spinner occupies the text area,
then the result replaces it in place. Only that spinner animates, not the
bottom-right button's loading bar. Double Escape cancels generation. Error,
cancellation or disconnection removes the provisional card without saving it.
The draft remains editable; an explicitly queued message sends after success or
is restored after failure. It appears below the card, whose position stays
reserved during generation. Recap does not restart the agent or change the last
turn's counters.

The TUI also requests a recap after **five continuous minutes without a submitted
message or ongoing work**, in an open connected session. The countdown is local,
monotonic and checked each second: time with the TUI closed, session age and
daemon age do not count. Execution, compaction or a new send resets the delay;
a long generation therefore requires five new idle minutes afterward. Changing
session/reopening also restarts it. Loading, popups and external editing suspend
counting, followed by a fresh five minutes. Typing without sending does not reset
it; automatic triggering preserves draft text and selection.

Only one recap is requested per idle period, even on failure/cancellation: no
five-minute repetition on unchanged discussion. Manual `/recap` also satisfies
that period. A new send/work rearms it, but **automatic triggering stays blocked
if the last displayable history item is already a Conversation Recap**. This
comes from the daemon's SQLite index regardless of viewed page: closing/reopening
or `/resume` cannot append a second consecutive automatic recap. Invisible system
events do not change that item. Manual `/recap` remains available even after
another recap. No recap timer runs in the daemon.

### Compaction

`/compact` in an idle session summarizes older exchanges to reduce model context.
Full discussion remains displayed; a collapsible bold red SLOP `Compaction`
drawer with a thick red top rule exposes the summary. Its spinner appears on
launch. Retained recent messages are not displayed twice. `/resume` restores all
history, including compactions. The button indicates activity during generation;
double Escape cancels. Failure/cancellation retains previous context. Manual
`/compact` does not resume work itself; explicitly queued messages send after success.

When known model limits allow the safety reserve, the daemon also compacts
automatically before a model call too close to the ceiling. The same drawer
appears; the turn stays active and work resumes after summary, preserving the
current turn and its tool calls/results. Escape also cancels this generation.
Drafts remain editable and are not sent automatically.

### Markdown and tools

Assistant responses render as Markdown: `TITLE_MD` headings, `BOLD_MD` bold,
`DEPTH_MD` links, `MISC_MD` inline code, italics, bullets and quotes. Code blocks
use `src/sos_tui/response_markdown.py`: strings `ALGAE_MD`, keywords `BOLD_MD`,
functions/types/numbers `TITLE_MD`, identifiers/comments `DEPTH_MD`, operators
`MISC_MD`. Other tokens and unknown languages use `TEXT_COLOR`, without native
palette fallback. Code background is `CODE_BACKGROUND_COLOR` (`#1E2226`), a
precomputed 25% blend of `#303A48` over `#181B1B`. `INPUT_BACKGROUND_COLOR` is
reserved for the composer. User messages stay plain text. Reasoning drawers use
the same Markdown renderer/palette as responses.

`read` shows only a golden-sand `read · path` line (`TITLE_MD`), without drawer or
detailed result; errors remain visible. Other tools have collapsed drawers with
coral titles on chat background. A drawer appears on call announcement, then its
result completes it through `callId`. Sections are `Arguments` and `Results`.
Results format on first expansion: paths/lines for `glob`/`grep`, output/exit code
for `bash`, tool-supplied diffs for `write`/`edit`/`apply_patch`, requested-format
rendering for `webfetch`. Errors use `SLOP_COLOR`. Old results without metadata
and unknown tools remain readable as plain text. Defined in `src/sos_tui/tool_view.py`.

Reasoning/tools group in Activity drawers, closed by default, following received
content order. The first contains activity between user request and first response
text. After each visible text, later activity creates another drawer before the
next text. Tool results complete their call in its original drawer. Grouping also
applies on resume, entirely in the TUI. `▸ Activity` in `MUTED_COLOR` precedes
colored squares, spinner, entry count (`1 step`, `12 steps`), `|`, duration and
known throughput: `Activity {squares} {spinner} 12 steps | 23s - 42 tok/s`.
Count/separator are absent without entries. Reasoning or a tool call counts once;
its result/refusal does not count again. Count restores from history without
loading drawer details. Clicking reveals activity two columns to the right and
changes arrow to `▾`; clicking again hides it. Response immediately follows,
without separator/blank line, open or closed.

During execution the dash rotates (`-`, `\`, `|`, `/`) using the pulsing button's
timer, no extra timer. Squares mark received items: coral for tools except `read`,
lagoon for reasoning, golden sand (`TITLE_MD`) for reads. They remain after turn
completion. Reasoning titles use `MISC_MD`, tool titles `BOLD_MD`, `read` uses
`TITLE_MD`. Clicking drawer titles does not move keyboard focus: the composer
keeps it without focus round trips or whole-drawer style recalculation. Titles
remain focusable for keyboard commands. Events continue filling closed drawers;
inner toggles retain state. User messages/responses stay visible. Component:
`src/sos_tui/turn_activity.py`.

Activity shows duration/throughput right of squares, spinner and count.
Intermediate Activities keep counts but no spinner. Model/reasoning remain at
response footers, also after history resume; missing old durations are not
invented. Duration grows from send using the spinner's timer, then freezes at
the daemon measurement saved in the final message. Formats: `12s`, `2m 08s`, then
`1h 05m` (no days). Old turns retain their own model, reasoning and duration.

### Modes and permissions

Tab switches Sane (default, blue-green) and Slop (pink). Mode is sent to the daemon,
which applies permissions and saves it in the session, including a switch without
a new message. `/resume` restores it; old sessions without a mode use Sane. The
stripe left of input indicates current mode and follows input height, with one
empty column before text and composer background retained. Each user message
keeps its send-time mode color on a one-column stripe followed by a space. Its
background is `CONTENT_BACKGROUND_COLOR` (`#242A31`), precomputed 50% blend of
`#303A48` over `#181B1B`. Model responses are left-aligned without role labels.

In Sane, writing/editing/patch tools and mutating/unknown bash commands are
forbidden. In Slop, these operations show a permission card in chat below turn
Activity at the next response position. Detected external-directory access also
requires permission in both modes. Only the launch workspace is authorized
initially, even after resuming another project's session.

The card offers `Yes`, `Always yes`, `No`, in that order on one compact button row.
It uses chat background, yellow `Allow …` title and a full-height one-column
yellow stripe separated by a space, without frame or scope explanation. Command,
working directory, paths and any diff remain visible. On arrival, scroll if needed
to expose `Yes`, then focus it after rendering. A new attempt does the same for
the previous choice. If a focused button is scrolled offscreen, Enter reveals it
without deciding; confirm again when visible.

After resolution the card disappears; the tool in Activity receives ` -> Yes`,
` -> No` or ` -> Always yes`. One call may have multiple decisions, e.g. external
access then writing. Traces restore with history. `No` cancels the turn like
Escape without launching later tools. Waiting remains cancelable; heartbeats continue.

`Always yes` lasts only for the current connection: directory/descendants, the
whole editing family (`write`, `edit`, `apply_patch`), or inferred bash command
scopes. Launch-directory access is already authorized but does not authorize
editing or mutating execution. Approval survives `/new` and `/resume` but ends
with TUI closure. It never bypasses Sane. Bash analysis is a permission policy,
not system isolation of program effects.

### Jev confidence

In `/permissions`, `How confident should Jev be before running an action?`
appears directly below the three modes only when Jev has `Current` and its row
is highlighted. On Ask/Danger, hide the block/buttons; returning to Jev restores
the draft. There is no separate percentage screen. Enter on Jev directly uses the
saved OpenRouter key. Without a key, masked entry appears with `Cancel`; once
validated, Jev activates and the list returns without closing the popup. There
is no `Use saved key` button.

Immediately below the question, a short borderless single-line integer field
0–100 sets minimum execution confidence; left/right arrows remain available.
Only 0% and 100% explanations appear at right, without presets or extra text.
`Save` (or Enter in the field) closes after threshold acknowledgement. Errors leave
the draft open. `Change API key` opens replacement entry; Escape/Cancel returns
without saving it. Escape from the list closes without saving a draft percentage.
The threshold is global, persistent and applies to later checks of the current turn.

Confidence is always confidence in executing. A 98% `deny` becomes 2%; the same
score controls decisions/display. Default threshold: 80%. At 0%, no Jev call,
like Danger, but Sane retains priority. `Skip Jev` is removed; entering 0 colors
the right-side 0% explanation in Slop warning before Save, without another line.
At 100%, a 100% score is required, without implying a safety guarantee.

`Refused tools` rows show `bash • Confidence 2%` in Slop without refusal suffix.
After `Approve`, ` -> Manually approved` is appended and the entire row turns
green. Historical score is retained. Old refusals/failures without score show
`Confidence unavailable`, never invented zero. Lower history stays immutable.

On next send, a system message gives the model the exact action and replay ID.
`rerun_approved_action` accepts only that ID: daemon retrieves tool, arguments,
directory and workspace without Jev. Revocation is checked before every permission
boundary; Sane and cancellation retain priority. Approval clicks execute nothing.
Approvals remain reusable within the session and survive `/resume`. Activity
shows the actual tool/arguments with `-> Manually approved`.

### Information bar

Above the composer: model/reasoning left, `last-call context / model capacity`
center, fixed-width button right, currently without action. Centered text switches
between `Sane implementation` and `Slop`, background following mode. During a
request it shows `Cancel`; dark background advances from left until one colored
column remains at right, then retreats. Width stays fixed. Text returns after
success/error/disconnection; timer is suspended at rest. Renderer:
`src/sos_tui/mode_button.py`.

The ratio is followed by percentage in brackets, rounded to nearest integer
(`[13%]` for 12.5%). As width shrinks, hide ratio, reasoning, percentage, then
model; button stays prioritized. Fields return as width grows. Values come from
the daemon; unknown context is `—`. Counter is input + output tokens of the last
discussion call, not cumulative calls or later additions.

Initial reasoning comes from daemon `DEFAULT_REASONING_EFFORT`. Ctrl+T requests
the next provider-supported level starting at `low`, wrapping after maximum.
Unsupported levels are skipped; no levels yields an error without changing settings.
Selection is global and saved immediately in `config.json`, even before the first
message, remaining after `/new`, `/resume` and restart. During generation it
applies to the next message: the bar shows the new selection, Activity keeps the
current turn's. Available reasoning is collapsed in clickable `▸ Reasoning`.

### Composer and commands

Enter sends; Shift+Enter inserts a newline. The TUI accepts Kitty/CSI-u, xterm
and LF encodings (including Zed). Ctrl+J also inserts a newline. On Unix it enables
`modifyOtherKeys` before Textual's Kitty protocol, without changing user config.
These modes exit before external editor/shell and reactivate on return. Newlines
and submission use the same queue as field characters, not priority shortcuts:
keys in one terminal packet insert preceding text before newline/send. Tab also
respects ordering before suggestion selection. An older terminal sending identical
codes for Enter/Shift+Enter without extended protocol cannot be distinguished;
Ctrl+J is the fallback. Input grows from one to six lines including wraps, then
scrolls, taking space from chat.

Pastes of at least three lines or 200 characters become compact yellow
`[the first three words…]` blocks without number/count. Preview is limited to
24 columns including ellipsis, excluding brackets. Small pastes remain normal
text. Multiple blocks, even identical previews, retain separate original content.
Arrows move across blocks; Backspace/Delete removes one atomically. Undo/redo
restores content. Clicking opens full text with `Close`/`Remove`. Sending,
queueing and copying use original content, never preview. External editing also
receives full text: unchanged blocks remain collapsed; changed text returns
normally. Restoring a rejected send returns full plaintext without altering
blocks in a new draft.

While idle, `/undo` removes the last user+AI turn and restores affected files;
user text/images return to input. `/redo` restores turn/files without rerunning
tools/model. A new send discards the redo branch. Git-captured file changes include
Bash effects; ignored/external files and non-Git workspaces journal only `write`,
`edit`, `apply_patch`. Conflict blocks the operation and leaves draft/displayed
history intact. Canceled/failed turns are covered unless a hard crash prevented
final-state verification. See [limits](../../docs/git-snapshots.md).

A leading `/` shows commands above the composer alphabetically: `/btw`, `/compact`,
`/connect`, `/model`, `/new`, `/permissions`, `/recap`, `/redo`, `/resume`, `/undo`.
Prefix filtering applies (`/mo` shows only `/model`). Initially nothing is selected,
even with one result. Up/down selects without leaving input; Tab selects first
if none, then advances/wraps. Shift+Tab does the reverse: last if none, then
backward/wrap. This applies to commands, skills and quotes. While suggestions are
visible and input focused, Tab/Shift+Tab do not change mode. Outside suggestions,
Shift+Tab keeps previous-control navigation.

Keyboard/click selection replaces only the cursor's word. Suggestions retain
initial-prefix candidates during navigation instead of refiltering on the inserted
name. Editing/cursor movement closes the list and retains text. Enter submits
current input. For skills, only `/name` is inserted; `argument-hint` stays gray
and unsent. Skills may be invoked without arguments; the model asks for needed
details. Replacements are undoable; navigation never sends. Unknown commands or
arguments on commands that accept none show a local error notification: draft
stays editable, nothing sent to model. A slash inside ordinary prose stays text.

A lone `>` on an input line suggests paragraphs from the last assistant message:
each nonempty line is a choice, including each bullet, regardless of terminal
wrapping. First choice selected by default; Tab/Shift+Tab cycle, up/down changes
choice. Navigation/click directly replaces the current line with `> text`, leaving
surrounding lines intact. The list stays open for another quote. Enter accepts
and places cursor below without sending. Movement/deletion also leaves navigation
below without deleting the quote. Typing, Space, paste or newline accepts then
applies input below; newline adds an empty line after the quote. Without a source,
continuing to type removes the marker. Choices/acceptance have separate undo
steps and preserve the rest of the draft. Quoted text stays ordinary Markdown
when sent/saved.

Displayed user quote lines have background `#445266`, lighter than composer,
with a MUTED half-bar `▌` at left in addition to Sane/Slop stripe. Text is
TEXT_COLOR, not italic; the following question resumes normal styling. Composer
uses the same decoration, including wrapped lines, one space between mode stripe
and quote bar. Normal text below starts at that bar's column. Two spaces separate
bar and quoted text both in composer and after send, including wraps. `>` remains
in editable/sent text; decoration changes neither copy, selection nor undo.
Deleting within a quote removes the entire consecutive `>` block plus its final
newline if present, without touching following text. Backspace, Delete, Ctrl+W
and Cut respect the atomic block; undo restores it. Ctrl+W deletes the preceding
word in normal text, including extended terminal keyboard encodings.

### Focus and copying

Clicks on chat content/titles retain current focus. Other interactions return
focus to composer when input is active, except permission buttons. A popup keeps
focus while open, notably `/resume` keyboard navigation. Terminal FocusIn,
including after Alt+Tab, restores input focus before the next key unless a popup
or authorization button owns it. This moves neither draft cursor nor chat scroll.
No forced refocus while window inactive, external editor open or input disabled.
If Zed focuses its editor rather than integrated terminal, TUI cannot take it:
events/keys must reach the terminal first.

In Discussion, left/right controls pending permission choices even if focus left
the buttons. Navigation resumes from the last choice, returns to the card and
does not move draft cursor. After a response, arrows stay reserved until resolution,
without allowing a second answer. Other typing is not intercepted. Popups/external
editor retain their own navigation. Without a request, arrows behave normally.

Selecting text in discussion/popups copies automatically on mouse release and
clears highlighting. Draft selection also copies but stays selected for editing.
A click without selection does not replace clipboard; password fields never
autocopy. Each copy shows a three-line `Copied` badge bottom left, just above the
model/reasoning bar (screen bottom in popups), no frame: full-height golden left
stripe, input background, vertically centered yellow `TITLE_MD` text between
blank lines. It disappears after 1.4 s; another copy restarts the delay without
stacking badges, moving content or taking focus.

Copy uses OSC 52 and available local native clipboard (`pbcopy` on macOS,
`wl-copy`/`xclip`/`xsel` on Linux, PowerShell on Windows), without installing tools
or changing terminal settings. SSH uses only OSC 52 to target the client terminal;
acceptance depends on terminal/multiplexers. No TUI tooltips. Clicking history
text/empty space does not focus its container, avoiding descendant style
recalculation; selection, links, drawers and scrolling remain active.

### Exit and external editing

Ctrl+C clears nonempty input without preparing exit. With empty input, the first
press replaces the whole model/reasoning/context/mode line with a Slop
`SLOP_COLOR` bar and centered `Exit ? Confirm with CTRL-C`. It drains over three
seconds from the shortcut's deadline. A second Ctrl+C in that interval exits;
otherwise updated information returns. Typing/sending cancels confirmation.
Height stays fixed; resize/info updates do not restart countdown. During the
interval, Escape is ignored: no closing exit confirmation, pending removal or
interruption arming. Normal behavior resumes at expiry. Exit/execution-cancel
confirmations remain separate: Escape/Cancel never confirms exit, Ctrl+C never
confirms cancellation. Ctrl+Q exits directly.

Ctrl+G opens draft in `$EDITOR`, including arguments. If unset, show an error and
retain draft. On editor close, restore saved text without sending. For graphical
editors use a wait option, e.g. `EDITOR="code --wait"`. Refresh is blocked before
terminal suspension and resumes only after driver restart. Async loop remains
active, including daemon pongs; no frame is sent to the stopped driver's output
queue. Shortcut is available whenever input is enabled, even during generation,
except popups/another external edit. Agent may respond/finish without closing
editor; draft never sends automatically. A restored unaccepted message (refusal,
cancellation/disconnection) is kept before the edited draft on return. On editor
failure, original draft is retained with restored text. Pending permission is
made visible on return and retains keyboard-focus priority for explicit response.

### Resume and history

`/resume` then Enter opens a centered session popup, even during generation.
Confirmation cancels/saves the current main turn before switching. Scrollable
list shows titles and right-aligned local time/date (`HH:MM DD/MM/YYYY`), newest
activity first. IDs remain internal. Up/down changes selection; lower preview
shows the last user request, ignoring stale preview responses. Enter resumes
history, workspace and title. Global model/reasoning stay selected for future
turns; old Activity shows actual historical values. Escape closes; failed resume
retains current conversation.

Daemon opens a lightweight disk index and reads latest visible messages without
building the agent or rereading large tool results. Details load on Activity
expansion. Old sessions without index need initial background indexing. First
send after resume shows cancelable `Preparing conversation…` while restoring
agent context. With compaction, read only checkpoint/needed messages; otherwise
full context is required.

During resume or return to present before sending from an old page, a bold
light-blue (`MISC_MD`) `[####                  ]` bar replaces context counters.
Centered when space allows, shortened on narrow terminals without overlapping
mode button. Composer/draft remain visible at the same height. Animation is
indeterminate, not a real percentage; counters return after loading. Events
without info changes do not refresh the bar; only changed labels update.
Resize/loading-state changes still recompute layout. Scroll pagination is silent:
no loading bar/status change, input keeps focus/editability. Sending stays blocked
until navigation ends without clearing draft; loading errors remain visible.

Only the last Activity's rows preload, even with more than 50 tools or ongoing
work. Others arrive as summaries and request rows on expansion. An old window
does not preload its own last Activity instead of the session's last. Imported
tool rows contain only title/state/permissions data. For `read`, this means path
and any error, never successful read content. Expanding other tools requests
full arguments/result with metadata, diffs and attachments. Reasoning text also
loads only on expansion. Close/reopen retries load errors. Protocol moves to
version 4: rebuild/restart daemon and TUI together. Two-level loading concerns
imported history; live generation events are unchanged.

Explicitly opened content stays cached in the displayed window until next send.
Every new message collapses/clears old drawers: detail widgets, preloaded/requested
data and pending Activity requests. Titles, indicators and metadata remain;
reopening reloads that group. Only new Activity fills live. Disk journal/model
context are unchanged.

History uses a sliding window of 20 user/assistant messages, moving by ten. Ten
shared messages retain visual position: slight downward scrolling after a load
does not immediately trigger reverse loading. Edge moves may be smaller. New
accepted messages join the same window; beyond 20, oldest messages/widgets leave
view without journal deletion. The last turn reserves only space needed for top
alignment; empty space creates neither another message nor a separate history batch.

Sending from the latest window reloads nothing: old drawers collapse/clear locally,
then message sends directly. From an older window, the new turn immediately
appears at top and stays visible while latest page loads before daemon send.
Failure removes preview/restores draft without sending. Old page stays visible
until new page, prepared outside layout, mounts; widgets swap in one batch.
Unchanged messages reuse widgets; evicted pages are not cached. Collapsed
compaction summaries construct Markdown only on expansion. On resume the popup
closes before page rendering, leaving previous conversation visible with loading bar.

Scroll locks from window request through final positioning: wheel, trackpad and
keyboard cannot move view during transfer, mounting or repositioning. Old events
are not replayed afterward. Layout/anchor recompute in the same rendering batch,
without an intermediate frame at the wrong offset. Error/cancellation also unlocks.

`/resume`, `/connect`, `/model` share `dialog_screen.py` styling: square muted
border, coral title, sand sections, muted bottom help. Session list keeps fixed
`Title` / `Last activity` headers. `Last user request` preview is at most four
lines and scrolls independently if needed.

### Models

`/model` then Enter opens model selection even during generation. It concatenates
compatible models from connected providers (OpenAI then OpenRouter), newest first
for OpenRouter (`sort=newest`) and OpenAI API (`created` descending, missing/invalid
last, stable ties). Codex sorts ascending `priority` (missing/invalid last, stable
ties): provider ranking, not chronology. Fast variants stay immediately after
base models. Model left, provider column far right with labels aligned by first
letter: `Openai (Codex)`, `Openai (Api key)` or `Openrouter (Api key)` according to
saved authentication. A model remains separately selectable from multiple
providers. Current model has `Current`; first result preselected on opening and
each search change.

Borderless `Search`, even focused, instantly filters provider/model substrings,
case-insensitive without regex. Filtering is local memory, no ripgrep/network
request. ↑/↓ navigates even while typing; Enter confirms, Tab changes field,
Escape closes/cancels pending loading. Catalog errors display without hiding
other providers' models. Empty list suggests `/connect`.

Selection remembers provider/model globally, retaining reasoning, mode and
conversation. Bottom-left bar changes immediately; an already-launched model call
keeps settings, and the next call in that turn applies new selection. If no more
calls, it applies on next message. Response footer retains the actual model used;
old Activity retains historical metadata. Selection failure keeps popup open to retry.

After model selection, a field asks effective token maximum, prefilled with
catalog maximum or saved value for the current model. Unknown maximum leaves
it empty. TUI/daemon reject below 32,768 or above known maximum. Enter saves with
global model; Escape returns to list unchanged. This is not a per-model preference:
changing model offers its maximum. Budget applies to display/compaction with the
usual reserve.

### Turn lifecycle

`/new` then Enter cancels/saves ongoing main execution and returns to initial
state: empty chat, title `-`, launch workspace, Sane mode. Global model/reasoning
are retained. Previous session stays available in `/resume`. No new session before
the next send. `/connect` and `/permissions` are also available during turns.
Provider confirmation stops the main turn before detachment. Permission changes
affect current/later checks without undoing launched tools. A pending Ask request
is reevaluated on switching to Auto/Danger.

During execution, `/compact` and `/recap` show a yellow-stripe warning to wait;
they are not queued. Full answers, available reasoning and tool calls/results
appear in chat. Composer remains active for the next draft; Enter puts it in
`[Pending]` until full turn completion. An unsubmitted draft never sends
automatically. Submitted text leaves input immediately; preacceptance rejection
restores it before any new draft. Ctrl+Q exits. There is no streaming.

During work, first Cancel click or Escape without pending replaces the same line
with a draining Slop bar, centered `Cancel Execution ? Confirm with ESC`.
Second click/Escape within three seconds sends `execution.cancel`; the line then
stays Slop with `Cancelling…` until daemon terminal response, without another
countdown. Without confirmation, normal information returns. Animation reuses the
mode button timer, which also keeps animating Activity. The next turn waits for
daemon terminal event. During permission-choice acknowledgement, Enter in composer
is ignored to avoid accidentally queueing a draft through double press.
Cancellation during title creation retains draft and creates no session.
Workspace sent to daemon is the TUI launch directory. Model is the daemon default;
selection/resumption will follow.

## Building and sharing

From the root, CMake builds `sosd`, packages a standalone distribution and installs
`sos`:

```sh
cmake -S . -B build
cmake --build build
```

The `cmake/build-tui/` subproject contains PyInstaller packaging/installation.
`cmake --build build --target build-tui` produces only the distribution;
`--target install-tui` builds and installs it. Agents use `build-ai/`, never the
human's `build/`, and keep verification installations under `build-ai/` as
specified in [CONTRIBUTING.md](../../CONTRIBUTING.md).

Share `build/tui/dist/sos-<OS>-<architecture>.tar.gz`. Recipient extracts the full
directory and runs `./sos/sos`, keeping `_internal/` beside the launcher. Python,
TUI packages and daemon are bundled; no uv environment/project checkout required.
The archive targets its build OS/architecture with compatible system versions;
build separately for macOS/Linux and each architecture.

Local installation keeps versioned distributions under `$XDG_DATA_HOME/sos/app/`
or `~/.local/share/sos/app/`, then creates/replaces `~/.local/bin/sos` pointing to
the installed standalone executable. No global `tui` command is installed.
Repository/`.venv` are unnecessary to run it. Full CMake build installs automatically.
Invoke `sos` from any directory, which remains the working directory.
`~/.local/bin/` must be on `PATH`; if needed, run `uv tool update-shell` and reopen
the terminal.

## Images

Pasting/dragging a PNG, JPEG or WebP file path into composer creates
`[Image • /path/screenshot.png]`. Spaces/quoted paths work; a sentence mentioning
a path remains text. Content is captured immediately: deleting/changing the file
does not change attachment. Long text blocks use `Pasted text` prefix.

Ctrl+V explicitly reads a native clipboard image into `[Pasted Image]`: macOS
uses `osascript`, Wayland `wl-paste`, X11 `xclip`. No clipboard monitoring.
Cmd+V depends on terminal; if it sends no image data, use Ctrl+V or paste a path.
Over SSH, client native clipboard is unavailable; path must address a file
accessible to the remote TUI.

Clicking shows format, dimensions, size and origin, with `Close`/`Remove`.
Deletion/undo/redo affect the whole block. Queueing/failure restoration preserve
captured bytes. External editor receives no pixels: images remain even if their
mentions are removed there; use composer `Remove` to remove an image.

Local limits: eight images/message, 5 MiB/image, 10 MiB total, 16,384 pixels/side,
40 megapixels. Provider limits may be stricter. Daemon and TUI must both use
protocol 6.

On send, composer clears immediately and provisional message/animated Activity
appears. This is not acceptance: daemon checks image support before saving or
calling the model, including on queue departure and for active-context images.
On refusal, TUI removes provisional turn, restores text/images without overwriting
a new draft, and shows SLOP error. Separate preflight no longer blocks composer.

Images send as multimodal content and persist in SQLite with session deduplication.
`/resume` restores without rereading original files. After compaction, old pixels
stay saved but are not automatically sent to the summarizer. Model can reread
with `view_image` and `img_…` ID, consuming context again. History displays
metadata, not terminal thumbnails.
