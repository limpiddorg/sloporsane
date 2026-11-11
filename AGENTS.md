# Working agreement

## Before making changes

- Read [CONTRIBUTING.md](CONTRIBUTING.md) for coding and build conventions,
  and the [README](README.md) for the product and architecture.
- Read the relevant sections of the [product contracts](docs/product-contracts.md)
  before changing the TUI, Implement, workspaces, skills or MCPs.
  These contracts are requirements, not suggestions.
- Use only `build-ai/` for verification builds, tests and installations.
  `build/` is reserved for the human.
- TUI requests target Python/Textual (`integrations/tui/`).

## Collaboration

The author leads; the agent completes the requested unit of work, explains the result
and returns control. The product's Slop mode does not change this agreement.

- A discussion, question or request for an opinion does not authorize implementation.
- Stay within the requested scope: do not add refactoring, abstractions, features
  or follow-up tasks on your own initiative. Preserve unrelated changes.
- Resolve obvious local details yourself. Ask before making a structural choice or
  acting on a discovery that changes the expected behavior, approach or scope.
- Inspect the relevant code before changing it. Keep orchestration short,
  name steps by their intent and share genuinely duplicated details.
- Every commit must remain shippable: all tests must pass. Run targeted tests
  while fixing issues, then all test suites before committing. After a failure,
  fix and recheck with targeted tests, then rerun all suites on the final state
  before committing. An unrun test or pending CI does not count as a success;
  if verification is unavailable, report the blocker and do not commit.
  Also confirm that CI is green after every push.
- Communicate in French, directly and briefly. Code, comments and application text
  remain in English. Explain actual changes and checks, distinguish uncertainties,
  then wait for the next instruction.
- Do not delegate to subagents, commit or push without an explicit request.
- Commit messages: `type(scope): description`, with lowercase type and scope
  and an English description. Possible types: `fix`, `feature`, `tests`, `docs`,
  `build`, `ci`, `refactor`, `chore`, according to the intent of the change.

## Product intent

**Sane makes building software understandable and participatory, in human-sized
steps.** The user's understanding and involvement are part of the result.

- Show semantic progress, not a list of tools, files or checked boxes.
- A step delivers a coherent result that can be exercised at its own level. A testable
  foundation may precede its integration; not every step has to deliver a final feature.
- Substeps expose understandable intentions and changes; their intermediate states
  may be incomplete. Tie explanations to the code actually produced.
- Discussion, branches, diffs, snapshots and validations support this experience
  without replacing it. Slop remains a legitimate choice for greater delegation.
- Do not take future adoption for granted: it still needs to be validated through use.

## Technical invariants

- Separate construction, read-only review and human validation. Only the harness
  authorizes an execution phase; neither the model nor loaded content can activate it.
- Do not transfer permissions through history, reviews or skills.
  An MCP `readOnlyHint` annotation is a declaration, not a security guarantee.
- Preserve histories and their snapshots. Historical views must not silently read
  the current checkout or ever replay tool side effects.
- Keep Activity summaries lightweight: preserve `bash.mainCommand` and `grep`/`glob`
  patterns without loading old messages in full to fill them in.
- Centralize global C++ paths in `PathResolution`
  (`src/agent-runtime/common/path_resolution.hpp/.cpp`).

## Storage and secrets

All global paths use a `sos/` subdirectory under the XDG base directories:

| Base | Usage | Default |
| --- | --- | --- |
| `XDG_STATE_HOME` | State and sessions | `~/.local/state/` |
| `XDG_DATA_HOME` | Persistent data | `~/.local/share/` |
| `XDG_CONFIG_HOME` | Configuration | `~/.config/` |
| `XDG_CACHE_HOME` | Rebuildable caches | `~/.cache/` |

- Install the command in `~/.local/bin/`: there is no standard `XDG_BIN_HOME`
  and no global `~/.sos/` directory.
- Provider credentials: `$XDG_DATA_HOME/sos/auth.json`, permissions `0600`.
  Use this file exclusively, never API keys from the environment.
- MCP secrets: `$XDG_DATA_HOME/sos/mcps/auth.json`, permissions `0600`, separate
  from providers and shareable configuration. Identity includes scope and destination.
- Never put secrets in the repository, conversations or sessions.
  `/connect` and MCP Connect use masked fields or the dedicated OAuth flow.

## References and maintenance

- [Product contracts](docs/product-contracts.md): detailed interactions and invariants.
- [Daemon protocol](docs/daemon-protocol.md), [snapshots](docs/git-snapshots.md),
  [skills](docs/skills.md), [MCP](docs/mcp.md): formats and behavior.
- Maintain this agreement based on explicit decisions and adopted conventions.
  Do not accumulate a session log, copy the README or turn an agent suggestion
  into a decision. Report updates to this agreement in the summary.
