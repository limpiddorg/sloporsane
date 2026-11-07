# MCP client construction

## Current status

`/mcps` opens the Textual management screen. The table shows Server, Scope,
Connected and Enabled, with independent checkmarks for connection and availability.
The selected server offers Connect/Disconnect and Enable/Disable only.
Create, edit and delete entries in the workspace or global `mcps.json` file, then
reopen `/mcps` to reload the list. The compact footer offers Connect/Disconnect and
Enable/Disable, without a Close button. Keyboard help shows Up/Down to select a
server, Left/Right to select an action, Enter to confirm and Escape to close.
Opening the dialog selects the first server and Connect/Disconnect by default.
The chosen action uses the same blue background and light text as selected tabs;
the action buttons accommodate their longer Disconnect/Disable labels.
There is no Refresh button;
management responses update the list. An idle open dialog polls every two seconds
to reflect choices made in another integration; it never interrupts authorization.
Remembered connections restore in the background after `integration.configure` returns,
so startup and `/mcps` do not wait for handshakes. During restoration `/mcps` shows
Connecting; a turn started meanwhile already has its Activity spinner and waits for
restoration before preparing the MCP tool catalogue. Failed connections remain visible
in the list and trigger the input error band.

The Textual composer also accepts messages before the local daemon handshake and
integration configuration finish. The user turn and Activity appear immediately;
the message is sent once configuration succeeds, then uses the same turn while
waiting for MCP restoration. Startup cancellation restores the unsent draft and
attachments locally; connection failure restores pending and queued drafts without
replaying them. Slash commands remain in the composer until the daemon is ready.

Connect is a single user journey: confirm the exact configuration if it is not
approved, attempt connection, enter a masked token or authorize in the browser if
required, then finish connecting. During OAuth discovery the UI displays progress;
once ready, a separate modal above the compact management dialog shows the authorization
URL and Open in Browser, including when automatic browser opening fails.
Escape returns to management and cancels OAuth; successful authorization
closes the modal and finishes connecting. Configuration approval and masked token entry
each open a separate modal without expanding the management dialog. Enter confirms
the approval or token, without a redundant confirmation button. Escape
returns to the list without continuing that journey. Existing credentials are reused. Cancellation
stops the journey; it does not undo approval or configuration already saved. A
failed authorization is not retried in a loop and rejected tool calls are not replayed.

Connected and Enabled are independent. Disable hides tools from the model and
rejects new calls using previously advertised bindings without closing the transport.
Already-running calls may finish. Connect/Disconnect and Enable/Disable choices
are saved immediately in `$XDG_CONFIG_HOME/sos/mcp-preferences.json` (default
`~/.config/sos/mcp-preferences.json`), atomically with permissions `0600`.
There is one global record per server name, not per workspace, identity or session.
All integrations in a daemon share it. Configuring an integration restores wanted
connections, including disabled ones, before the first model turn. An absent name
does not create a server. Each workspace still uses its own effective configuration,
approval and credentials; sharing a choice never grants trust or shares secrets.
No browser is opened by restoration. Missing authentication appears as `needs_auth`.
Disconnect persists across restarts. A failure retains the connect choice for the
next daemon lifetime but is not retried automatically within the current lifetime.
An explicit Connect retries the named server across loaded workspaces, subject to
each configuration's approval. Only the selected configuration runs interactive auth.
Enablement choices override file defaults without rewriting either config file.
Without saved choices, file defaults and enabled/approved connection at first use
remain in effect. Closing sessions or shutting down the daemon does not save a
disconnected choice. Separate daemon processes read the persisted choices on startup;
live synchronization is within one daemon, not between independent daemons.

All server configuration is file-based; there are no Add/Edit/Delete controls. There is no resource/prompt browser, secret-environment
editor, OAuth settings editor or token-revocation UI. OAuth itself remains part of
Connect. Secrets remain separate from shareable configuration and conversations.
Deleting configuration is not remote OAuth revocation.

Implemented components in `src/daemon/mcp/`:

- `ConfigStore`: typed stdio/HTTP configuration, validation and workspace override.
- `AuthStore`: credentials and explicit local trust, bound to configuration identity.
- `Preferences`: global per-name choices and immediate publication to live connections;
  preferences never contain credentials or alter approval identities.
- `JsonFile`: bounded reads, locked updates and atomic private file publication.
- `Client`: initialization, tool discovery with pagination, JSON-RPC correlation,
  ping responses and tool calls. Sampling and elicitation are not advertised.
- `StdioTransport`: managed subprocess, bounded newline-delimited JSON, explicit
  environment, deadlines, cancellation checks and process-group shutdown.
- `HttpTransport`: Streamable HTTP POST, JSON or SSE responses, session and
  negotiated-version headers, optional explicit bearer token, bounded transfers.
- `Manager`: per-workspace configuration/catalogue ownership and automatic-attempt
  memory. Delegates live operations rather than manipulating client state.
- `ServerConnection`: one immutable server configuration, its state and client.
  Owns serialization, cancellation, trust/credential transitions and binding checks.
- `Connections`: daemon-wide ownership, shared by canonical workspace, and binding
  of the manager to agent tool sources in the daemon factory.
- `ToolAdapter`: runtime definitions, SOS permission requests and checkpoint hooks.
  `makeTools` captures connected tools for a registry. `renderToolResult` separately
  validates and converts protocol content, without network or connection state.

The runtime now offers `Agent::setToolSource`: an idle-installed callback invoked
on the worker before each generation. `ToolRegistry::replaceDynamic` validates a
complete replacement before publishing it and cannot shadow built-in tools. The
catalogue is held fixed through compaction, provider retries and all tool calls in
the response, including approved-action replay. Subsequent generations reload it.
Source failures stop generation rather than silently advertising stale tools.
`tests/agent_test.cpp` covers these boundaries and cancellation during loading.
This callback does not itself connect servers or discover MCP configuration.

The configuration `env` field is public; secrets belong in the separate credential store.
The client offers and accepts only MCP 2025-11-25.
Version negotiation does not imply all optional extensions: sampling, elicitation,
roots and experimental tasks are not advertised. Unknown server requests receive
JSON-RPC method-not-found, never implicit permission to run an action.

## Ownership and lifecycle contracts

`Connections -> Manager -> ServerConnection -> Client -> Transport` is the
ownership chain. The adapter only addresses a manager with a frozen tool binding;
it does not manipulate a connection. Storage (`ConfigStore`, `AuthStore`, `JsonFile`)
does not launch processes. Fingerprinting and environment validation are shared
functions, without manufacturing dummy server configurations.

- `Manager` locks its catalogue, not network operations. Retired entries are
  destroyed outside that lock; in-flight callers keep their own shared reference.
- `ServerConnection` has a short state lock, an operation lock and an administration
  lock. Stop/trust/authentication signal cancellation first and retain the operation
  lock through their update. A revision invalidates connects queued before a stop.
  Retirement is irreversible. Stale identity checks precede cancellation, so a stale
  approval cannot disconnect a replacement server. No user cancellation callback is
  invoked under the state lock.
- `Client` transitions from NEW to READY or CLOSED; CLOSED never reinitializes.
  Malformed envelopes/catalogues close it. A valid JSON-RPC rejection (`RpcError`)
  and a tool's `isError` result do not close an otherwise healthy connection.
- `Transport` requires serialized caller access. Its internal HTTP worker is an
  implementation detail, not permission to call close concurrently. Cancellation
  flows through checkpoints; close is terminal and idempotent. HTTP only publishes
  its terminal response after transfer validation. Stdio shutdown retains the
  unreaped process leader until the final group signal, including when children
  ignore SIGTERM.

Owners must drain workers before destruction. Settings and credentials are distinct
atomic files, not a cross-file transaction: an error saving credentials can leave
the configuration saved and the connection closed. Parent directories and external
process effects are not sandboxed. These are explicit boundaries, not guarantees
provided by the lifecycle locks.

## Configuration

Workspace configuration: `<workspace>/.sos/mcps.json`.
Global configuration: `$XDG_CONFIG_HOME/sos/mcps.json` (default
`~/.config/sos/mcps.json`). A workspace entry replaces an entire global entry of
that name, including when the workspace entry is disabled or invalid. A malformed
configuration document prevents loading servers, rather than revealing potentially
shadowed global entries.

```json
{
  "mcpServers": {
    "local-example": {
      "command": "example-server",
      "args": ["--stdio"],
      "env": {"PUBLIC_OPTION": "value"}
    },
    "notion": {
      "url": "https://mcp.notion.com/mcp"
    }
  }
}
```

This accepts the common `mcpServers` JSON convention, including Notion's direct
HTTP example and its `command: "npx"`, `args: ["-y", "mcp-remote", URL]` example.
It is not an importer for Codex TOML or every vendor-specific field. `command` is
an executable string, `args` an optional string array, and `env` an optional public
string map. No shell splitting or environment-variable interpolation is performed.
Keep secrets in the private credential store, not in `env` in a shared file.

`url` implies Streamable HTTP; `command` implies stdio. `type` remains optional;
contradictory fields and legacy `type: "sse"` are rejected. No `version` is required.
`enabled` defaults to true, but approval is separate and required before any
connection or subprocess launch. Copy-pasted entries need no explicit activation field.

HTTP authentication defaults to `auto`: try without credentials until authorization
is required, then expose `needs_auth`. The explicit Connect journey handles OAuth
or a configured bearer token. No browser opens during autonomous preparation,
and no rejected tool request is replayed. Saved OAuth
credentials are reused and refreshed. Explicit `authentication` values `none`,
`token`, and `oauth` remain available; token servers should configure `authentication: "token"`.
OAuth accepts optional `client_id`, `oauth_scope`, and `callback_port` (0 selects
an ephemeral port). Without a client ID, SOS uses dynamic client registration.
No legacy HTTP+SSE transport is planned. HTTP is restricted to loopback; other
hosts require HTTPS. URL userinfo and fragments are refused.

Only `mcpServers` documents are supported, with string `command`, array `args`
and public `env` for stdio. Old `version`/`servers` documents, command arrays and
the `environment` alias are rejected without rewriting the file. Missing
`enabled` still means true; remote authentication defaults to automatic.
The credential store requires the current ten-field record, including empty
OAuth fields for simple bearer tokens. Incomplete older records are rejected
without modifying secrets or trust.

Name, scope, destination and connection settings are hashed into an identity.
Workspace servers and local processes include the canonical workspace path, even
for globally configured local processes. Enablement and timeout changes do not
invalidate that identity. This identifies configuration, not executable bytes or
the remote server's actual identity.

## Browser OAuth

For Notion: paste the `mcpServers` example above into the workspace or global
`mcps.json` file. Choose Connect, confirm approval, and complete Notion
authorization in your browser when asked. SOS finishes connecting without a separate
Credentials action. Leave client ID and scope absent for discovery. This path
matches Notion's public metadata; real account consent and a real Notion tool call
still require a user test. Automated tests use a loopback OAuth fixture, not an account.

`oauth.*` orchestrates protected-resource/authorization-server metadata, S256 PKCE,
public dynamic registration, authorization code exchange and refresh.
`oauth_http.*` is bounded HTTPS transport (HTTP only for loopback tests), with no
redirects and no raw server errors in diagnostics. `oauth_callback.*` owns an
IPv4 loopback listener on an ephemeral port, exact state/path checks, bounded
requests, denial handling and a ten-minute authorization deadline. The browser
and daemon must run on the same machine, or the user must arrange forwarding.
No port is exposed on external interfaces. Callback sockets are close-on-exec.

The integration owns one flow, cancels and drains it on close, and rechecks the
configuration identity and trust before persisting credentials. OAuth does not
implicitly enable or approve a server; the explicit TUI Connect journey connects
after successful authorization. Escape/cancel discards the flow;
saved configuration is not rolled back. URL opening runs off the Textual event
loop, with a displayed URL and Open browser button for manual recovery.

`OAuthCredentials` serializes refresh per identity within the daemon and checks
expiration before HTTP POST, including tools/call. Tokens within 30 seconds of
expiration are refreshed at the **persisted** endpoint, never one rediscovered
while holding a refresh token. Storage uses a compare-and-replace under its file
lock: changed credentials or revoked trust cannot be overwritten. Rotated refresh
tokens are saved before sending MCP traffic. Cross-daemon refresh is not serialized;
use one daemon for a credential store. A refused request is never replayed; refresh
failure requires explicit recovery. Cancellation after a server rotates a token
but before local persistence may require reauthorization.

Discovery first probes the MCP endpoint without credentials and parses Bearer
`WWW-Authenticate` challenges (including mixed schemes). An explicit
`resource_metadata` URL takes precedence over path-specific then root well-known
URIs. Required challenged scopes are combined with explicitly configured scopes;
metadata defaults are used only when neither is present.
Resource metadata is required; SOS never guesses an authorization server when it
is missing. RFC 8414 discovery is followed by OIDC path insertion, then OIDC path
appending. The resource must cover the MCP endpoint on the same origin; issuer
must match metadata and HTTPS cannot be downgraded. The first advertised issuer
is selected. A runtime 401/403 challenge is retained on that connection. Choosing
Connect starts step-up authorization when required with its required scopes and
metadata location (plus initial/configured scopes). A successful connection clears
the challenge. There is no autonomous browser flow and the rejected tool call is
never replayed: retry is an explicit user/model action after authorization.

Public dynamic registration, pre-registered public clients and confidential clients
using `client_secret_basic` or `client_secret_post` are supported. Set a registered
client ID and callback port in the configuration file; the callback URI is
`http://127.0.0.1:PORT/callback`. Confidential-client credentials are supported by the daemon credential API,
not exposed by the simplified TUI. Neither is included in shareable configuration.
Changing client credentials discards previous tokens and requires authorization.

An HTTPS client-ID metadata URL can be supplied in the same client ID field.
SOS validates `client_id`, `client_name` and that the registered redirect URI
matches its listener. Hosting that document at a public HTTPS URL is an external
deployment prerequisite, not something SOS publishes or invents. Use a matching
fixed callback port for a document with a fixed redirect URI. There is no implicit
SOS-hosted client document or private-key JWT identity.

The daemon `mcp.oauth_revoke` API disconnects, then uses the stored
RFC 7009 endpoint. Refresh/access token revocation is serialized with refresh and
credentials are cleared only after successful responses. Configuration approval
and client registration remain intact. A server without a revocation endpoint
produces an explicit error; revocation must then be done in its account settings.
Partial remote revocation or cancellation can require reauthorization. Removing a
configuration only removes local state; it does not assert remote revocation.

## Resources and prompts

Resources/prompts are not exposed in the TUI. The underlying daemon catalogue and
preview APIs remain available and tested, but are not exposed as model tools.
They do not inject content or modify a conversation on notifications. Removing
the UI does not add autonomous resource reads.

## Secrets and storage

MCP credentials live only in `$XDG_DATA_HOME/sos/mcps/auth.json` (default
`~/.local/share/sos/mcps/auth.json`). They are separate from provider credentials.
Explicit trust is stored there too, outside the repository. Setting credentials
does not grant trust; forgetting an identity removes both its trust and secrets.

Secret environment values are separate from the public `environment` map. Public
configuration must not contain tokens. The subprocess inherits only PATH, HOME,
TMPDIR, LANG, LC_ALL and SYSTEMROOT, plus explicit configured environment and
secret overrides. It is **not sandboxed**: it can still read files and use the
network with the daemon user's privileges. Executable search uses `posix_spawnp`;
use an absolute executable path to avoid ambient PATH selection.

JSON files are limited to 1 MiB. Updates use an advisory lock, a private temporary
file, fsync, then rename with mode 0600. Final-path symlinks and nonregular files
are refused. Parent directories are not sandboxed; power-loss durability of the
rename/directory metadata is not guaranteed. Readers see an entire old or new
file, not a partial write. Invalid files are not silently overwritten.

## Client boundaries

Each client serializes its requests. Callers should run it on a worker, not the
Asio UI/transport loop. Transport/protocol failures close the connection. There
is no automatic replay, particularly for potentially mutating calls with an
unknown outcome. Cancellation cannot guarantee rollback of server-side effects.

A tool is classified read-only only when `annotations.readOnlyHint` is the JSON
boolean `true`. This is a server declaration, not a guarantee. The adapter always
requests the `mcp` permission: Sane rejects absent/false declarations even under
Danger or manual approval. Read-only declarations remain subject to Ask/Auto/Danger;
they do not bypass user permission. Always approvals are connection-local and bound
to the runtime tool ID, which fingerprints server identity and tool definition.
Changed definitions receive a different ID, so old manual approvals cannot address
their replacements. The manager revalidates the binding after permission approval.

Text and embedded resource text become tool output; images use the existing image
validation and attachment pipeline. Structured content is rendered when no content
blocks are supplied. Resource links are described without fetching them. Unsupported
blocks (including audio and binary resources) are explicitly omitted. Large text
uses the existing bounded output capture. `isError: true` becomes a tool error,
without replaying the call or disconnecting a healthy server.

Local stdio calls use the file-mutation hook, even for declared read-only tools,
because the declaration is not a sandbox. This allows the existing snapshot owner
to checkpoint success or failure, like Bash. No individual paths can be inferred;
ignored/external files have no per-file fallback. HTTP calls do not use this hook:
their server effects cannot be rolled back, even for an HTTP server on loopback.
Background mutations by a persistent local process are not tracked per call.

Discovery is bounded to 64 pages and 512 tools; stdio messages to 4 MiB. Requests
have a total deadline that notifications cannot extend indefinitely. Process
stderr is currently discarded to avoid deadlocks and accidental secret logging;
a future diagnostic sink would need bounded, redacted output.

HTTP accepts JSON and SSE responses to POST on the same endpoint. This is not the
legacy HTTP+SSE transport. A response stream can deliver a server ping while the
original call is pending; the client can answer on a separate POST. LF, CRLF, CR,
UTF-8 BOM, comments, multiline data and fragmented chunks are handled. The transfer
ends after the matching response instead of waiting for the stream to close.
HTTP bodies are bounded to 4 MiB, headers to 64 KiB, and the incoming queue to
4096 messages. Redirects are refused, including with credentials. Error response
bodies are not included in diagnostics. Loopback requests bypass environment
proxies. Explicit Disconnect sends an authenticated DELETE with session/version
headers, at most once, with a two-second deadline; 404 (already gone) and 405
(unsupported termination) are accepted. Failure still closes the local transport.
Protocol failures/destructors close locally without additional network work.
Per the chosen non-streaming integration scope, there is no persistent GET
subscription, Last-Event-ID resumption or automatic session recovery. SSE decoding
of a POST response remains required by Streamable HTTP and is supported.
Cancellation is polled during transfers; DNS behavior depends on libcurl's resolver.

Manager operations run on workers. Each server has its own serialized operation
lock; disconnect first signals cancellation, then drains that server's operation.
Changing configuration retires old connections. Approval and authentication require
the exact identity shown to the user, not just the server's name. An old tool
binding cannot call a changed configuration or a different cached tool definition.
`prepare` attempts connection once per approved configuration identity, before
generation, on the agent worker. Disabled or untrusted entries never launch.
Failures remain in server status without preventing healthy tools from loading.
An explicit disconnect also consumes that automatic attempt, even before the first
generation. Failure/disconnect suppression survives session changes, timeout edits
and disable/enable, until explicit connection or daemon restart. New destinations
need their own trust. Servers are prepared sequentially with their bounded startup
deadlines. Concurrent agents share connections; cancellation of an in-flight call
closes that shared client, without automatically replaying or reconnecting it.
Daemon shutdown cancels managers and prevents new launches. Its owner must drain
workers before destruction. Received tools-list change notifications trigger a
catalogue refresh after the in-flight call, without replaying it or discarding a
successful tool result if subsequent discovery fails. Generation preparation also
refreshes idle connected servers without reconnecting. It does not queue behind
another agent's tool call. Tool bindings/permissions change with the definitions.
With no background subscription, notifications are consumed during exchanges;
idle changes are observed at the next preparation/browse. Resources/prompts are
relisted on each explicit browse and before preview, never read on notification.

## Verification

`tests/mcp_config_test.cpp` exercises scope replacement, malformed local shadowing,
validation, credential identity isolation, permissions, concurrent updates,
symlink refusal and size limits. `tests/mcp_client_test.cpp` uses an in-memory peer
and `tests/fixtures/mcp_stdio.py` for real stdio initialization, discovery, calls,
errors, cancellation, timeout, malformed JSON and process exit. No external MCP
service or real credentials are used.

`tests/mcp_http_test.cpp` and its loopback Python fixture verify real HTTP requests,
session/version/token headers, streaming initialization, server pings, truncation,
authentication refusal, redirects, cancellation, timeout and absence of retries.
HTTPS certificate validation relies on libcurl defaults and has not been exercised
against an external MCP service. `tests/mcp_manager_test.cpp` checks explicit trust,
stale-identity rejection, credential requirements and independent cancellation of
two connections. Adapter tests in that file cover refusal before dispatch, Sane
restrictions, stale bindings after permission, cancellation, checkpoint-hook
ordering, error propagation, image validation and bounded output. Additional tests
exercise automatic connection policy, concurrent preparation, shutdown, and a real
Agent with a fake provider calling a tool through the daemon Connections binding.
Real-provider and undo/redo end-to-end MCP integration remain unverified.

`tests/mcp_oauth_test.cpp` exercises registration, browser callback, S256 proof,
wrong state, denial, cancellation, issuer mismatch, missing PKCE, concurrent token
rotation, refused refresh, trust revocation and an actual authenticated MCP call
against `tests/fixtures/mcp_oauth.py`. No account or real tokens are involved.
`integrations/tui/tests/test_mcps_e2e.py` runs the real Textual app and `build-ai/bin/sosd`
with isolated XDG storage, loopback OAuth and a stdio subprocess. Only browser
opening is replaced by an HTTP visit. It verifies UI → daemon → callback → token
storage → refresh → MCP tool discovery, plus cancellation and independent
connection/enablement for HTTP and local servers.
It skips if that daemon binary has not been built. This is management E2E, not a
real-model conversation or real Notion account test.

Additional coverage includes modern version negotiation, content catalogues,
resource/template reads, prompt arguments and role isolation, rejected binary
previews, notification-driven tool binding changes, DELETE termination,
WWW-Authenticate discovery, OIDC path insertion, basic/post confidential clients
RFC 7009 revocation and explicit insufficient-scope step-up without replay. The Textual/daemon E2E tests cover preview → existing draft
without sending and token revocation. A real Agent with a deterministic provider
calls the actual loopback OAuth/MCP server and receives the result back through
the runtime. External-provider/Notion-account consent is not part of these tests.

References checked for this implementation:
- https://modelcontextprotocol.io/specification/2025-11-25/basic/authorization
- https://github.com/anomalyco/opencode/blob/dev/packages/opencode/src/mcp/index.ts

OpenCode is an interoperability reference, not a promise to copy its legacy SSE
fallback or its UI/permission policies. No feature is claimed merely by advertising
a newer protocol date.

Connection failures preserve locally generated curl codes/descriptions and HTTP
status codes. Server response bodies, URLs and credentials are not included in
these diagnostics; unknown exceptions retain the generic safe message.
