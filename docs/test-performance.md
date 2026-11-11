# Test measurements

Local measurements in the `optimize-tests` worktree, on macOS with Python 3.14.8,
Textual 8.2.8 and dependencies from `uv.lock`. The native build is Release.
Dependency installation, compilation and packaging are excluded from the
runtime/daemon and Textual timings. Python and C++ suites were measured separately,
without CPU contention between them.

## Results

| Suite | Sequential baseline | Optimized execution, 4 workers | Improvement |
| --- | ---: | ---: | ---: |
| Textual | 825.18 s | 127.39 s | ×6.48, or −84.6% |
| C++ runtime/daemon | 349.62 s | 98.95 s | ×3.53, or −71.7% |

The Textual baseline discovered 911 tests, including 11 skipped for lack of a
daemon. The optimized run executes 916 tests with none skipped: five runner tests
were added and the 11 daemon tests can run. The C++ baseline had 687 tests; the
corrected suite has 689, with three skipped in both measurements. These figures
are observations on this machine, not guaranteed thresholds.

The main initial Textual costs were `test_implementation_workspace.py` (227.50 s,
141 tests) and `test_chat.py` (98.81 s, 84 tests), together about 40% of total time.
An eight-case experiment measured 26.40 s with asyncio debugging versus 16.84 s
without, with identical results.

The native test
`ImplementationWorktreeTest.ResumePartialPreparationAllowsSlowCleanupAndBaselineHashing`
takes about 1.2 s rather than 64 s. It inspects the actual process options,
including the ten-minute budget exceeding the generic timeout, and still runs
the real Git resume commands. The two `sleep 31` calls were removed, not the
cleanup and baseline checks.

## Changes

- `integrations/tui/run_tests.py` distributes individual cases among processes
  created with `spawn`, instead of letting one large file monopolize a worker.
  Each batch has its own temporary XDG paths.
- Asyncio debugging is optional. Interactions, animations, assertions and
  `unittest` cleanup are not disabled.
- Pure substep-counter, diff/bullet-color and duration-formatting tests no longer
  open an application or fake server.
- Expiration tests inspect recorded delays and trigger real callbacks rather
  than waiting three real seconds.
- Affected interaction tests explicitly await useful deferred effects: proposal
  focus, the end of a button's active state, and post-mount scrolling. OAuth
  waiting uses a bounded wall-clock timeout rather than an iteration count whose
  duration depended on asyncio debugging.
- E2E sockets use a short temporary path. Their old path was 132 characters long
  in this worktree and failed with `AF_UNIX path too long`.
- Real daemon startup tests use `build-ai/bin/sosd`, without requiring a user
  bundle to be built or installed.

## Verification and caveats

The baseline was not green: 12 Textual failures and three errors, and six C++
failures. The full optimized Textual measurement reported 17 failures and three
errors. Synchronization fixes were made after that measurement; only targeted
tests were rerun afterward, not the entire suite.

The latest 30-case targeted check includes the runner, scroll tests and
interactions that became timing-sensitive. It reports only four scroll failures
already present in the baseline. Targeted repetitions of permission/proposal
interactions and previews also passed. A 33-case targeted check, including the
runner and E2E tests, passes on Python 3.11.9. All 21
`ImplementationWorktreeTest` tests pass.

The long Conclusion test also required automatic scrolling to Close. That
expectation was removed after clarification from the author: the button must
receive default focus even outside the viewport, without requiring scrolling.
`test_final_validation_focuses_close_with_long_conclusion` retains the focus and
return-to-steps checks, with no product change.

The first full optimized C++ validation (89.18 s) reproduced the six initial
failures:
`SessionTest.WorktreeToolMovesLiveConversationWithoutConfirmationAndResumeKeepsSourceConfiguration`,
`SessionTest.WorkspaceToolMovesAcrossPlainFoldersRepositoriesAndWorktreesKeepingSourceConfiguration`,
`SessionTest.SkillInvocationValidatesBeforeGenerationAndPersistsExactLoadedContent`,
`SessionTest.AutomaticCompactionStopsIfTheSummaryStillExceedsTheThreshold`,
`SessionTest.CompactionPreservesFullHistoryAndRestoresOnlyActiveContextAcrossRepeatedCompactions`
and `PermissionModesTest.LoadingSkillNeverRunsItsScriptAndBashStillEnforcesSane`.
At the author's request, these scenarios were then corrected: skill invocations
use `input` instead of the old `arguments` object, fixtures isolate and restore
`XDG_CONFIG_HOME`, inherited skills are found by name, and compaction assertions
account for chunked reductions. Rejection of a still-oversized context remains
covered in a separate scenario without checkpoint activation. A new ordinary
compaction test covers chunking, all three reductions and the final bounded
fallback. The production mechanism was neither removed nor relaxed.

The corresponding targeted validation passes all 101 tests. Full native
validation after correction passes in 98.95 s: 689 registered tests, 686 passed,
three skipped and no failures. The Textual caveats above remain unchanged.

Detailed local reports are under `build-ai/test-measurements/`:
`baseline.json`, `final-tui.json`, `last-targeted.json`, `python311.json`,
`native.xml`, `final-native.xml`, `skills-compaction-targeted.xml` and
`final-native-fixed.xml`, with associated logs. `baseline-conclusion-fast.log`
preserves reproduction against the `HEAD` test. These artifacts are ignored by
Git and are not shipped with the code.

See reproducible commands in
[CONTRIBUTING.md](../CONTRIBUTING.md#pythontextual-tests).
