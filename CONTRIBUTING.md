# Development conventions

## Commits

Use `type(scope): description`, with lowercase type and scope and an English
description. Possible types: `fix`, `feature`, `tests`, `docs`, `build`, `ci`,
`refactor`, `chore`, for example
`fix(tui): preserve reading position during markdown updates`.

Text displayed by the application (TUI, logs, errors and daemon prompts)
is written in English.

## Comments

- Write comments and docstrings in English, explaining intent or a non-obvious
  constraint rather than paraphrasing the code.
- Preserve security, concurrency, protocol and compatibility invariants, as well
  as license notices. Avoid session notes and time-relative wording such as
  "new" or "now" without a useful version reference.
- Keep descriptions concise and up to date; do not comment on every operation.

## Default integration

**By default, TUI changes only concern the Python/Textual version in
`integrations/tui/`.** Only change the C++/FTXUI integration in
`integrations/ftxui/`, its tests or its build when the author explicitly requests
it for that version. A Textual fix or enhancement does not authorize porting it
automatically to FTXUI, even to maintain parity between the two integrations.

## C++

- Use C++23, with the standard required and compiler extensions disabled.
- Represent known structures with explicit C++ types. Reserve JSON values for
  dynamic data (tool arguments/results, provider-specific data). Handle
  deserialization into our types at the boundaries.
- Name constants in `SCREAMING_SNAKE_CASE`.
- Use `.hpp` for C++ headers and `.cpp` for implementations.
- Use lowercase filenames, with no uppercase letters: for example,
  `agent.hpp` and `agent.cpp` for the `Agent` class.
- Place `.hpp` headers next to their `.cpp` files.
- Protect headers with `#ifndef` / `#define` / `#endif`, not `#pragma once`.
- Name the guard `SOS_{CLASS_NAME_IN_SCREAMING_SNAKE_CASE}_HPP`.
  For example, `Agent` uses `SOS_AGENT_HPP` and `AgentRuntime` uses
  `SOS_AGENT_RUNTIME_HPP`. For a header without a class, use the filename:
  `main.hpp` uses `SOS_MAIN_HPP`.

## CMake organization

- The root `CMakeLists.txt` adds components from `src/` with `add_subdirectory`.
- Executables are generated in `bin/` and libraries in `lib/`,
  inside the chosen build directory.
- Each component has a `CMakeLists.txt` next to its `cmake/` directory.
- Explicitly list its `.cpp` and `.hpp` files in `cmake/sources.cmake`,
  included by the component's `CMakeLists.txt`.
- `agent-runtime` is a static library, available as the `sos::agent-runtime`
  target. It exposes the include path for `#include <agent-runtime/agent.hpp>`.
- `sosd` is the daemon executable. Its transport uses standalone Asio 1.36.0,
  downloaded by CMake, without Boost.

## Building and running

The runtime uses CPR for HTTP requests, through a `cpr::Session` object.
CMake uses an installed CPR (1.14.2 minimum) or downloads version 1.14.2,
configured to use the system libcurl. The libcurl development files must be
available. HTML conversion for `webfetch` uses libxml2
(`find_package(LibXml2 REQUIRED)`). The runtime also depends on
`nlohmann/json` (3.12.0 minimum). CMake uses a compatible installed version
or downloads version 3.12.0 through `FetchContent`.
OpenSSL Crypto is required for cryptographic randomness and OpenAI OAuth PKCE.
The Codex WebSocket transport uses IXWebSocket 11.4.6 (downloaded by CMake)
with OpenSSL SSL, TLS verification enabled, and HTTP fallback if the handshake fails.
Skill frontmatter uses `yaml-cpp` 0.8, found on the system or downloaded by CMake.
The loading format and scope are described in [`docs/skills.md`](docs/skills.md).
The daemon's snapshot facility and its tests require `git` in the `PATH`.
Its API, private storage and limits are described in
[`docs/git-snapshots.md`](docs/git-snapshots.md). It powers `/undo` and `/redo`,
with an editing-tool journal for ignored files or files outside the repository.
These downloads require network access during the first configuration.
Bash parsing uses Tree-sitter 0.25.10 and the Bash grammar 0.25.0,
downloaded and statically compiled through `src/agent-runtime/cmake/tree_sitter.cmake`.

The full build also requires `uv`. All building and installation are orchestrated
by CMake; the integrations' Makefile has been removed.
The root `CMakeLists.txt` adds `cmake/build-tui/` with `add_subdirectory()`.
The `build-tui` target depends on `sosd`, syncs the Python dependencies from the
`build` group using `uv.lock`, then uses PyInstaller to bundle the TUI, Python,
`sosd` and the required native libraries other than system libraries.
It produces `tui/dist/sos/` and `tui/dist/sos-<OS>-<architecture>.tar.gz`
inside the build directory. The archive is standalone on a compatible platform;
it does not require Python or uv on the recipient's machine.
To share it, send the archive. The recipient extracts it and runs `sos/sos`
from a terminal, keeping the entire extracted directory intact.
The archive must match the recipient's operating system and architecture.
On macOS, an unsigned downloaded distribution may be blocked by Gatekeeper.
CI on `main`, tag-triggered draft Releases and the future backend contract for
the `scripts/install.sh.in` template are described in
[docs/distribution.md](docs/distribution.md). The public installer downloads a
remote archive; it is separate from the local CMake installation.

The `install-tui` target, included by default, installs this distribution in
`$XDG_DATA_HOME/sos/app/` (or `~/.local/share/sos/app/`) and creates the `sos`
link in `~/.local/bin/`. Installed versions are separated by their archive hash,
so an already-running daemon retains its files during a rebuild.
Old versions are not removed automatically.
`SOS_TUI_INSTALL_DIR` and `SOS_TUI_BIN_DIR` allow custom destinations.
For agent verification, both must remain under `build-ai/`.
A packaging or installation failure fails the build. Native-only targets
do not trigger packaging.

To run the TUI locally from `integrations/tui/`: use `uv run tui`, after a
`build-tui` build, which also places a development copy of `sosd` there.
The installed `sos` command preserves the directory it was invoked from.

The native `integrations/ftxui/` integration uses C++23 and FTXUI 6.1.9, without
a Python interpreter. `SOS_BUILD_FTXUI` enables its build (default `ON`);
`SOS_BUILD_TEXTUAL=OFF` completely disables Python packaging configuration
and its dependency on `uv`. The `build-ftxui` target produces `bin/sos-ftxui`
and `bin/sosd`. The explicit `install-ftxui` target installs hash-separated
versions in `$XDG_DATA_HOME/sos/ftxui/`, then a `~/.local/bin/sos-ftxui` link.
For agent verification, also configure
`-DSOS_FTXUI_BIN_DIR="$PWD/build-ai/ftxui/bin"` and
`-DSOS_FTXUI_INSTALL_DIR="$PWD/build-ai/ftxui/installed"`.
The `FtxuiE2E.*` tests are themselves written in C++ and drive the real executable
in a pseudo-terminal, using either a fake protocol daemon or an isolated real `sosd`.
The daemon's native system libraries are still required at installation time.

Always configure and build from the root `CMakeLists.txt`.
**The agent uses only `build-ai/` for verification.
The `build/` directory is reserved for the human.**

### For the human

From the repository root:

```sh
cmake -S . -B build
cmake --build build
sos
```

### For the agent

From the repository root:

```sh
cmake -S . -B build-ai \
  -DSOS_TUI_BIN_DIR="$PWD/build-ai/tui/bin" \
  -DSOS_TUI_INSTALL_DIR="$PWD/build-ai/tui/installed"
cmake --build build-ai
./build-ai/tui/bin/sos
```

### Connection server

`sosd` currently listens on `/tmp/sosd.sock` and logs every connection.
You can supply a different path and a sessions directory:
`sosd /path/server.sock /path/sessions`.
The TUI connects to the existing daemon or starts its bundled `sosd` in the
background, with the first launcher's environment (including XDG paths).
A per-socket lock serializes concurrent starts. A stale socket is replaced,
but a regular file is never deleted. The TUI signals its closure through
`integration.close`. The daemon also detects socket closure and checks each
integration with ping/pong: a 15-second interval and a 30-second response timeout.
An unresponsive integration is closed and its execution canceled.
After the last integration leaves and cancellation/saving finishes, the daemon
removes its socket and exits. At startup, it waits at most 30 seconds for its
first connection. These durations are centralized in `src/daemon/config.hpp`.
Its logs and launch PID are in `$XDG_STATE_HOME/sos/sosd.log`, or
`~/.local/state/sos/sosd.log`. An existing daemon is not automatically replaced
by a new build; closing its integrations is enough for the next launch to use
the installed binary. To connect from another terminal:

```sh
nc -U /tmp/sosd.sock
```

Ctrl+C stops the daemon and removes its socket. An already-occupied path is
rejected without automatic deletion. Connections exchange JSONL; an integration
is created per connection, with no session until the first message or an attach
request. See [the protocol](docs/daemon-protocol.md).
No API key is needed to listen/list or open `/connect`.
This command connects OpenRouter through an API key, or OpenAI through an API
key, browser OAuth (local callback on 1455) or a device code. The daemon stores
credentials in `$XDG_DATA_HOME/sos/auth.json`, defaulting to
`~/.local/share/sos/auth.json`, as unencrypted JSON with `0600` permissions,
replaced atomically. Entered keys are masked and never pass through conversations.
Authentication uses `auth.json` exclusively; no key is read from the environment.
Provider/model/reasoning selection and the context maximum are conversation-specific.
The last explicit choice is also stored in `$XDG_CONFIG_HOME/sos/config.json`
as the default for new conversations.
`/resume` restores saved settings without changing that default; `Ctrl+T` saves
the level for the conversation and the global default, including across restarts.
Metadata from previous turns remains historical.
The `none` level is displayed as `No reasoning`. OpenRouter only offers it when
explicitly listed in `reasoning.supported_efforts`; a null list does not prove
support. Codex reads `supported_reasoning_levels` from its authenticated catalog,
including for fast aliases. For the OpenAI API, the known GPT-5.1, GPT-5.2 and
GPT-5.4 families, excluding Codex/Pro variants, also offer `none`. The reasoning
default is still resolved without network access; level discovery runs on a worker.
A request using `none` includes neither a reasoning summary nor a request for
encrypted content. A change through `/connect` detaches the idle session and
starts a new conversation; the old one remains accessible through `/resume`,
with its saved provider.

Global C++ paths are centralized in the `PathResolution` struct
(`agent-runtime/common/path_resolution.hpp/.cpp`). Relative or empty XDG overrides
are ignored. `ProviderAuth` separates credentials from transport;
`OpenAIProviderAuth` refreshes OAuth tokens under a lock shared by agents and title
generation. The Codex transport reads an HTTP SSE stream until
`response.completed`, then returns a complete Responses object to the runtime.
SSE decoding (`common/sse_decoder.hpp/.cpp`) is unaware of the model protocol.
`ResponsesStreamAssembler` consumes JSON events, retains
`response.output_item.done` items by `output_index` and merges them with the
final output without duplicates. Final metadata and replay data are preserved,
including when Codex finishes with `output: []`. A truncated or failed stream,
or one with missing items, is rejected before reaching the runtime.

`Integration` orchestrates commands and sessions; `ProviderConnection` owns
the local connection flow (start, polling, cancellation, draining).
`ProviderConnections` encapsulates shared credentials and exposes `hasAuth`,
`requireAuth`, `saveApiKey` and provider creation. OAuth details remain in the
runtime. Worker waits go through `transport/await_future.hpp`.
See [the refactoring plan](docs/auth-refactor.md) for responsibilities and interfaces.
`ModelCatalog` loads and filters catalogs for automatic selection in `/connect`,
which displays no model field. This lookup runs after authentication on a
cancelable worker, before changing the selection or detaching the session.
`/model` reuses these catalogs to list models from connected providers, with a
local text filter in the TUI and explicit provider/model selection.
For Codex, SOS supplements the authenticated catalog with visible, API-compatible
models from the official public `openai/codex` catalog (anonymous request).
Authenticated entries, including hidden ones, take precedence for identical IDs;
if the public catalog is unreachable, only the authenticated catalog is used.
This additional list does not guarantee that the account can generate with a model:
a server error is still returned at call time.
For Codex, `service_tiers: [{"id":"priority"}]` or the older
`additional_speed_tiers: ["fast"]` capabilities produce a synthetic `…-fast`
entry immediately after the original model, without hiding a real catalog ID.
It inherits context and reasoning; the provider sends the original model with
`service_tier: "priority"`. This choice may consume more credits.
Unavailable aliases are rejected without silently falling back to normal mode.
The OpenAI API-key and OpenRouter catalogs are not enriched this way.
The `model.list`, `model.select` and `model.cancel` commands go through
`ProviderConnection`. Selection preserves the conversation and reasoning;
the next user message applies the provider/model change to the agent context.
All generations in an already-started turn retain their settings and metadata.
`/new`, session resumption and the final `/connect` selection cancel and save
the main agent before switching.
`/model` then requests an integer `context_limit`, at least 32,768 and capped
at the catalog's known maximum. This value is saved with the conversation and
the last global choice in `config.json`, without a per-model table. The list
transmits `max_context`, and the daemon validates input against its own catalog,
not a client-supplied bound.
The chosen budget becomes the compaction input ceiling (with the usual reserve),
the retention basis and the displayed total. `/new` inherits the last global
choice, `/resume` restores the conversation's choice, and changing reasoning
preserves it.
`ApiKeyValidator` validates a candidate key before saving it, without using the
old key or generating content: `/api/v1/key` for OpenRouter, `/v1/models` for
OpenAI. The bounded, cancelable authenticated JSON GET is shared with
`ModelCatalog` in `providers/provider_http.hpp/.cpp`.

Textual custom providers are named OpenAI-compatible Chat Completions instances:
`provider.create` discovers `/models` before saving global configuration
(`custom_providers` in `config.json`) and the optional key separately in
`auth.json`. Generated `custom-…` IDs are stable; names are used for display.
`ChatCompletionsProvider` adapts internal Responses items to Chat messages and
normalizes replies to retain the existing transformers, journal and tool loop,
without sending a Responses request to the custom server. Capabilities remain
unknown, with no OpenAI-family filter or imposed reasoning parameter.
`/model` discovers their models and allows setting the context budget.
`provider.delete` preserves sessions and rejects deletion of the current provider
until another has been selected. See
[the protocol](docs/daemon-protocol.md#custom-providers).

OpenAI (API and Codex) and OpenRouter generations also use this shared transport:
a 15-second connection timeout, 10-minute total request timeout and 10-minute
receive-idle timeout. For Codex (SSE), inactivity is monitored from the start,
and every received fragment, including heartbeats, resets that timer.
Without streaming (OpenAI API and OpenRouter), the provider may send nothing
while reasoning, so the idle timeout starts at the first body bytes, with the
10-minute total cap maintained throughout the wait. Errors distinguish inactivity,
request timeout and transport categories without exposing raw diagnostics or
credentials. Cancellation takes priority. Catalog and authentication timeouts
are unchanged.

Sessions are saved in `$XDG_STATE_HOME/sos/sessions`, or
`~/.local/state/sos/sessions`. Each directory contains `session.sqlite`, the
source of truth for typed messages, provider data and resume metadata, and
`info.json`, a readable cache of that metadata. The registry holds only weak
references to active sessions. A session can be attached to only one integration
at a time within this daemon.

SQLite 3.50.4 is downloaded from the official amalgamation with a pinned SHA-256
and statically linked into the daemon (`src/daemon/cmake/sqlite.cmake`). Its code
is in the public domain: https://www.sqlite.org/copyright.html. Users need
neither the `sqlite3` tool nor a system SQLite library.

`SessionJournal` stores each message individually with a lightweight summary.
WAL mode and `synchronous=FULL` provide synchronized transactional commits,
subject to the storage's guarantees. Messages and resume metadata are committed
together. A failed compaction no longer truncates the journal.
`info.json` is atomically replaced at creation, turn boundaries, setting changes
and explicit saves; it is not rewritten for every tool result. Failure to publish
it does not roll back a commit. It may be stale after an interruption and is
regenerated on resume.

Only a new database (version 0) or the current schema 6 is accepted. Older
schemas and corrupted databases are rejected without rewriting or replacement.
JSONL files and old metadata are never imported or modified.

`SessionHistoryService` exposes pages, Activities and details. Reading a body
does not load an entire batch. `SessionRecoveryIndex` maintains interrupted
calls, refusals and approvals within the message transaction. These reads scan
neither summaries nor bodies.
`SessionHistoryIndex` is the incremental display projection, updated in the same
transaction. The service reads pages through indexed anchors and ranges, without
rebuilding old groupings or loading all summaries. A collapsed Activity reads
only its aggregate. Old aggregates are not rebuilt, and bodies are not read to
fill in skill names. Large individual messages and expanded Activities (including
the preloaded last one) are still read in full. See
[storage tracking](docs/history-index-plan.md) for limits and remaining pagination work.

To back up a session, shut down the daemon cleanly and copy the directory,
or use the SQLite backup API. Do not copy only `session.sqlite` during a write,
and do not delete an open database's `-wal` and `-shm` files.
Only one daemon per sessions directory is still required.

The agent context is prepared on a worker at first send or `/compact`.
The last compaction allows direct reading of the initial instructions,
checkpoint, retained messages and subsequent messages. Without a checkpoint,
preparation still reads the entire context, but simple browsing does not.
The `execution.preparing` event displays `Preparing conversation…` in the TUI.
Cancellation and disconnection prevent installation of incomplete preparation;
no new user message is saved before acceptance.
Interrupted tool calls are still repaired without automatic execution.

Runtime events are forwarded through `asio::post` to the integration's own queue.
It saves and forwards each event in order, without making the worker wait for
disk writes, except for automatic compaction checkpoints, whose saving it awaits
before activating the new context, and Implementation construction boundaries.
Those boundaries are captured and saved on the worker's own SQLite connection
before the next tool, to preserve change ownership by substep. Closing requests
cancellation, processes remaining events and then releases the session.
Cancellation is cooperative and interrupts provider requests through the libcurl
progress callback, as well as interruptible tools. `execution.cancel` stops the
run without closing the session; interrupted and unexecuted calls are recorded
for resume. Local writes are synchronous on the Asio thread; SQLite transactions
use `synchronous=FULL` to request commit synchronization.

### Compaction

`/compact` sends `session.compact` to the daemon, only when idle and within an
existing session. `CompactionService` receives a `Conversation`, generates a
summary without tools using the selected provider/model/reasoning, and returns
a new active conversation. The old history remains intact in `Session` and
`session.sqlite`; the history service feeds the TUI, not the active context.
The summary is an assistant message carrying a `compaction` checkpoint with the
retained recent message IDs and session mode. The synthetic user request before
this summary exists only in the active context.

Selection preserves complete turns, without separating tool calls from their
results. The recent budget is estimated at one token per four JSON bytes, using
25% of the model's context limit capped at 30,000 tokens, with no floor, or 8,000
if the limit is unknown. At least the oldest turn is summarized. Tool results
in the transcript to summarize are limited to 2,000 UTF-8 bytes (without splitting
a character), and attachments are replaced by their type. The previous summary
is incorporated into subsequent compactions.

All summaries use a bounded mechanism of chunks followed by recursive merges.
Ordinary compaction requests at most 1% of the effective input window, with a
2,000-token budget floor, and less if possible. Internally, it accepts up to 5%,
with the same floor, without telling the model about this tolerance.
Both budgets remain capped by the model's output limit; summaries have no minimum
length. Each summarization allows three calls: an initial call and two size
corrections. Review summaries keep their separate budget and two corrections
followed by an emergency summary. The TUI counter remains hidden on the first
call of each chunk/merge; only corrections display `2 / 3` or `3 / 3`.
Keep the shortest candidate, then truncate at a UTF-8 boundary with a warning
if necessary. Provider errors, empty responses and cancellations are never
silently replaced by truncation. These budgets are byte heuristics, not exact
tokenization. The original history is never deleted.

Before each model call, the agent submits a `CompactionContext` (active
conversation, `ModelInfo`, tools and the boundary of the last measured usage)
to the preparation hook installed by the session.
`CompactionService::needCompaction` is a side-effect-free calculation; it also
accepts pending messages without adding them. The estimate uses actual input +
output usage from the last call, plus new messages at one token per four UTF-8
JSON bytes, rounded up. Without a measured boundary, it estimates the whole
context and tool definitions. Display and checkpoint metadata are excluded.
This is not a tokenizer and does not guarantee that limits will not be exceeded,
especially with media.

The threshold is the maximum input budget, otherwise the maximum context, minus
a reserve of `max(10% of the budget, 16,384)`. For a combined window, the reserve
also covers the known output limit. A missing/zero limit or a budget too small
for this reserve does not trigger automatic compaction. OpenRouter reports
context and `top_provider.max_completion_tokens`. With Codex authentication,
OpenAI reports `context_window` from the authenticated catalog, falling back to
`max_context_window`. Display prefers `max_context_window` without changing the
operational compaction budget; otherwise, it displays the current window.
The catalog is cached per provider instance and account; a loading failure is
propagated, not turned into an unknown image capability. Temporary network errors
are retried during image preflight and turn preparation; cancellation propagates.
Only a successfully fetched catalog without a declaration results in an unknown
capability. Codex-specific thresholds do not replace our reserve policy.
OpenAI API-key access still has no known limit, without inventing model capabilities.

If needed, the session summarizes using the current turn's provider, without
tools, and always preserves the last complete turn, even beyond the retention
budget. Additions are already saved locally but have not yet been sent to the
model. After the checkpoint is saved on the Asio thread, the worker activates
the new context and resumes the flow without a synthetic "Continue" message.
It does not recursively compact the summarizer. If no earlier history can be
summarized, generation fails, or the new context still exceeds the estimated
threshold, the turn fails without activating the summary. This version does not
automatically recover from context errors returned by the provider.

Saving the checkpoint precedes activation. Failure and cancellation delete no
messages and activate no partial summary. Cancellation and connection closure
interrupt the provider worker. `/resume` rebuilds context from the last checkpoint
without calling the model again. Retained message IDs remain stable; their
display data remains intact, but their provider replay becomes portable to avoid
reusing opaque reasoning that depends on removed context.
`provider_context_start` identifies a boundary in the full history, translated
into the active context on loading. The context counter resets to unknown after
compaction until the next normal generation, without confusing summarizer usage
with the agent's context.

### Permissions

`/permissions` selects a policy independent of Sane/Slop: `ask`, `auto` or
`danger`. The global mode is saved as `permission_mode` in `config.json` and
applied to tool checks during the current turn; `/new` and `/resume` do not
replace it. Switching from Ask to Auto or Danger reevaluates a pending request.
The selector uses `DialogScreen` and briefly describes the highlighted mode.
Shared `dialog_row` rows display `Current` (permissions and models) and
`Connected` (providers and authentication methods) on the right in coral (`BOLD_MD`).
Danger removes confirmations, never Sane prohibitions or operating-system limits.
Its warning explicitly mentions file loss and recommends AI Check when in doubt.

AI Check intervenes where human permission would otherwise be required.
`PermissionVerifier` uses TypeSafe System One or OpenAI Decisions, with a
configurable endpoint and model and an allow/deny `choice` question. The prompt
allows routine development in the repository, without overzealous scrutiny of
source content, and targets concrete risks: production, personal files, system
damage and exfiltration. Running a command from the repository does not make
its external effects safe.
The global integer `permission_confidence` threshold (0–100, default 80) retains
its name for compatibility but means the minimum probability of `allow`, without
reading or inverting the API's `confidence` field. Saved values remain unchanged,
with the new meaning explained in the TUI. At 0, there is no verifier call, like
Danger without bypassing Sane; at 100, the probability must equal 1.
These estimates are neither security guarantees nor a shared calibration across
models. Invalid distributions and errors can never authorize an action.
A tool error explains to the model that the action was not executed and that it
must not retry or work around the refusal without manual approval. It can suggest
opening Activity, expanding the action under `Refused tools`, clicking `Approve`,
then sending a new message; otherwise it continues without that action.
This instruction is passed to the model, not enforced by stopping the agent loop.
Unlike a human `no`, this refusal does not cancel the turn.
Auto decisions directly emit `permission.resolved` with `ai_accepted` or
`ai_refused`, then are retained in the result's `metadata.permissions`.
The optional `confidence` field (0–1) retains its name for compatibility and
carries the authorization probability for new decisions. Histories retain their
original recorded scores. A missing score is not zero.
Decisions are displayed live and after resume; they are never accepted as human
responses. The request is cancelable and limited to 30 seconds; the permission
lock is not held during network access. Sane is rechecked after the call.

The submitted context retains full Bash arguments. For `write`, `edit` and
`apply_patch`, an allowlist retains only resolved paths and operations
(creation/modification and existence for write/edit; addition, modification,
deletion and movement for patches). Content, raw arguments and diffs are excluded,
including during checks outside the workspace. Jev evaluates targets, not the
behavior of the code being written. Local diffs remain unchanged.
The context includes the workspace and at most 2,000 UTF-8 bytes of the last
user message. No full history or tool output is added. A state exceeding 48,000
bytes is rejected without a remote request, rather than truncating the action.
Commands, paths and user excerpts may contain private data.
Model configuration and thresholds are in config.json; dedicated keys, masked
in the TUI, remain in auth.json protected with 0600 permissions. They are tied
to the protocol and endpoint, without replacing conversation credentials.
Historical Jev/OpenRouter configuration retains its destination and model.

AI Check displays the model, percentages, `Save` and `Change model`. Enter on
the row opens settings without activating the policy; only Save applies the
full draft. Left/right selects actions; in the numeric field these arrows adjust
the value. Hide these settings for Ask or Danger. Threshold 0 remains available
with a warning. Save closes after acknowledgement; an error leaves settings
editable. Escape closes without saving and clears the key.

Change model opens the API choice (OpenAI Decisions / TypeSafe), then fields
for the full endpoint, free-form model and masked key. Use model returns to the
parent with a draft, without writes or network calls. A saved key may be left
blank only for the same protocol and endpoint. Fields never receive the daemon's
key. Require `info.permission_check_version: 1` to prevent an old daemon from
silently ignoring the model or the new percentage semantics.
See the [product contracts](docs/product-contracts.md#ai-check) and
[protocol](docs/daemon-protocol.md) for the detailed contract.

AI refusals are also displayed under `Refused tools` in a separate Activity
section, above a Muted separator aligned with the arrows. An additional Slop
square remains on Activity, even after approval and before historical details
are loaded (activity type `refused`). The section below this separator is titled
`History` in coral, aligned with `Refused tools`; these titles and the separator
remain hidden when there are no refusals or when collapsed.
The lower area remains the immutable journal.
The row displays `tool_name • Confidence X%` in Slop, or `Confidence unavailable`
for older refusals and verification errors. The `Approve` button becomes
`Revoke approval` after approval; ` -> Manually approved` is appended to the title,
entirely in Algae, without changing the historical score. Revoking removes the
suffix and returns the title to Slop. The button is created only when expanded
and removed when collapsed; empty context/error fields are hidden.
The button emits `permission.manual`: the daemon retrieves arguments from the
refused result, without trusting arguments supplied by the TUI. The toggle adds
a `manual_approval` system audit event to the journal; it does not modify the
running agent or execute anything.
On the next message, a chronological report describes toggles since the last user
message. Approvals are loaded into the manager at that boundary and remain valid
for unlimited attempts with identical tool, arguments, directory and workspace,
in both Ask and Auto, without bypassing Sane. Revocation removes that approval
on the next turn. They are session-specific, restored on resume and independent
of connection-specific `always` approvals. Authorized attempts are marked
`manually_approved` in their own result, never in the initial refusal.
SQLite summaries retain refusal IDs, audit events and model-call counts/durations
for average throughput per Activity; arguments remain in the full journal.
Older refusals without a recorded target are visible but have a disabled button.

The system report also provides session-specific replay IDs, and active approvals
are recalled on every turn, including after compaction/resume.
`rerun_approved_action` accepts only an ID, then retrieves the recorded tool,
exact arguments, directory and workspace. No argument can be overridden.
The worker's resolver uses its own SQLite connection, checks that the ID was
available at the start of the turn and rechecks revocations at permission
boundaries. Replay bypasses Jev, never Sane or cancellation. It remains reusable
until revoked and is not exposed to BTW branches. The provider journal retains
the replay call; `metadata.approved_action` lets Activity show the actual tool
and its arguments. No SQLite migration is required.

The following paragraphs describe the base **Ask before** policy.

The daemon has a `PermissionManager` per connection, with a typed Sane/Slop mode,
the initially authorized workspace and temporary approvals. The displayed mode
changes immediately, but active-turn permissions remain unchanged; the new mode
applies to the next user message.
The mode saved in SQLite and reflected in `info.json` corresponds to the last
user-message launch and is restored on attachment; `mode` is required in current
metadata. The initial prompt explains both modes and announces the first message's
mode. Sending in a different mode adds a system announcement just before the user
message, then updates the session mode. No announcement is added on switching
or between model calls in a running turn; the existing prefix remains unchanged.
Old sessions retain their original prompt.
`always` approvals remain connection-specific and are not saved with the mode.
The TUI configures this workspace at startup; resuming a session does not change
this authorization root. In Sane, `write`, `edit`, `apply_patch` and mutating or
unknown bash commands are refused. In Slop, these operations require confirmation.
Detected external access requires confirmation in both modes.
`webfetch` and internal reads/searches remain allowed.

Permission request, response and trace types live in
`agent-runtime/tools/permission.hpp`. The runtime identifies the tool call and
performs a preflight check of forbidden families. The daemon's manager emits
`PermissionEvent` to the Asio queue and waits for a cancelable response on the
worker. The TUI presents a card in the chat, then retains the decision on the
activity. A user refusal cancels the entire turn; a Sane-policy refusal is a
tool error result. Decisions are saved in result metadata, never reusable
authorizations.

`Always yes` remembers, for this connection only, the directory and its
descendants, the entire editing family (`write`, `edit`, `apply_patch`), or the
displayed command scopes. Access to the launch workspace and its descendants
is allowed from the start; this access right does not waive editing or bash
command approval.
The Sane restriction takes priority. Editing tools release their lock while
waiting for the human, then recheck files under the lock.

Bash is parsed before execution: a conservative list of read commands, detection
of redirections, substitutions, unknown commands and explicit external paths.
Unknown programs retain exact approval rather than authorization for their
entire interpreter. As in OpenCode, this policy is not a sandbox: computed paths
and the internal effects of scripts or program configurations cannot be fully
inferred.

### Ripgrep and filesystem tools

`core::ensureRipgrep()` searches for `rg` in the `PATH`, then in
`$XDG_DATA_HOME/sos/bin` (or `~/.local/share/sos/bin`). If missing, it downloads
version 15.1.0 from official ripgrep releases and keeps its own copy, without a
system installation. This first version covers macOS and Linux on arm64/x86_64
and uses `/usr/bin/tar` for extraction.

`glob` and `grep` use this binary without a shell and limit results to 100.
Process calls are limited to 30 seconds and 8 MiB of output per stream.
Search tests require `rg` in the `PATH`; they do not trigger network downloads.

Tool contracts follow those of the OpenCode clone, without LSP integration or
automatic formatting. `edit` applies an exact match, then a line-based fallback
that ignores leading/trailing whitespace; other OpenCode heuristics are not
reproduced. Permission previews and results from `write`, `edit` and `apply_patch`
share a line-based unified diff, with three context lines and separate hunks
for distant changes. The bidirectional Myers calculation retains two search
frontiers, without a quadratic LCS table or an external executable dependency.
Line endings, BOM and a missing final newline remain represented in the diff;
preview calculation does not change the logic that applies changes.
`apply_patch` prepares all changes before permission/writing, but is not a
multi-file transaction in case of an I/O error during application.
Existing add/move destinations and repeated paths within the same patch are
rejected.

### Common tools

`tools/common/bash` exposes the OpenCode arguments `command`, `workdir`, `timeout`.
It runs `/bin/bash -c`, with stdin closed and a new shell per call. The default
timeout is 120,000 ms. stdout and stderr are merged, the exit code is returned,
and timeout/cancellation terminates the process group. Output beyond 2,000 lines
or 50 KiB is stored in `<workspace>/.sos/tool-output/`; the tool returns its tail
with the path to the complete output.
Tree-sitter analysis and permission policy are applied before execution.
There is no PowerShell/cmd selection or live output preview.

`tools/common/webfetch` exposes `url`, `format` (`markdown` by default, `text`,
`html`) and `timeout` in seconds (default 30, capped at 120). It uses CPR,
follows at most ten HTTP(S) redirects within the same time budget and limits
received/decompressed content to 5 MiB. JPEG/PNG/GIF/WebP images become attachments.
Long text output is saved in the same directory, with its beginning returned
to the model. HTML conversion uses libxml2 and covers common structures; it
is not a complete reproduction of OpenCode's Turndown.
HTTP URLs remain HTTP, as in the consulted OpenCode code; no browser, JavaScript
or anti-bot bypass mechanism is used.

## Tests

Use GoogleTest for C++ tests. Tests are registered in CTest and built with the
project when `BUILD_TESTING` is enabled (the default).
CMake uses an installed GoogleTest (1.17.0 minimum) or downloads version 1.17.0
through `FetchContent`.

After building, the agent runs:

```sh
ctest --test-dir build-ai --output-on-failure
```

The human uses the same command with `--test-dir build`.

### Python/Textual tests

Textual tests are a `unittest` suite separate from CTest. To prepare dependencies
without packaging or user installation:

```sh
UV_PROJECT_ENVIRONMENT="$PWD/build-ai/tui-venv" \
  uv sync --project integrations/tui --frozen --no-default-groups
```

The fast runner uses at most four processes by default. It distributes individual
cases, including those from large files, and isolates each batch's XDG directories.
Asyncio debugging is disabled, but assertions, Textual interactions, animations
and `unittest` cleanup remain active.

```sh
# Full suite. Build build-ai/bin/sosd to include real daemon tests.
build-ai/tui-venv/bin/python integrations/tui/run_tests.py

# A specific file, class or case; -j 1 preserves sequential order.
build-ai/tui-venv/bin/python integrations/tui/run_tests.py -j 1 \
  test_implementation_workspace.ImplementationLogicTest

# Full asyncio diagnostics, also available directly through unittest.
build-ai/tui-venv/bin/python integrations/tui/run_tests.py -j 1 --asyncio-debug test_chat

# Reuse timings from a previous run to distribute work more evenly.
build-ai/tui-venv/bin/python integrations/tui/run_tests.py \
  --timings-from build-ai/test-measurements/tui.json
```

`--jobs N` sets parallelism; `--pattern 'test_*e2e.py'` filters files and `--list`
displays selected cases. The default report is
`build-ai/test-measurements/tui.json`, with wall/CPU times and individual statuses,
accompanied by a `.log` containing details. `--report path.json` allows keeping
multiple measurements. Failures, errors and unexpected successes produce a
nonzero exit code; skipped tests are reported, not declared successful.

For new checks, keep pure calculations in `TestCase` classes without mounting
the application. Reserve full fixtures for interactions that need them.
Test timeouts by checking the recorded delay and triggering its callback rather
than waiting several real seconds. Fixtures using Unix sockets must choose a
short temporary path independent of checkout depth. Do not globally disable
Textual pauses or animations to save time.

Native tests can also be explicitly parallelized, for example:

```sh
ctest --test-dir build-ai --parallel 4 --output-on-failure
```
