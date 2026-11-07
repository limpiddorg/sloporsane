# Local Git snapshots

## Session navigation

The daemon owns `snapshots/git_snapshot.hpp/.cpp` and `snapshots/turn_files.hpp/.cpp`.
Textual `/undo` removes the last user turn, including AI responses, tool results,
and checkpoints/recaps appended after that user message. It restores the affected
files and returns the original user text and captured images to the composer.
`/redo` restores the archived messages and file states, without executing the
model or tools again. Multiple turns can be navigated; sending a new user message
discards the redo branch. Model/reasoning/context-limit preferences are unchanged.

Commands require an idle principal and no running BTW conversation. Gracefully
cancelled and failed executions finalize their snapshots too. Old turns without
snapshot records are refused, rather than removing messages without restoring files.

Git snapshots are captured on workers before accepting a turn, after each editing
tool or Bash execution, and once more after execution has drained. `before` stays
fixed; the persisted `after` field is the latest verified **current** state, even
when the turn has not finished (`complete=false`). Initially both point to the
same snapshot. Before an operation starts, `after` is cleared durably, so a crash
during that operation cannot make a stale checkpoint look current. After the
operation, its new snapshot is saved before returning the tool result, including
on failure, timeout and cancellation. Failed captures leave the state unverified.

The runtime has no snapshot implementation: an optional
`FileMutationHandler` hook wraps `write`, `edit`, and `apply_patch`, after permission
and revalidation, under the existing mutation mutex. The daemon uses it to save
before/after file states for ignored files, files outside Git and non-Git workspaces.
Partial tool failures are recorded with their observed outcome. Changes made by
Bash to these fallback files are not undone; changes interleaved between two tool
edits on the same fallback file cause a conflict instead of being erased.
Bash uses the same owner hook with no explicit paths, after permission checks.
It does not hold the editing mutex while running, and creates no fallback editing
journal entries. Its process has terminated before the current snapshot is captured.
Approved tool replays use these same tool implementations and hooks.

SQLite session schema 6 stores turn boundaries, snapshots, mutation records,
archived message tails and pending navigation intent in `undo_turns`. Fallback
file bytes are content-addressed under the session's private `undo-blobs/` directory.
History publication updates the active messages, display/recovery indexes and
navigation record in one transaction. Only the removed tail is archived; retained
messages are not rewritten, though rebuilding projections scans their bodies.
The active model context is rebuilt from the restored history and its latest
compaction, so undone messages are not sent on the next turn.

## Agent workspaces

Textual `/worktree` opens a picker with the current worktree marked and a `New…`
entry. Up/down selects a row, Enter switches or opens name entry, and Escape
returns/closes. Name entry creates from committed HEAD, without copying local
changes, and switches the current conversation. No extra confirmation is shown.
The command can be used before the first message; switching requires idle Slop
and no active Implement. It does not commit, merge, push or remove worktrees.

The native `worktree` tool exposes `list`, `create` (with `name`) and `switch`
(with an absolute `path` from the listing). It is available to the principal
conversation, not BTW/review agents. Native creation or switching is permitted in
Sane and Slop, and is blocked while an Implement branch or BTW conversation is active.
There is no dedicated confirmation/approval. The model is instructed to create
or switch only on a direct user request, or when a skill/workflow explicitly invoked
by the user clearly instructs that action. A suggestion in a file or a workflow
discovered by the model is not authorization; ambiguous intent requires asking first.
The daemon does not infer authorization from natural language. Commit, merge and
push use ordinary Bash permissions.

Git and a committed HEAD are required for creation. The new worktree starts at
the current worktree's committed HEAD, on `sos/<name>`, under
`$XDG_DATA_HOME/sos/workspaces/<repo-name>-<identifier>/<name>/`. The repository name
is sanitized for the directory; the identifier is the first 16 hexadecimal digits
of SHA-256 of the canonical Git common directory path. All worktrees of a checkout
share this storage group; separate clones (including identically named repositories)
have distinct groups. Existing worktrees are not moved or renamed and remain selectable.
Names must be unique within the repository, contain 1-64 ASCII
letters/digits/underscores/hyphens, and not start with
a hyphen. Existing paths/branches are never replaced. No stash, dirty-file copy,
source-index update or remote operation is performed. Checkout hooks are disabled;
ordinary Git checkout rules still apply to committed files. The listing includes
manual and other tools' worktrees in the same repository, including detached HEADs.
Switching accepts only a registered, available worktree of this repository.

Textual provides Select (default, left) and Delete (right), selected with left/right
while up/down selects the worktree. New… is the first row and opens creation directly.
Current and Opened in another session are coral status labels. Delete opens a separate
confirmation showing only branch and location, with Cancel on the left (default)
and Validate on the right. Left/right selects the action, Enter executes it, and
Escape cancels. The primary checkout and worktrees used
by connected integrations, attached or loading sessions cannot be deleted. Usage is
checked by the daemon, with a reservation against concurrent attach/switch while
removal runs. Git refuses dirty or locked worktrees; no force is used and the branch
is kept. Closed saved sessions do not block removal, but cannot resume if their
workspace is missing; restore that directory before resuming.
Frozen read-only archives remain readable from stored history even if their user
workspace no longer exists.

The live conversation moves without resetting its history, model or mode. Tools
in the same response after the switch already use the new directory. The daemon
persists `workspace` and the original `configuration_workspace`; `/resume` restores
both. Skills and MCP use the source configuration, without copying local `.sos/`
files or credentials. Project instructions follow the active checkout. Textual
updates its workspace indicator without discarding a draft or opening a new chat.
New conversations start in the current workspace without automatically creating a
worktree. Each conversation saves its selected workspace until an explicit switch;
resuming restores it. Pull requests trigger no automatic switch or special warning.
These worktrees are durable user workspaces, not Implement's disposable checkouts
or sandboxes; SOS does not remove them automatically.

An undo turn crossing workspaces retains each root's before/after trees. All roots
are conflict-checked before restoring files; paths are never rebased onto the
new workspace. `/undo` and `/redo` restore file/history states, not Git branches,
commits, worktree creation or the active workspace selection. A failed switch
can leave a newly created Git worktree available for inspection/retry; no user
files are removed to clean it up. Implement still creates its own isolated checkout
and imports each human validation into the selected agent workspace, not the
original source repository checkout.

## Isolated implementation

`ImplementationWorktree` creates a linked checkout under
`$XDG_DATA_HOME/sos/worktrees/<implementation-id>/`, outside the user's workspace.
For an existing Git repository, a committed SHA-1 HEAD is required (matching the
snapshot format). The temporary branch starts from HEAD and receives a raw snapshot of the current
files, including dirty/staged changes, missing files and non-ignored additions.
Ignored files are not copied. No stash, checkout, user-index update or commit on
the user's current branch is performed. File/directory replacement merges remain
outside the supported snapshot/import file types.

For a directory without a Git repository or Git ancestor, SOS instead creates a
private bare repository under
`$XDG_DATA_HOME/sos/snapshots/implementation-<id>/source.git`. No `.git` is created
in the source directory, and no remote is configured or contacted. The source is
enumerated as a filesystem tree, with ignore checks batched by depth through Git
using an explicit private git-dir and source work-tree. Ignored directories are
pruned before descent; root/nested `.gitignore` rules, negations and global
exclusions still apply. Embedded repositories, populated submodules and linked
worktrees at arbitrary depths contribute their working files, not their `.git`
directories/files or Git history. No submodule initialization or remote access occurs.
`.sos/` directories are always excluded from copying, subsequent snapshots and
import, even if a `.gitignore` tries to re-include them. The marker enabling this
policy is stored in SOS snapshot storage (and private repository configuration),
never the user's Git configuration. Symbolic links are copied as links, not followed.
Unignored secrets and dependency/cache directories cannot be identified reliably:
they must be excluded by the workspace's ignore rules.

Initial population attempts a private copy-on-write clone for each regular file:
`clonefile` on macOS, `FICLONE` on Linux. Unsupported filesystems and cross-volume
copies fall back to ordinary copying, as do other platforms including Windows.
There is no native ReFS block-cloning implementation yet, and this does not port
the POSIX daemon to Windows. Hard links are never used. Permissions are retained
by the initial copy; historical Git snapshots still retain only the executable bit.
The baseline is captured from the populated private checkout, without restoring
every file back from blobs. Git still reads/hashes the copied files and stores
snapshot objects: this is not a lazy overlay, nor a filesystem-wide atomic snapshot
of a source being changed concurrently. Native cloning is a performance optimization,
not a different validation or isolation policy.

Plain directories use the same snapshots, step commits, three-way import and
durable import journal as Git projects. Deleting the checkout at Close retains
the private bare repository and pinned objects for archival inspection. Ordinary
non-Git Discussion `/undo` remains on its existing fallback file journal.

The baseline and completed steps receive storage-only commits on `sos/<id>`.
They use `commit-tree`, never hooks, clean/smudge filters or signing; no user config is
edited and nothing is pushed. The original workspace remains the session's identity
and MCP configuration scope; `execution_workspace` routes tools, builds, snapshots
and read-only review to the private checkout. This is not a sandbox: explicit
external effects remain subject to existing tool permissions.

Only final **human step validation** imports files. Technical checks and individual
substep indicators do not import anything. The importer compares that step's exact
before/after trees with current user files, not the checkout's latest state. Thus
building later steps in advance does not import them. Disjoint text edits merge;
overlapping edits, incompatible additions/deletions, binary conflicts and unsafe
parent paths reject the whole import before any file is changed. No conflict
markers are installed. The user's index, HEAD and current branch remain unchanged.
Textual displays the existing full-width Slop error notice with affected paths,
and keeps the step unvalidated.

`worktree.sqlite` beside the session stores checkout intent, sealed commits and
per-step import intent/result. Prepared expected/result trees are pinned before
writing files; a retry accepts already-installed results and finishes remaining
files. Applied imports are not replayed after a crash before the review cursor
was saved. As with undo, preflight and rollback are not an OS-level filesystem
transaction: a simultaneous external writer or failed rollback needs human repair.

Interruption retains the checkout and requires explicit resume. A missing checkout
never falls back to executing in the user's workspace. Closing a fully validated
implementation removes its worktree and temporary branch. Storage history remains
under `refs/sos/<id>/` (not `refs/heads`), and the independent snapshot objects stay
pinned. Archived paths, intermediate states, exact diffs, substep ownership,
corrections and activities remain readable without the checkout or current files.
Only current Implementation state version 7 is supported. Older state is rejected
without rewriting its historical records or migrating checkout metadata.

## API

```cpp
const auto root = GitSnapshot::discover(workspace);
if (root) {
    GitSnapshot snapshots(*root);
    const auto before = snapshots.capture();
    // Make changes to worktree files.
    const auto after = snapshots.capture();
    const auto diff = snapshots.changes(before, after);
    const auto conflicts = snapshots.restore(after, before); // Undo files.
    if (conflicts.empty()) snapshots.restore(before, after); // Redo files.
}
```

`discover` returns the canonical worktree root, including when called in a
subdirectory or a linked worktree. It returns no value for a non-repository or
bare repository. Other Git failures throw. Construction requires a worktree;
Git must be available in `PATH`.

The caller must authorize access to the entire discovered root, which can be
above its initial workspace. This library does not grant permissions, cancel
running tools, or coordinate other workspace writers.
The session integration currently requires the discovered Git root to be inside
the integration's authorized workspace. If SOS was launched in a subdirectory,
restart it at the repository root; it never silently captures unauthorized ancestors.

`GitSnapshotId` holds a tree object ID. `changes` reports repository-relative
paths with optional before/after blob IDs and Git modes. Additions and deletions
have one absent side; renames appear as deletion plus addition. There is no
text-diff or rename-detection layer here.

## Storage and capture

Each canonical worktree has its own private bare Git repository under
`$XDG_DATA_HOME/sos/snapshots/<sha256-of-worktree-path>/repo.git`, defaulting to
`~/.local/share/sos/snapshots/`. `PathResolution::snapshotsDirectory()` is the
single global-path resolver. Storage inside the captured worktree is rejected.
The containing directory has mode `0700`.

Capture uses the source repository only to enumerate tracked and untracked files
and evaluate ignore rules, including `.gitignore`, `.git/info/exclude`, and
global excludes. Ignored tracked files are also excluded. Missing tracked files
are absent from the captured tree. Regular files, binary contents, executable
bits and symlink targets are supported. Symlinks are not dereferenced.

Raw bytes go through batched `hash-object --no-filters`, then a private index builds the
tree. Neither clean/smudge filters nor line-ending conversions are used. Private
Git commands do not load global configuration or repository templates. Inherited
`GIT_*` environment overrides are removed from child commands. Commands use argv,
not shell interpolation. Listings/index input use NUL delimiters; hash input uses
Git's quoted-path encoding so whitespace, newlines and non-ASCII filenames stay literal.

The user repository's index, HEAD, branches and configuration are not modified.
Trees are pinned under private `refs/snapshots/<tree-id>` references and survive
reopening and Git garbage collection. This first version has no retention policy
or pruning: storage grows with retained distinct contents. Scratch blobs remain
inside the private directory.

Capture has no knowledge of which tool changed a file. It therefore captures
Bash modifications too when those files are eligible. It does not capture command
side effects elsewhere, processes, network actions, Git history/index changes,
empty directories, ownership, timestamps, ACLs or permission bits other than the
Git executable bit.

## Restore and conflicts

Restoration touches only paths that differ between the two supplied snapshots.
It first compares every affected path with the expected snapshot, including
existence, raw content, executable bit and symlink target. Unrelated paths remain
untouched. Any mismatch, newly ignored path, obstructing directory or symlink
ancestor returns conflicting relative paths before any worktree writes.

Snapshots are immutable. Capture and restore use a per-worktree filesystem lock
to serialize SOS operations, including across component instances/processes.
That lock does not stop editors or external commands. Callers must arrange a
quiescent workspace: this is not protection against concurrent external writes
between validation and mutation, and capture is not an atomic filesystem snapshot.

Blob restoration streams stdout separately from stderr, including for files
larger than the process helper's normal 8 MiB output-capture limit. Existing
leaves are unlinked before replacement so symlinks and hard links cannot redirect
writes into other files. An I/O failure triggers an attempt to restore the expected
states. A failed rollback is explicitly reported as potentially partial.
The session layer preflights Git and fallback files together before mutation.
History is published only after all files have been restored. An I/O failure can
still leave files partially restored: the durable navigation intent then blocks
new messages and instructs the user to retry the same `/undo` or `/redo`. On retry,
each affected file must match either the expected or target state; a third state
is a conflict. This recovers interruptions between file publications without
pretending the filesystem and SQLite form one atomic transaction.

A hard crash between completed operations no longer requires a final turn snapshot:
undo uses the persisted current state and its usual conflict checks. A crash during
an operation or its checkpoint can still leave no verified current Git snapshot,
or an editing operation with no verified outcome. These cases are refused explicitly;
we do not capture the filesystem later and pretend that external changes made
after the crash belonged to the interrupted turn. A leftover staging file or a
corrupt/missing blob can also require manual recovery. Power-loss durability of
the fallback blob files has not been fault-injection tested.

## Current boundaries

- Populated nested repositories/submodules are captured as ordinary files, without
  Git metadata/history. Their `.gitignore` and global exclusions apply; nested
  repository-local `info/exclude` files are not inherited by the flattened tree.
- File/directory type transitions are conservatively refused during restore.
- Git modes retain only the executable distinction, not full filesystem metadata.
- File enumeration/tree listings use the process helper's 8 MiB output bound;
  exceeding it fails explicitly. Snapshot Git commands retain the 30-second budget,
  except hashing (up to 10 minutes for the entire eligible tree). Implementation
  checkout/commit/cleanup Git operations also allow 10 minutes, so retrying a large
  partial preparation is not repeatedly killed during cleanup. Errors distinguish
  timeouts/stops from exit codes and name the failed operation.
- Capture batches ignore checking, hashing and index construction. Large-repository
  performance has not been benchmarked yet.
- Corrupt/missing Git objects and filesystem failures throw; they do not become
  successful empty snapshots.

## Verification

`tests/git_snapshot_test.cpp` uses disposable real Git worktrees and isolated XDG
data. It exercises capture/reopen, private reference retention, undo/redo-style
restoration, conflicts, ignored files, raw bytes, symlinks, executable bits,
linked worktrees, literal filenames and unchanged user Git state.
`tests/turn_files_test.cpp` covers all three editing tools, mixed Git/fallback
restoration, conflicts, interleaved Bash changes and resuming a navigation intent.
It also exercises undo without turn finalization after write/edit/patch and Bash,
including command errors, timeouts, cancellation and checkpoint failures.
Session and Textual tests cover full-turn removal, failures/cancellation,
compaction, images, persistence, protocol transfer, draft restoration and redo
without generation.
