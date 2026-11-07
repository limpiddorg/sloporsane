# Native FTXUI integration

`sos-ftxui` is a C++23 terminal client using FTXUI 6.1.9. The application, transport,
state handling, image preparation and tests contain no Python. It is an additional
integration, not a launcher around Textual. `integrations/tui/` and the `sos`
command are unchanged.

## Build

Run CMake from the repository root:

```sh
cmake -S . -B build -DSOS_BUILD_TEXTUAL=OFF
cmake --build build --target build-ftxui
./build/bin/sos-ftxui
```

`SOS_BUILD_TEXTUAL=OFF` removes the `uv`/PyInstaller requirement from configuration
and build. `SOS_BUILD_FTXUI` defaults to `ON`; turn it off to retain a Textual-only
build. FTXUI is found locally or fetched at the pinned release. CommonMark/GFM
uses statically linked cmark-gfm 0.29.0.gfm.13, with no Python renderer. The daemon's
existing native dependencies are still required. Linux and macOS are supported by
the transport; this implementation uses Unix sockets and POSIX process APIs.

## Install

```sh
cmake --build build --target install-ftxui
sos-ftxui
```

The explicit installation target creates a versioned application under
`$XDG_DATA_HOME/sos/ftxui/` (default `~/.local/share/sos/ftxui/`) and a launcher in
`~/.local/bin/sos-ftxui`. Versions are keyed by the hashes of both executables, so
rebuilding does not overwrite an executable used by an existing daemon. Old
versions are not removed automatically. This target does not replace `sos` and is
not part of the default build's installation side effects.

Override `SOS_FTXUI_INSTALL_DIR` and `SOS_FTXUI_BIN_DIR` to select other destinations.
A conventional `cmake --install build --component ftxui --prefix /destination`
installs `bin/sos-ftxui` and `bin/sosd` without creating the user launcher. Native
shared libraries such as OpenSSL must remain available on the destination system;
this is not the self-contained PyInstaller archive.

Agent verification stays inside `build-ai/`:

```sh
cmake -S . -B build-ai \
  -DSOS_BUILD_TEXTUAL=OFF \
  -DSOS_FTXUI_BIN_DIR="$PWD/build-ai/ftxui/bin" \
  -DSOS_FTXUI_INSTALL_DIR="$PWD/build-ai/ftxui/installed"
cmake --build build-ai
ctest --test-dir build-ai --output-on-failure
```

## Connection

The client connects to `/tmp/sosd.sock`, or starts the sibling `sosd` under a
per-socket startup lock. Non-socket files are never removed. It validates protocol
**7**, answers heartbeats on its independent network thread, and sends
`integration.close` on exit. It does not silently reconnect a lost conversation.

```sh
sos-ftxui --socket /path/to/daemon.sock
sos-ftxui --socket /path/to/daemon.sock --daemon /path/to/sosd
sos-ftxui --socket /path/to/daemon.sock --check
```

`--check` waits for the handshake and workspace-configuration acknowledgement,
then closes without creating a session or contacting a model provider.

Credentials, model preferences and sessions belong to the existing daemon. The
client never reads API keys from environment variables or writes them to session
messages. API-key fields are masked, cleared after submission and excluded from
chat. The workspace is the directory from which the command was invoked.

## Controls

| Control | Action |
| --- | --- |
| Enter | Send, or add to the pending message while working |
| Ctrl+J / supported Shift+Enter encodings | Insert a newline |
| Tab | Switch Sane/Slop; cycle slash-command suggestions when present |
| Ctrl+T | Persist the daemon's next supported reasoning level |
| Escape | Restore pending input first; otherwise arm cancellation |
| Escape twice within three seconds | Cancel the principal execution |
| Ctrl+C | Clear input; with empty input, confirm exit on a second press |
| Ctrl+Q | Close immediately |
| Ctrl+G | Edit the draft using `$EDITOR`, without sending it |
| Ctrl+Z / Ctrl+Y | Undo / redo text, compact paste and image changes |
| Ctrl+A / Shift+arrows | Select the draft / extend its selection |
| Page Up / Page Down | Scroll; request older/newer history at the boundary |
| Mouse wheel | Scroll history |
| Click an activity/tool/reasoning header | Expand or collapse it |
| Left / Right and Enter during a permission | Select Yes / Always yes / No |
| Ctrl+V | Capture a native PNG clipboard image; disabled over SSH |
| Ctrl+O | Inspect attached-image metadata; Enter removes the selected image |
| Select text with the mouse | Copy through OSC 52 and the native clipboard when available |
| Click a compact paste block | Open its full text, with Close and Remove |

Bracketed multiline paste is collected as one draft edit, never dispatched as
multiple messages. Pastes of at least three lines or 200 Unicode characters become
gold compact blocks, each with its own original content even when previews match.
Arrow keys and deletion treat a block atomically. Sending, queuing, copying and
external editing expand the original text. Rejections restore plain original text
without changing blocks in a newly typed draft. An unchanged editor file preserves
the compact blocks. The composer wraps to the terminal width and grows to six rows.

Pasting an existing PNG/JPEG/WebP path captures its bytes
immediately. The runtime's image validator enforces the same format, dimension,
per-image and per-message limits as the daemon. Image-only input, pending images
and restoration after rejected sends are supported. Clipboard image capture uses
`osascript` on macOS, or installed `wl-paste`/`xclip` on Linux.

## Commands

- `/connect`: OpenRouter API key, or OpenAI API key/browser/device authorization.
  OAuth URLs and instructions remain visible; Enter opens the browser. Polling and
  authentication are cancelled when the dialog closes.
- `/model`: local case-insensitive filtering, provider/model selection and a
  validated context budget of at least 32,768 tokens.
- `/permissions`: Ask before, AI Verified with Jev, or Danger. Auto supports a
  replacement masked key or the saved dedicated key. Danger's warning is displayed
  before selection; Sane restrictions remain daemon-enforced.
- `/new`: detach the saved conversation and reset to the initial Sane view.
- `/resume`: list sessions, preview the latest question and atomically install the
  validated history window. Activity rows and full tool details are separate loads.
- `/compact`: run the daemon's manual compaction without deleting display history.
- `/recap`: request a persistent conversation recap. The client also requests one
  after five continuous idle minutes, respecting `last_history_is_recap`.
- `/btw [btw-N] question`: create or continue a persisted, read-only side conversation.
  Its lifecycle is independent of the principal execution.

Rejected or disconnected unaccepted messages restore their draft and image bytes.
A pending message is dispatched only after a terminal success, never after an
intermediate response or automatic compaction. Cancellation/failure restores it
instead. Historical refused tool details expose Approve/Revoke approval; these
send only the recorded refusal ID and never execute a tool locally.
Refusals appear in a separate mutable **Refused tools** section above the immutable
**History** transcript. Activity headers retain their refusal marker after approval.

During `$EDITOR`, only terminal rendering is suspended. Incoming execution events
continue to be applied, heartbeats are answered and an explicitly queued message
can start. Restored text is prepended to the edited draft when the editor closes;
the draft itself is never submitted implicitly.

## Implementation

- `client.*`: bounded JSONL framing, serialized writes, heartbeat reader, startup
  locking and daemon lifecycle.
- `model.*`: typed display state, optimistic drafts, queued attachments, activity
  projection, branch state and strict ordered history transfers.
- `app.*`: FTXUI components, dialogs, command dispatch and terminal interactions.
- `draft_buffer.*` and `composer.*`: tracked paste spans, shared image snapshots,
  undo/redo, Unicode selection, wrapping and atomic editing.
- `history_viewport.*`: row-based scrolling, shared-message anchors across page
  swaps and reflow, and last-turn alignment. Retained details and rendered bodies
  are reused while they remain in the active window; a new turn purges old details.
- `markdown.*`: CommonMark/GFM AST rendering, including nested emphasis, reference
  links, tables, task lists and code. Syntax colors use the Textual palette.
- `tool_view.*` and `status_bar.*`: tool-specific sections, colored diffs, immutable
  permission decisions, context counters, responsive field priorities and animations.
- `attachments.*`: image loading, canonical base64, clipboard capture and browser
  invocation. Pixel validation is reused from `agent-runtime`.
- `clipboard.cpp`: bounded native clipboard helpers, with private temporary files
  removed after copying. SSH only uses OSC 52.

## Verification

`FtxuiTransfer.*`, `FtxuiModel.*`, `FtxuiAttachments.*` and `FtxuiMarkdown.*` cover
state transitions, malformed/truncated transfers, lazy details, image recovery
and rendered content. `FtxuiE2E.*` starts the actual `sos-ftxui` executable in a
pseudo-terminal and exercises keyboard input, dialog focus, masked credentials,
model selection, permissions, queuing, cancellation, paste and image restoration.
Additional tests exercise anchor retention, paste identity and Unicode editing,
image undo, narrow layouts, native GFM rendering, manual approval by mouse and
queue/rejection processing while an external editor remains open. PTY tests never
write to the developer's desktop clipboard.

Three backend levels are deliberately distinguished:

- A scripted protocol peer makes error and race paths deterministic.
- The real `sosd` verifies startup, provider listing and final socket removal.
- `ftxui-fixture-daemon` uses the actual Integration, Session, journal and Agent
  implementations, replacing only the remote provider with a deterministic C++
  fixture. A full UI send/generate/save/new/resume round trip checks SQLite
  persistence without credentials, external requests or provider charges.

## Verification limits

Rendering is independently implemented with FTXUI and cmark-gfm, not by embedding
Textual, Rich or Pygments. The native lexer colors common coding languages and
leaves unknown languages in plain text; token-by-token equivalence with every
Pygments lexer is not claimed. Exact line wrapping and glyph appearance also
depend on terminal font/width handling. Native clipboard and browser integration
depend on installed desktop applications; OSC 52 depends on terminal settings.

The automated suite does not authenticate against live OpenAI/OpenRouter accounts
or certify every terminal emulator. Its deterministic end-to-end tests cover the
native integration and the real local backend, not remote service availability.
