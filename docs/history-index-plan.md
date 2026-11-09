# Session storage and paginated history

## Chosen architecture

Each session has two main files:

```text
<session-id>/
  session.sqlite
  info.json
```

SQLite is the source of truth, not a disposable index. `info.json` is a readable,
rebuildable metadata summary, including mode, model and reasoning. New sessions
create neither `messages.jsonl` nor `messages.index.jsonl`. SQLite 3.50.4 is
statically compiled into the daemon from the official public amalgamation,
verified by SHA-256. Its code is in the public domain:
https://www.sqlite.org/copyright.html. No `sqlite3` executable is required.

## Delivered behavior

`SessionDatabase` owns the connection, prepared statements and transactions.
`SessionJournal` retains its name and message API but uses SQLite exclusively.
The current schema is 6, versioned through `user_version` and identified by
`application_id`. Tables contain messages by position and unique identifier,
lightweight summaries separate from full bodies, and resume metadata. A
role/position index provides direct access to the last user message. Provider
replay data is preserved.

`SessionRecoveryIndex` retains pending calls, resolved call IDs, refused actions,
approval audits and the latest state of each exact action. It is updated in the
source message's transaction. The registry no longer scans summaries to repair
interrupted execution. Permissions read recorded actions directly, without
decoding tool output. Equivalent refusals share the latest approval state; the
next turn's report retains all chronological toggles. Reads scale with pending
calls or returned permission states, not total messages. Approval-state responses
still return every relevant refusal, without pagination.

An event's messages and metadata are saved in the same transaction, as are a
compaction checkpoint and its context boundary. Failure rolls back the whole
transaction without truncating a file. SQLite's cache is configured to about
4 MiB per connection; this does not yet bound the application's display objects.

`info.json` is published by atomic replacement at creation, turn start/end,
setting changes and explicit saves. Intermediate events do not systematically
rewrite it. Publication errors are reported but do not turn a successful commit
into a failure. Resume always reads SQLite metadata. The session list uses the
cache and rebuilds it from SQLite if absent or invalid; a valid but old cache may
remain stale until that session is next opened.

## Formats and durability

Only a new database (version 0) or current schema 6 is accepted. Older internal
schemas are rejected without migration or message replacement. JSONL files and
old metadata remain untouched and are never imported; `info.json` cannot
initialize a database. A corrupt database is rejected, never replaced from
another format.

Recovery and display indexes are created with a new database and updated in
message transactions. Opening a database neither rebuilds old aggregates nor
loads bodies to fill in a missing skill name. An approval audit without a prior
qualifying AI refusal is rejected, never converted into authorization.

WAL mode and `synchronous=FULL` ask SQLite to synchronize every commit to storage.
The guarantee also depends on the filesystem and hardware. An open database may
have accompanying `session.sqlite-wal` and `session.sqlite-shm` files. Do not
delete them or copy only the database during execution: use SQLite's backup API,
or shut down the daemon cleanly before copying the directory. Only one daemon
per sessions directory is supported.

## Remaining pagination

`SessionHistoryService` queries SQLite's `SessionHistoryIndex`. It no longer
calls `SessionHistoryView::update()` or loads all summaries. Pages use indexed
anchors and position ranges, not OFFSET scans over older rows. A collapsed
Activity reads its precomputed aggregate without loading its items. Rendering
reuses `SessionHistoryView` functions; the old projector also serves as a test
oracle.

The projection is updated in the message transaction, including late results
that complete an older Activity. Durations, model counters and final metadata
require no group reconstruction on opening. Page selections are stable positions,
not snapshots: groups may change between selection and reading during execution.

Individual contents are still read in full. Expanded Activities, including the
preloaded last one, still load all their items. Detail reads retrieve source
message summaries by position, so a message with many parts remains expensive.
Pages are bounded by visible messages, not yet by bytes or auxiliary items.

The target remains opening and pages bounded by both items and bytes:

1. Replace checkpoint scans during context preparation with dedicated queries.
   Approval and interrupted-call scans have already been removed.
2. Paginate Activity items and split large contents into chunks, with explicit
   budgets in the protocol and TUI.
3. Extend budget measurements to large contents and expanded Activities. Tests
   already compare the same last page after 30 and 3,000 older messages and check
   a collapsed Activity with 100,000 items.

`bytesRead()` measures body bytes returned by the reader, not total physical
SQLite I/O. `summaryRowsRead()` counts decoded summaries. Auxiliary recovery tests
check that both remain zero even with thousands of messages and large refused
outputs. Equivalence tests compare pages, aggregates and details against the old
projector after every append and reopen. They also check rollback, canceled
creation and nondestructive rejection of older formats. These tests neither
measure physical reads nor guarantee bounded memory for expanded contents and
Activities, which still need pagination.
