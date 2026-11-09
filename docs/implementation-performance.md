# Sane and Slop execution costs

## Snake: all fixes

Real run `snake-all-fixes-sane-1`, same prompt, `gpt-6.1-sol`, medium,
272,000-token context and Auto/Jev 60. The measurement daemon was rebuilt with
the corrected provider; no TUI automation or human waiting. One completed run,
none discarded. Other columns reproduce the retained earlier runs.

| Section (provider time) | Slop baseline | Sane focused validation | Sane automatic transitions | Sane all fixes |
| --- | ---: | ---: | ---: | ---: |
| Objective Plan | N/A | 24.1 s | 22.7 s | 19.7 s |
| Implementation Plan | N/A | 31.1 s | 31.5 s | 22.3 s |
| Construction reads | 2.7 s | 13.0 s | 15.6 s | 6.5 s |
| Writing/editing | 110.0 s | 139.8 s | 150.6 s | 142.9 s |
| Progress reports | N/A | 63.7 s | 32.0 s | 12.8 s |
| Checks and verification | 2.7 s | 66.7 s | 39.9 s | 11.8 s |
| Step completions | N/A | 22.6 s | 18.8 s | 13.9 s |
| Separate final response | 5.1 s | Included | Included | Included |
| Total provider | 120.4 s | 361.2 s | 310.9 s | 229.8 s |
| Outside provider, all phases | 9.0 s | 26.9 s | 20.9 s | 18.6 s |
| Total wall time | 129.4 s | 388.1 s | 331.9 s | 248.4 s |
| Construction only, wall time | 129.4 s | 328.6 s | 273.6 s | 202.5 s |
| Model calls, all phases | 10 | 42 | 32 | 22 |
| Model calls, construction | 10 | 37 | 27 | 17 |
| Overall cache (cached / input tokens) | 91.49% | 88.73% | 88.23% | 90.05% |

Current result: 285,568 cached / 317,120 input tokens, 9,237 output tokens.
Construction only: 267,904 / 286,234, or 93.60% cached. The first calls of the
three successive agents (Discussion/planning/execution) have zero cache hits;
later calls reuse the cache. Confirmed transport: three successive connections,
22 requests, 19 continuations; the same construction agent keeps its connection
and 16 continuations across 17 requests, including the step transition.

The plan has two steps and four substeps, versus three steps and six substeps
previously. Four completed reports, no separate started/validating reports.
Final validation groups bash/read/grep in one response, without ad hoc DOM/timer
simulation or repetition of unchanged engine tests.

Observed wall-time reduction: 83.4 s (25.1%) against the previous Sane run,
139.7 s (36.0%) against the run before automatic transitions. Construction only:
71.1 s (26.0%) and 126.1 s (38.4%), respectively. The construction ratio against
the retained Slop run falls to 1.56; the total including plans to 1.92. The gains
combine plan size, automation, instructions and Codex format; one generation per
state proves neither individual causality nor reproducibility. Overall cache rate
improves only modestly: do not attribute the time reduction to caching alone.

Outside timing: all 14 generated tests pass, and all three modules have valid
syntax. No game files modified; no real browser or new independent acceptance
suite. Artifacts under
`build-ai/benchmarks/snake-modes/snake-all-fixes-sane-1/`: result, models, events
and execution checkout in `data/sos/worktrees/`.

## Codex format corrected and checked in both modes

The actual 400 diagnostic is `System messages are not allowed` when
`role: system` is sent with `type: message`. The same item without `type`
(EasyInputMessage) is accepted with either string content or an `input_text`
array. A late system instruction in that form is followed. This was tested on
the Codex OAuth endpoint, not inferred solely from the public Responses API schema.

The shared Sane/Slop provider therefore keeps `instructions` constant at
`Follow the conversation instructions.` and removes only `type` from `system`
messages in the Codex network copy. Order, role and content are preserved;
the original history and ordinary OpenAI API format are unchanged.

Direct HTTP check with stable identity and growing synthetic history: six calls,
successive cached-token counts 0, 0, 6400, 6528, 6528, 6528. Calls 3 and 5 add a
late system instruction without changing the prefix. Cache rates are 96.59% and
97.13% on those calls, without a guaranteed hit or latency improvement.
Artifact: `build-ai/benchmarks/snake-modes/codex-history-cache-1.jsonl`.

After the provider fix, a real Integration/Agent check successfully creates a
title/session then runs Slop → Sane → Slop turns, without tools, producing the
three expected answers. Artifact: `codex-fixed-format-modes-1/` in the same folder.
All 44 local provider, title, transformer and WebSocket tests pass; `sosd` builds.
No new Snake benchmark was run for this fix alone.

## Further optimizations: no new timing measurement

The harness also activates the first substep before the first generation.
Resume reuses the existing boundary; revisions retain explicit targets. Planning
instructions limit tests to requested behavior and concrete risks. Validation and
completion favor short checks, reused results and explanations without redundant
recaps, without removing explicit requirements.

`prepareCodexInstructions` is removed. The required Codex `instructions` field
remains constant; `system`/`developer` items remain in `input` at their original
positions. Late announcements no longer alter the previous request's prefix.
Stored histories are not rewritten and compaction boundaries remain explicit.

Local validation: 189 runtime tests and 54 provider/Implement tests pass, and
`sosd` builds. Tests cover HTTP prefix stability and WS continuation after
announcements, initial activation without a model call, resume and the Implement
protocol flow. No time or cache improvement measured at this stage. The real
`cache-preserved-instructions-1` check is blocked by HTTP 400 before session
creation and before any instrumented model call (no `models.jsonl`). It therefore
does not validate acceptance of the format by the real server.

## Snake: automatic transitions

After normal-construction substep completion or dismissal, the harness atomically
persists activation of the first remaining substep, or validation. Initial startup
per step and repairs/revisions remain explicit. Boundaries use the same snapshot
and add neither a diff nor fictitious success.

New real Sane run, same prompt and settings as the previous pair. The first two
columns are retained earlier runs, not new executions. Section times are provider
time, not tool duration.

| Section | Previous Slop | Previous Sane | Sane automatic transitions |
| --- | ---: | ---: | ---: |
| Objective Plan | N/A | 24.1 s | 22.7 s |
| Implementation Plan | N/A | 31.1 s | 31.5 s |
| Construction reads | 2.7 s | 13.0 s | 15.6 s |
| Writing/editing | 110.0 s | 139.8 s | 150.6 s |
| Progress reports | N/A | 63.7 s | 32.0 s |
| Checks and verification | 2.7 s | 66.7 s | 39.9 s |
| Step completions | N/A | 22.6 s | 18.8 s |
| Separate final response | 5.1 s | Included | Included |
| Total provider | 120.4 s | 361.2 s | 310.9 s |
| Outside provider | 9.0 s | 26.9 s | 20.9 s |
| Wall time | 129.4 s | 388.1 s | 331.9 s |
| Model calls | 10 | 42 | 32 |

Observed Sane improvement: 56.2 s (14.5%), ratio against previous Slop 2.56 versus
3.00. Nine reports instead of 17: three initial starts and six results, no
separate started/validating reports for automatic transitions. The plan has six
substeps rather than seven and explicitly reuses test/syntax results after
documentation. Conditional validation instructions are also present: this is not
an isolated transition measurement, nor an established reproducible gain from one
generation.

An earlier retained run (`snake-auto-sane-1`, 370.40 s, two steps) used the local
selection, which had meanwhile changed to `gpt-6-astra`, with automatic context.
It is excluded from the like-for-like comparison. The second run,
`snake-auto-sane-sol-1`, fixes public settings from the previous result using
`SOS_BENCH_REFERENCE` without modifying user configuration.

Untimed verification of the comparable run: 15 generated tests pass and all three
modules have valid syntax. No deliverables changed by the verifier; no real
browser. Implement tests also cover activation/resume, diff ownership, targeted
repair and substep dismissal without automatic step completion. Human validation
remains separate.

## Snake: focused validation instructions

New pair, same prompt/configuration and same rebuilt harness, Slop then Sane,
after adding proportionate validation and reuse of still-valid results.
No completed run excluded.

| Measurement | Slop | Sane |
| --- | ---: | ---: |
| Wall time | 129.41 s | 388.10 s |
| Model calls | 10 | 42 |
| Provider time | 120.43 s | 361.18 s |
| Construction excluding planning | 129.41 s | 328.62 s |
| Input tokens | 69,256 | 819,534 |
| Output tokens | 5,247 | 13,331 |
| Cached tokens | 63,360 | 727,168 |

Sane is 3.00 times slower than Slop and takes 38.47 s more than the preceding WS
pair (+11.0%). Two fewer model calls do not offset the extra generated content:
17 progress reports and three completions remain. Checks take 66.72 s of provider
time versus 44.95 s previously, including validation reads. The generated plan
explicitly requires another test/syntax pass and a final inspection of all six
files/diffs. It therefore retains repetition despite focused execution
instructions; the model does reuse DOM-stub results at the last step. The second
step's wiring check generates a substantial Node script, and the last step
creates another scope-check/HEAD-comparison script. This nondeterministic pair
establishes neither a gain nor causality.

Untimed verification: nine generated Slop tests and 16 Sane tests pass, as do
syntax checks for their three modules. No generated files changed, no real
browser check or new independent suite. Artifacts:
`build-ai/benchmarks/snake-modes/snake-focused-{slop,sane}-1/`.

## Snake: rerun with identity and WebSocket

The same Snake benchmark was rerun after identity and transport fixes, with
current sources rebuilt under `build-ai/`. It does not drive the installed TUI:
same measurement daemon, real runtime classes, no human waiting, same prompt and
OpenAI `gpt-6.1-sol`, `medium`, 272,000-token context, Auto/Jev at 60%. Order:
Slop then Sane. System-instruction handling was not corrected.

| Measurement | Previous Slop | Slop identity + WS | Previous Sane | Sane identity + WS |
| --- | ---: | ---: | ---: | ---: |
| Wall time | 119.51 s | 122.69 s | 357.44 s | 349.63 s |
| Model calls | 9 | 9 | 43 | 44 |
| Cumulative provider time | 110.89 s | 113.51 s | 327.87 s | 323.14 s |
| Input tokens | 58,767 | 59,109 | 838,417 | 871,239 |
| Output tokens | 4,885 | 4,906 | 12,234 | 11,590 |
| Cached tokens | 3,968 | 47,872 | 53,120 | 768,768 |
| Cache rate | 6.75% | 80.99% | 6.34% | 88.24% |

Transport is confirmed: Slop uses one connection, nine requests and eight
continuations. Sane uses three successive connections (Discussion, Implement
planning, construction agent), 44 requests and 39 continuations. No additional
continuation-recovery call is counted. The instruction block still changes at
Implement transitions: five successive states, as in the previous benchmark.

Sane gains **7.80 s, or 2.18%**, on this pair. Construction falls from 308.82 s to
292.80 s (**−5.19%**), while planning takes longer. It remains **2.85 times slower
than Slop**, versus 2.99 previously. About 92% of the gap is still in provider
calls. Cache rate rises sharply, but removes neither generated responses nor
round trips: 17 progress reports and three step completions remain. These few
seconds cannot be causally attributed to transport alone: one pair per
configuration, nondeterministic generations, different plans/content and
uncontrolled server loads.

Untimed verification: ten generated tests pass in Slop, 15 in Sane; both modules'
syntax and the same seven independent checks pass in each case.
`acceptance-ws.mjs` adapts only function names and checks resulting movement for
the different direction models. No generated files were corrected by the
verifier. No real browser check.

Local artifacts: `build-ai/benchmarks/snake-modes/snake-ws-slop-1/` and
`snake-ws-sane-1/`, with `result.json`, `models.jsonl`, `events.jsonl`, prompt and
produced games. The Sane code is in its execution checkout under
`data/sos/worktrees/`, not in the initially empty user repository.

## WebSocket and continuation: Slop check

Codex transport for identified conversations uses a persistent TLS WebSocket
(IXWebSocket 11.4.6). When parameters are identical and the next input exactly
retains the previous input plus its normalized response, it sends
`previous_response_id` with only new items. A change of context, tools,
instructions or model restarts with full input. No connection/continuation data
is persisted in history.

An unavailable handshake falls back to HTTP before generation submission.
Environments configured with an HTTP proxy stay on curl to avoid bypassing it.
A disconnect after submission reaches normal retry handling, without silent
replay of partial output or tools. `previous_response_not_found` is retried once
with full context. Cancellation closes the connection; connections idle for five
minutes or aged 55 minutes are replaced on the next call. A new session or
changed authentication headers uses another connection. The `x-codex-turn-state`
header and system-instruction handling are unchanged.

Same small conversation with six exchanges/seven calls as the check below,
identity active in all three runs. Order: WS 1, HTTP, WS 2. Correct responses,
one read; instructions, tools and prefixes verified stable.

| Run | Cached / input tokens | Rate | Cumulative provider time | Client wall time |
| --- | ---: | ---: | ---: | ---: |
| WebSocket 1 | 34,688 / 50,927 | 68.11% | 17.09 s | 23.93 s |
| HTTP with identity | 42,368 / 50,879 | 83.27% | 17.33 s | 24.99 s |
| WebSocket 2 | 42,368 / 50,922 | 83.20% | 16.86 s | 22.88 s |

Each WebSocket run confirms **one connection, seven requests and six
continuations**, without extra attempts: seven input items transmitted in total,
versus 49 when replaying the full windows for all seven calls. Tools and
instructions remain in each request; this does not remove all context tokens.
Provider usage still counts the full logical context. Telemetry records only
counters, hashes and usage, never credentials or raw request bodies.

**This check shows no cache-rate improvement over HTTP with identity.** Timings
are close and the sample too small to establish a reproducible latency gain.
Continuation works and reduces transmitted input, but does not justify a promise
of systematic cache hits.

Local artifacts under `build-ai/benchmarks/snake-modes/`:
`cache-websocket-slop-{1,2}/`, `cache-websocket-http-1/`. The measurement wrapper
adds `transport.statistics` and enables the control through `SOS_BENCH_HTTP_ONLY=1`.
Final validation: 188 runtime tests pass, 66 provider/session tests pass and
three are skipped as inapplicable. The installed bundle was not rebuilt.

## Codex session identity: Slop check

The Codex provider sends the persisted conversation ID in the `session-id` header
and `prompt_cache_key`. Identity remains stable after resume and provider
replacement; two conversations keep distinct identities. These fields are not
added to OpenAI API requests. System-instruction transformation is unchanged.
This fix adds neither WebSocket nor reuse of the `x-codex-turn-state` routing header.

Real check with the same settings as Snake: six Slop exchanges, reading a
synthetic 120-rule file then asking five short questions about it. Each run uses
real `Integration`, `Session`, `Agent` and provider classes, with seven model
calls. Answers were checked: correct values, no rereading after the first
question, no modified files.

| Run | Cumulative input | Cached tokens | Rate |
| --- | ---: | ---: | ---: |
| With identity, run 1 | 50,894 | 34,560 | 67.91% |
| Control without identity | 50,866 | 0 | 0% |
| With identity, run 2 | 50,894 | 26,880 | 52.82% |

Order: identity 1, control, identity 2. The control disables only identity
transmission in the measurement wrapper. Same fixture/questions, but distinct
directories/sessions and nondeterministic model answers. Hits after reading reach
97–99%; some calls stay at zero. This small check demonstrates actual cache reuse
with identity, not a guaranteed rate, an isolated measurement of each field, or
an established latency gain: the control takes about 25.5 s versus 29.3 s and
24.1 s with identity. Reported cache and latency are not interchangeable.

Local artifacts: `build-ai/benchmarks/snake-modes/cache_conversation.py`,
`cache-session-slop-{1,2}/` and `cache-session-control-1/`, containing prompts,
events, usage and `cache-result.json`. The control wrapper uses
`SOS_BENCH_OMIT_SESSION_ID=1`, only in the measurement daemon.

## Snake: real-model comparison

The same specification requests a dependency-free browser Snake: six files,
20×20 grid, arrows/WASD, food, growth/score, collisions, pause/resume, restart and
deterministic Node tests. Two separate fresh Git repositories were used under
`build-ai/benchmarks/snake-modes/`.

Runs use real SOS `Integration`, `Session`, `Agent`, tools and providers. A
measurement executable adds only a telemetry wrapper to the provider and isolates
storage. Authentication stays in the existing official file; credentials are not
copied into the repository. Identical selected configuration: OpenAI
`gpt-6.1-sol`, `medium` reasoning, 272,000-token context, Auto/Jev at 60%.
Jev remains enabled.

Slop builds directly. Sane follows Objective Plan, branch opening, Auto plan,
then Everything. Sane timing includes both planning stages, checkout preparation
and construction through technical completion. No human waiting, human validation
or import is included. The TUI is not run: this isolates the engine and does not
measure graphical responsiveness.

### Results

| Measurement | Slop | Sane |
| --- | ---: | ---: |
| Wall time | 119.51 s | 357.44 s |
| Model call count | 9 | 43 |
| Cumulative model call time | 110.89 s | 327.87 s |
| Remaining wall time | 8.62 s | 29.57 s |
| Cumulative input tokens | 58,767 | 838,417 |
| Cumulative output tokens | 4,885 | 12,234 |
| Input tokens reported cached | 3,968 (6.75%) | 53,120 (6.34%) |

Sane takes **2.99 times** as long as Slop on this pair. Of the additional
**237.92 s**, **216.97 s**, about **91%**, are spent in model calls, not snapshots
or display. Provider durations include transport and server processing; they do
not separately measure reasoning, prefill and generation.

Observed Sane phases: Objective Plan 22.39 s, opening 0.02 s, Implementation Plan
26.20 s, construction/preparation 308.82 s. Even without planning, Sane construction
takes about 2.58 times the entire Slop turn.

### Sources of additional calls

| Model response type | Slop: calls / duration | Sane: calls / duration |
| --- | ---: | ---: |
| Progress reports | 0 | 17 / 53.12 s |
| Technical step completions | 0 | 3 / 23.90 s |
| Objective Plan + Implementation Plan publication | 0 | 2 / 37.57 s |
| Reads | 1 / 2.37 s | 7 / 27.01 s |
| Writing/editing | 6 / 100.80 s | 7 / 137.75 s |
| Check commands | 1 / 2.42 s | 5 / 43.89 s |
| Final text without tools | 1 / 5.31 s | 2 / 4.61 s |

Counts refer to **model responses**, not individual tool calls. One Sane response
groups three reads, hence nine `read` calls in seven reading responses.

The model proposed three steps and seven substeps: engine, interface, then
documentation/final verification. This decomposition has several effects:

- All 17 progress reports use separate responses. The engine already supports
  grouped ordered calls and the prompt requests them, but this run did not group
  those announcements.
- Required boundaries add model round trips; their cost is not limited to the
  progress tool's local capture.
- Each new step rereads some prior work and reruns checks. Final verification
  becomes a planned substep, then step validation triggers further checks. Extra
  inspections reassure the model even though construction is already preserved.
- Sane also generates more content: more developed tests, explanations, plans and
  extra small verification scripts. Its engine is created then completed in a
  second edit; Slop writes its engine in one pass.
- Repeated calls carry a growing conversation: the last Slop call has 9,220 input
  tokens versus 35,332 for Sane. The 17 progress responses alone account for
  342,790 cumulative input tokens.

The main reproduced problem is **overly fragmented, repetitive orchestration**,
not an editing tool that is four times slower.

### Cache and prefix integrity

Telemetry keeps sizes and hashes, not raw requests or credentials. In the
observed loops, every new input retains all previous input items as an exact
prefix. Tool descriptions remain stable during construction. System messages
change at step boundaries; the Codex adapter groups them into `instructions`.

Reported cache is low in **both modes**, about 6% of cumulative input. This test
cannot specifically attribute the gap to a general rewrite of Sane messages or
a Sane-specific cache failure. Provider routing/cache and the exact effect of
`instructions` changes need separate examination.

### Verification and limitations

Both implementations pass their generated tests: nine Slop and 14 Sane. After
timing, both also pass the same seven engine acceptance checks, independent of
the generated tests, and syntax checks for `snake.mjs` and `main.mjs`. No real
browser was used; real-browser rendering and interactions remain unverified.

Only one completed pair is measured. Outputs differ and provider performance
varies: this is neither a representative median nor proof of a constant ×3/×4
factor. Two client-development runs, interrupted at a `session.created` field
then a branch-transfer response, are excluded; no completed run was discarded.
The build used is `build-ai`, without explicit optimization in the inspected command.

Local artifacts: `slop-2/result.json`, `sane-2/result.json`, per-run `models.jsonl`
(durations, usage/cache, hashes and tools), `events.jsonl`, `verification.json`,
prompt and generated sources. The reproducible client is `run.py`, the instrumented
daemon is `daemon.cpp`, and common checks are in `acceptance.mjs`, under
`build-ai/benchmarks/snake-modes/`.

## Snapshots: local measurement without a model

The independent microbenchmark under `build-ai/benchmarks/snapshot-cost/` calls
real snapshot/editing code, without a model, Jev or TUI. Each recurring cell is
the median of five measurements after an initial fresh capture. Synthetic files
are unique and Git-tracked; system caches are not flushed.

| Repository | Source volume | Unchanged capture | Small Slop edit | Small Implement edit |
| --- | ---: | ---: | ---: | ---: |
| 1,000 × 4 KiB | 3.9 MiB | 0.287 s | 0.407 s | 0.524 s |
| 10,000 × 4 KiB | 39.1 MiB | 1.315 s | 1.276 s | 1.717 s |
| 30,000 × 4 KiB | 117.2 MiB | 3.751 s | 3.303 s | 4.016 s |
| 10,000 × 64 KiB | 625 MiB | 2.531 s | 2.842 s | 3.088 s |

With 30,000 files, start/end milestones take an additional 4.13 and 3.98 s,
respectively. A five-edit substep therefore represents about 28.2 s of local
Implement processing versus 16.5 s for Slop, excluding turn preparation/completion
and model time. This is a measurement-based estimate, not time that can all be
removed without preserving historical guarantees.

Full capture recalculates hashes and rebuilds the private index after every edit
in both modes. Implement adds change attribution and progress captures. Git
deduplicates storage but does not remove repeated reading/computation.

Another lead, **not separately measured**, is `poll(..., 100)` in
`core/process.cpp`: if output closes before process termination is detected,
waiting can add up to 100 ms. This hypothesis must not be presented as the
established explanation of the microbenchmark timings.

Detailed local report: `build-ai/benchmarks/snapshot-cost/RESULTS.md`.

## Proposed priorities

This investigation applies no behavioral optimization. Leads to compare
experimentally: group announcements with tools when no decision depends on an
intermediate result, avoid redundant verification steps/substeps, limit rereading
and repeated summaries, then stabilize context and measure caching. Incremental
captures and removal of redundant captures remain useful for large repositories,
but alone would not resolve the ×3 factor observed on this small Snake task.
