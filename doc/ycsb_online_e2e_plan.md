# Add deterministic YCSB support and measure online-checker overhead

Status: in progress; Milestones 0-6 and Milestone 7 smoke/calibration are
complete; the requested 100,000-row diagnostic is complete and the five-trial
four-mode publication matrix remains paused at the user's request

Scope: `/home/dyf/code/db-consistency/co-checker`,
`/home/dyf/code/db-consistency/tidb-online`, and a pinned go-ycsb v1.0.3 source
checkout

Supported database semantics: optimistic repeatable-read/snapshot isolation;
the checker infers a compatible snapshot and does not consume TiDB's numerical
transaction start timestamp

This ExecPlan is a living document. Keep `Progress`, `Surprises & Discoveries`,
`Decision Log`, and `Outcomes & Retrospective` current while implementing it.
It follows `/home/dyf/code/db-consistency/tidb-online/PLANS.md`. A contributor
must be able to resume from this file alone; update the plan whenever code,
commands, assumptions, or observed results differ from what is written here.

## Purpose / Big Picture

After this work, a user can run deterministic YCSB Core workloads A through F
against the instrumented TiDB and obtain a fail-closed online-checker verdict
plus a reproducible performance-overhead report. The supported topology is one
TiDB process, one checker producer, and TiDB's in-process UniStore. The first
version records the exact physical key/value reads actually returned or consumed
by supported point and scan operators. Inferring predicates, ranges, or phantoms
is intentionally outside the checker problem.

The finished workflow will build immutable checker, TiDB, SQL-helper, and
go-ycsb artifacts; create schema outside the checked epoch; capture every data
load write; exercise a fixed YCSB workload; drain the online pipeline; close
TiDB gracefully; validate the authoritative verdict; and compare at least five
paired trials of pristine TiDB, capture-only, order-only, and full checking.
The observable successful result is a compact result directory containing
experiment/build identity, retention record, and the atomic summaries
`ycsb-trials.csv`, `ycsb-performance.csv`, `ycsb-summary.json`, and
`ycsb-report.md`. Stores and raw attempt evidence exist only until analyzer
validation succeeds. Failed detail is retained temporarily for diagnosis.

## Progress

- [x] (2026-08-09) Audited the native online checker, current TiDB
  instrumentation, TPC-C runner, pinned go-ycsb v1.0.3 client, and YCSB A-F SQL
  shapes.
- [x] (2026-08-09) Fixed the semantic scope for this plan: optimistic SI only,
  returned physical KV evidence for scans, and no predicate/phantom inference.
- [x] (2026-08-09) Wrote this implementation and measurement plan.
- [x] (2026-08-09 12:39Z) Established the pre-change source/test baseline,
  corrected the failpoint-aware retry command without weakening its lifecycle
  assertions, rebuilt the stale native Release tree, and passed all Milestone 0
  tests.
- [x] (2026-08-09 12:55Z) Made capture-active direct and prepared optimistic
  autocommit point reads use a normal TSO while preserving both MaxTS
  optimizations when capture is disabled.
- [x] (2026-08-09 12:55Z) Rejected pessimistic and non-repeatable-read checked
  transactions at activation, kept automatic retries optimistic, and passed the
  focused plus complete `TestOnlineCapture*` regression sets.
- [x] (2026-08-09 13:15Z) Added and tested the pinned deterministic, strict
  go-ycsb patch, reproduced its `-trimpath` binary hash from two fresh exports,
  and preserved its build/test/result evidence in an immutable artifact root.
- [x] (2026-08-09 13:31Z) Added the fixed/hash-pinned A-F profiles and the YCSB
  schema, emptiness proof, bounded canonical digest, exact table/session/plan
  gate, and actual text/prepared SQL probes. Unit/vet and live UniStore
  validation passed.
- [x] (2026-08-09 13:57Z) Added exact binding-shape, evidence, fail-closed,
  lifecycle-conservation, and native full-mode YCSB TiDB tests; passed the
  focused Go/native tests, the YCSB-filtered Bazel target, and all applicable
  race/regression selections.
- [x] (2026-08-09 14:30Z) Implemented and fixture-tested the immutable,
  append-only four-mode runner and fail-closed analyzer; a live fresh-store
  Workload A diagnostic passed all modes and produced the required atomic
  summaries.
- [x] (2026-08-09 14:41Z) Passed the A-F four-mode smoke suite from fresh
  stores: 24/24 matrix cells were valid, every full verdict was `pass`, and no
  attempt required a retry.
- [x] (2026-08-09 14:41Z) Used the six full-mode smoke attempts as proportional
  capacity pilots, derived fixed 60-second warm-up and 600-second measurement
  operation counts from pristine rates, and froze the one-million-row,
  16-client calibration with host/capacity evidence.
- [x] (2026-08-09 15:51Z) Preserved the first measured root after it failed
  closed in A trial 1 capture-only: one Zipfian hot-key update exhausted TiDB's
  default ten internal optimistic retries after 1,012,627 successful operations.
  Source and log evidence justified freezing a uniform retry limit of 100 and
  restarting under a new experiment identity; the failed attempt is not a
  measured trial.
- [x] (2026-08-09 16:15Z) Passed a stricter 16-thread retry qualification with
  100,000 keys, 200,000 warm-up operations, and 2,000,000 measured operations:
  all four modes were valid, capture-only survived 43,082 measured physical
  aborts, and full returned `pass` after 43,278 measured physical aborts.
- [x] (2026-08-09 16:15Z) Re-ran the storage gate with observed measured-store
  growth. Preserving all required evidence makes 120 one-million-row fresh
  stores unsafe on the remaining filesystem, so the publishable calibration
  uses 500,000 rows, retains the fixed operation counts and 16 clients, targets
  at least 180 seconds of pristine work, and is explicitly not labeled the
  one-million-row profile.
- [x] (2026-08-09 16:51Z) Preserved the first 500,000-row matrix root after its
  first untimed load completed all 500,000 inserts but the canonical digest's
  inherited 30-second context expired on the final keyset page. Added a
  configurable, manifest-recorded 10-minute whole-digest deadline while
  retaining the 1,000-row page bound; syntax, 8/8 fixture tests, 5/5 CTests,
  helper unit/vet, and exact measured validation pass.
- [x] (2026-08-09 16:53Z) Re-ran the append-only storage gate after retaining
  that 19.63 GB failed root. A new 500,000-row matrix would leave only 36.28 GB
  under the frozen model; reduced only initial rows to 400,000, preserved all
  fixed operation counts, and froze the validated calibration with 56.44 GB
  projected reserve and unchanged ≥180-second empirical acceptance gate.
- [x] (2026-08-09 17:36Z) Paused the corrected publishable root
  `/home/dyf/code/db-consistency/ycsb-online-results/milestone7_measured_400k_20260809T165500Z`
  on user request after its immutable build, mandatory 24-cell smoke, all six
  independent 400,000-row references, and A trial 1 baseline passed. Baseline
  completed 7,231,770 operations with zero failures in 446.708 seconds. The
  interrupted capture-only attempt was invalid by construction; its superseded
  raw root was pruned under the later final-results-only retention decision.
- [x] (2026-08-09 20:02Z) Ran one calibrated A-F diagnostic trial containing
  only `baseline-original` and `serialized-full` so rough per-workload overhead
  is available before resuming the publication matrix. All 12 measured cells
  and all 12 automatic smoke cells are valid; every full verdict is `pass`.
- [x] (2026-08-09 18:00Z) Replaced raw-success retention at user request.
  Pruned 113 GB of superseded/failed stores and raw logs, leaving 332 KB of
  accepted smoke/pilot summaries. Added immediate post-validation store pruning,
  final-summary-only suite cleanup, error-level TiDB logs, and an opt-in
  `--retain-details` debugging mode. A live two-cell retention smoke passed and
  left a 64 KB final root with zero invalid rows.
- [x] (2026-08-09 19:12Z) Simplified future pairing at user request. Removed
  the experimental physical-store clone path, fixed one permanent load,
  warm-up, and measured seed for every workload/mode/trial, and restored an
  empty store plus the same seeded YCSB load SQL in every mode. Added and
  validated the frozen one-million-row fixed-seed calibration; shell syntax,
  9/9 fixtures, and 5/5 CTests pass. A live fresh-store baseline/full smoke
  passed 2/2 cells and retained only a 64 KB compact result.
- [x] (2026-08-09 19:32Z) Stopped the one-million-row diagnostic at user
  request during A baseline measurement after 1,063,594/7,231,770 operations;
  the interrupted cell is invalid by construction and provides no performance
  result. Replaced it with a fixed-seed 100,000-row basic calibration using
  one-tenth of the calibrated warm-up/measured operation counts, targeting
  roughly 60 seconds of pristine measurement per workload.
- [x] (2026-08-09 20:21Z) Profiled the high baseline/full overhead with
  separate C/E CPU, `perf`, and delayed goroutine diagnostics. Source,
  telemetry, and profiles agree that point workloads lose the prepared
  point-get/MaxTS shortcuts, pay 69-77% more TiDB CPU per logical operation,
  and convoy at the process-wide finalization guard. E amortizes the same
  transaction path across 47.37 events/publication and remains CPU-saturated.
  Preserved 5.2 MiB of compact profile evidence and recorded the diagnosis
  before pruning temporary stores/logs.
- [ ] Complete at least five valid paired measured trials per selected workload
  and publish the raw and summarized overhead results.

## Surprises & Discoveries

- Observation: the native checker is workload-independent. It consumes opaque
  physical keys and values, so YCSB needs no new C++ event type or table decoder.
  Evidence: `online/include/co_checker/online/types.h` and
  `online/doc/c_api_v1.md`.
- Observation: stock go-ycsb point operations are separate autocommit SQL
  transactions. Workload F's logical read-modify-write is one `Read` call
  followed by one `Update` call and therefore becomes two TiDB transactions.
  Evidence: go-ycsb `pkg/workload/core.go`, function
  `doTransactionReadModifyWrite`.
- Observation: TiDB's optimistic autocommit PK/unique point-get optimization
  uses `math.MaxUint64` instead of activating a normal TSO-backed transaction.
  Current checked mode deliberately rejects it. TPC-C does not hit the branch
  because every measured go-tpc transaction calls `BeginTx`.
  Evidence: TiDB `pkg/sessiontxn/isolation/optimistic.go`,
  `pkg/planner/core/common_plans.go`, and go-tpc v1.0.12
  `tpcc/workload.go`.
- Observation: capture hooks can record some pessimistic paths, but that does
  not make pessimistic RR a supported checker semantic. Pessimistic current or
  locking reads may use `for_update_ts` newer than the transaction snapshot.
  Positive checked workloads must be optimistic RR, and unsupported modes must
  fail before publication.
- Observation: stock go-ycsb always calls `ANALYZE TABLE` after loading and
  ignores its error. Checked TiDB correctly rejects `ANALYZE`, so an unpatched
  client can silently poison a run.
  Evidence: go-ycsb `pkg/client/client.go` and `db/mysql/db.go`; TiDB
  `pkg/executor/compiler.go`.
- Observation: stock go-ycsb logs or suppresses operation errors, counts the
  failed operation toward progress, and normally exits successfully. Its RMW
  histogram also records `READ_MODIFY_WRITE` even when a component failed.
  Publishable trials need explicit requested/successful/failed conservation and
  a nonzero process exit on any SQL/client error.
- Observation: stock `warmuptime` is duration-based. Warm-up samples are
  discarded and each histogram begins at its first measured operation, but the
  printed total includes warm-up, different operation histograms have different
  first-sample origins, and faster modes execute more state-changing warm-up
  operations. The controlled workflow therefore uses a separate fixed-operation
  warm-up invocation, sets `warmuptime=0`, and records one authoritative client
  interval.
- Observation: go-ycsb v1.0.3's bundled property files do not match their own
  comments: A/B/C/E/F use `uniform`, and E sets `maxscanlength=1`. This plan
  defines and hashes its own clearly labeled `co-checker-ycsb-core-v1` profiles
  instead of silently calling the bundled files canonical YCSB.
- Observation: current scan evidence transport is implemented only by
  in-process UniStore clients. External TiKV does not carry the evidence
  sidecar. Workload E is supported only in the controlled UniStore topology.
- Observation: the online engine retains interned values and rollback history.
  Update-heavy YCSB can exceed the default 256 MiB retained-state limit, so a
  full-mode capacity pilot is mandatory before measured trials.
- Observation: the current TiDB working tree reproduces an attempt-count
  failure in `TestOnlineCaptureRealSQLLifecycleAndRetry` under plain `go test`,
  but the capture lifecycle is correct. `failpoint.Inject` is inert until TiDB's
  failpoint tool rewrites the package. The repository-mandated
  `tools/check/failpoint-go-test.sh` wrapper activates `mockCommitError8942`,
  produces the fifth retry attempt, passes the original assertions, and restores
  the source afterward. Evidence: `pkg/session/test/txn/online_capture_test.go`,
  `pkg/session/txn.go`, and `docs/agents/testing-flow.md`.
- Observation: the pre-existing `online/build-release` binaries dated
  2026-08-04 while current native source and CMake inputs dated 2026-08-07. A
  bare CTest invocation initially passed 4/4 against stale binaries. A current
  configure/build performed 29 build steps; the rebuilt suite then passed 4/4.
- Observation: TiDB normally changes transaction retry replay to a pessimistic
  provider whenever `RetryInfo.Retrying` is true. Unconditionally rejecting
  pessimistic checked transactions would therefore reject a retry whose
  original attempt was optimistic. Evidence: `pkg/session/session.go`, function
  `decideTxnMode`, and `pkg/session/test/txn/online_capture_test.go`, function
  `TestOnlineCaptureRealSQLLifecycleAndRetry`.
- Observation: prepared point SELECTs have a second optimization after planning:
  `session.executeStmtImpl` calls `ExecStmt.PointGet` directly and immediately
  invalidates the session transaction. That lifecycle is safe for the ordinary
  MaxTS short path, but after capture suppresses MaxTS it abandons the active
  capture attempt before the lazy result is consumed or published. Evidence:
  TiDB `pkg/session/session.go`, the `stmt.PsStmt != nil` branch, and the first
  focused prepared point-get test run, which registered a normal start TS but
  left the sink active with no publication.
- Observation: this environment did not have `bazel` or Bazelisk on `PATH`.
  Building Bazelisk v1.25.0 from the existing Go module cache into a temporary
  directory supplied the repository-pinned Bazel 7.7.1; `make bazel_prepare`
  then completed successfully. No repository tooling was vendored or modified
  to work around the missing executable.
- Observation: after making the upstream working-directory-relative workload
  test self-contained, the exact patched package set passes from an arbitrary
  fresh build directory. Two independent clean exports produced the identical
  `-trimpath` binary SHA-256
  `cb9b81e33a2e713f75b9c00339f9f7e8a6bba290bede672a04d36a030ce7f30f`.
- Observation: TiDB rejects the SQL `READ ONLY` transaction characteristic
  produced by `database/sql` unless `tidb_enable_noop_functions` is enabled,
  reporting Error 1235 because the characteristic is only a compatibility
  no-op. The frozen checked DSN deliberately does not enable no-op functions.
  A digest page therefore uses an explicit optimistic transaction that is
  read-only by construction: its only statement is the bounded keyset SELECT,
  followed by commit. The first live Milestone 3 run exposed this before any
  digest result was accepted; the schema, emptiness proof, and full YCSB gate
  had already passed.
- Observation: the planned plain `go test -race ./pkg/store/copr` command is
  not a valid TiDB package-suite invocation. Without failpoint rewriting,
  existing `TestCheckAliveStore` passes a nil region cache down a production
  branch and panics; adding only `-tags=intest` instead makes several tests
  observe `intest.InTest` while their `failpoint.Inject` bodies remain inert.
  The repository wrapper enables both the `intest,deadlock` tags and failpoint
  source rewriting, after which the complete race package passes and the
  wrapper restores the source. `pkg/store/driver/txn` does not need the wrapper
  and passed the original plain race command.
- Observation: TiDB's Bazel failpoint workflow is not usable in this working
  tree. Both `make bazel-failpoint-enable` and the otherwise working local
  `make failpoint-enable` rewrite unqualified injections to expressions that
  retain the sentinel identifier `_curpkg_`; Bazel's nogo/type checker then
  fails in existing files such as `pkg/util/injectfailpoint/random_retry.go`
  or `pkg/util/intest/assert_common.go`. With ordinary sources, the complete
  `//pkg/session/test/txn:txn_test` target builds and runs, and its only three
  failures are existing tests whose injected branches are necessarily inert:
  `TestCommitTSOrderCheck`, `TestOnlineCaptureRealSQLLifecycleAndRetry`, and
  `TestOnlineCaptureEarlyResultCloseWaitsForCopWorker`. The exact YCSB-filtered
  Bazel target passes. Failpoint-aware Go wrapper runs separately passed the
  retry and complete coprocessor race suites. Every attempted rewrite was
  disabled afterward and the serialized refcount returned to zero.
- Observation: go-ycsb's Zipfian generator has an inclusive upper bound, but
  Core v1.0.3 overwrites the already-correct loaded-key upper bound with
  `insertStart + insertCount + expectedNewKeys`. For a workload with no
  inserts, that admits key number `recordcount`, which was never loaded. The
  first strict live warm-up selected exactly that key and correctly failed.
  The pinned patch now subtracts one and includes a deterministic regression
  that samples 10,000 Zipfian keys and requires every key to remain in
  `[0, recordcount)`. The complete patched package, race, and vet selections
  pass from a fresh export.
- Observation: `TIDB_ONLINE_CHECKER_CLUSTER_EPOCH` is an unsigned integer, not
  a free-form run label. The first checked runner startup failed closed when an
  otherwise unique nanosecond epoch had workload/mode suffixes. The runner now
  records the unique numeric nanosecond value; a direct artifact startup and
  all three checked live diagnostic modes passed.
- Observation: the live metrics' capacity high-water fields are
  `capture_peak_bytes` and `peak_retained_state_bytes`. The active-byte fields
  return to zero at a drained sampling boundary and cannot represent capacity
  demand. The analyzer now reports the high-water fields and continues to use
  active fields only for ownership/drain validation.
- Observation: the plan's nominal checker capacities total 66 GiB before its
  required 8 GiB host headroom, exceeding the 53.5 GiB available on the target
  host. The complete A-F full-mode smoke instead observed at most 3.87 MiB of
  capture buffer and 84.2 KiB of queue occupancy. Retained state scaled at no
  more than 1,145 bytes per initial row or successful write; the largest
  one-million-row projection, Workload A, is 6.90 GiB including 30% headroom.
  The measured run therefore uses 1 GiB queue, 2 GiB capture, and 12 GiB
  retained-state caps, leaving more than the required 8 GiB host headroom.
- Observation: 16-client Zipfian A produced 35,877 retryable write conflicts in
  the first 87.9 seconds of measured capture-only execution. Exactly one logical
  operation reached TiDB's default `tidb_retry_limit=10`, after which the strict
  client correctly failed and canceled the remaining work. Before that failure,
  warm-up and measured execution had completed 1,735,804 logical operations;
  capture closed with zero active ownership, fatal calls, publish errors, or
  abort errors. The raw root was retained through diagnosis and then pruned on
  user request after these findings were recorded in this plan.
  TiDB's retry loop applies jittered exponential backoff capped at 100 ms;
  `tidb_retry_limit` is a session variable with default 10 and no practical
  upper bound. TiDB's own Error 9007 guidance says to raise the retry limit when
  automatic retry is already enabled. Evidence: TiDB `pkg/session/session.go`,
  `pkg/kv/txn.go`, `pkg/sessionctx/variable/sysvar.go`, and
  `docs/design/2020-05-08-standardize-error-codes-and-messages.md`.
- Observation: retry configuration is equal across the four modes, but replay
  strategy is intentionally different. Pristine TiDB changes an optimistic
  retry replay to a pessimistic provider and ordinarily resolves the conflict
  on its first replay; checked capture keeps replay optimistic to honor the
  optimistic-SI-only checker contract. In the failed evidence, baseline logged
  89,705 first replays and none reached a second replay, while capture logged
  35,874 replays, 20,636 at retry count one or higher, and one exhaustion at
  ten. This additional checked-mode TSO/replay/abort work is real tool overhead,
  not a pairing mismatch. Evidence: pristine/instrumented
  `pkg/session/session.go`, `decideTxnMode`, plus the preserved server logs.
- Observation: fresh stores are deterministic re-loads, not clones of the
  pristine reference. Measured evidence showed a 100,000-row A attempt with
  2.2 million total operations used 2.1 GiB, and the failed one-million-row A
  baseline with roughly 7.95 million total operations used 6.1 GiB. A
  conservative model of 315 MB per-store overhead, 1,600 bytes per initial row,
  1,476 bytes per successful write, 250 MB per-attempt logs, references,
  artifacts, and automatic smoke projects about 429 GB for the 500,000-row
  matrix but over 529 GB for one million rows. Only 488.5 GB (455 GiB) remained
  after preserving the required failed and retry-pilot roots, so one million
  rows no longer passed the disk-capacity gate.
- Observation: the SQL helper's default 30-second operation context covers an
  entire canonical YCSB digest, not one keyset page. The six otherwise idle
  pristine references happened to finish within it, but the first matrix cell
  loaded 500,000 rows in 170.623 seconds and its immediately following digest
  expired after 30 seconds on the page after key
  `user8909272566108302061`. All 500,000 inserts had succeeded with zero client
  failures; the runner correctly stopped before warm-up or measurement and
  retained the failed evidence through diagnosis. That raw root was later
  pruned under the final-results-only policy after these facts were recorded.
- Observation: retaining that required 19,634,814,976-byte stopped root changed
  the capacity decision again. The filesystem had 465,282,539,520 bytes free;
  the same conservative model projected 429 GB for a fresh 500,000-row matrix,
  leaving 36.28 GB. With 400,000 rows and the same attempts, writes, and logs,
  the projection is 408.84 GB and leaves 56.44 GB. The change does not reduce
  warm-up or measured operations, and the run still rejects any pristine cell
  below 180 seconds.
- Observation: successful raw retention, rather than the compact measurements,
  dominated disk use. One completed A baseline used 5,969,219,584 bytes for its
  UniStore directory and 210,550,784 bytes for all non-store artifacts; the
  latter was almost entirely a 209,272,832-byte TiDB log containing internal
  conflict warnings. Compact accepted summaries for the smoke, retry pilot,
  and Milestone-5 diagnostic total 332 KB after pruning. A live retention smoke
  proved two valid baseline/full cells can leave a 64 KB final root.
- Observation: a stopped database image is not sufficient to initialize full
  checking. The native checker keeps its version history in memory; when full
  mode opened a preloaded clone, its first present-value reads had no captured
  source write and correctly produced `NoCompatibleSnapshot`. Therefore every
  full run must execute the initial inserts while capture is active. YCSB value
  generation is part of that same untimed load command, not a separate costly
  preparation phase.
- Observation: the A/B/C/D/F loss is transaction-frequency amplification, not
  native queue backpressure. In the 100,000-row headline run, point workloads
  add 69-77% TiDB CPU per logical operation and accumulate 9.58-13.25
  waiter-thread-equivalents at the serialized finalization guard. Three C
  goroutine snapshots found 10, 10, and 11 of 16 client sessions stopped in
  `txnCaptureFinalizationCoordinator.acquire`. All cells had zero queue
  backpressure and no sustained backlog. E captured 47.37 events per
  publication, added only 19% CPU/operation, accumulated 0.65 waiter-thread-
  equivalents, and lost 13.56% throughput. Evidence:
  `benchmark/ycsb_online_e2e/overhead_profile_20260809.md` and the compact
  external profile root named there.
- Observation: the current retained serialized-global-lock TPC-C artifact does
  not show 17% full overhead: its three-trial median is 7.12% (4.17-9.01%). It
  executes about 1,423 successful NewOrder transactions/s and captures roughly
  63 events/publication, so its per-transaction serialization cost is amortized
  similarly to YCSB E. It is not comparable to 25,731 one-event C
  transactions/s solely by client count or total SQL statement count.
  Evidence: `/home/dyf/code/db-consistency/results/tpcc-global-lock-20260724-3x600s/tpcc-report.md`.

## Decision Log

- Decision: support optimistic repeatable-read only and add an explicit
  fail-closed rejection for pessimistic or non-RR checked transactions.
  Rationale: the checker validates classic snapshot reads; the existence of
  capture plumbing for current/locking reads is not semantic qualification.
  Date/Author: 2026-08-09 / Codex.
- Decision: when capture is active, suppress the optimistic MaxTS optimization
  before it installs `math.MaxUint64`; obtain a normal TiDB start TSO but do not
  add that timestamp to checker evidence. Preserve MaxTS unchanged with the
  runtime disabled.
  Rationale: this keeps database execution within optimistic SI, gives the
  recorder a real transaction to attach to, and charges the necessary TSO cost
  honestly as online-tool overhead.
  Date/Author: 2026-08-09 / Codex.
- Decision: keep TiDB's later MaxTS rejection as a defensive backstop.
  Rationale: the supported path should avoid MaxTS upstream; silently accepting
  a MaxTS transaction arriving through another route could omit evidence.
  Date/Author: 2026-08-09 / Codex.
- Decision: automatic retry replay remains optimistic while a checked capture
  runtime is active; the ordinary disabled path retains TiDB's existing
  pessimistic replay choice.
  Rationale: exempting retry-created pessimistic providers would violate the
  optimistic-SI-only contract, while failing every retry would contradict the
  supported autocommit retry and zero-client-error workflow. Restricting the
  change to an actually opened capture session preserves pristine and
  instrumented-disabled behavior. The retry regression must prove the original
  and replay attempts share one logical identity and use distinct physical
  attempt identities.
  Date/Author: 2026-08-09 / Codex.
- Decision: bypass TiDB's prepared point-get execution short path only while an
  opened capture runtime requires a real start TS; run the same prepared plan
  through the ordinary lazy result and autocommit lifecycle instead. Preserve
  the short path unchanged when capture is disabled.
  Rationale: a real TSO-backed transaction must remain valid until the result is
  consumed so exact evidence can be finalized. Reusing the existing ordinary
  lifecycle is narrower and safer than duplicating commit/capture ownership in
  the MaxTS-specific short path.
  Date/Author: 2026-08-09 / Codex.
- Decision: patch a pinned go-ycsb v1.0.3 source at commit
  `f030f9942393a8febbf4365c2d582711723159f5`; never build a floating branch or
  download moving HEAD during an experiment.
  Rationale: client behavior is part of the measured system and must be
  reproducible.
  Date/Author: 2026-08-09 / Codex.
- Decision: correct the pinned client's Zipfian inclusive-upper-bound error in
  the versioned patch rather than weakening `mysql.strict_rows` or accepting
  missing point reads.
  Rationale: key `recordcount` is outside the exact loaded set; accepting the
  read would turn deterministic client key-generation corruption into an
  apparently valid database operation and violate zero-error conservation.
  Date/Author: 2026-08-09 / Codex.
- Decision: derive workload and throttle seeds with two domain-separated
  SplitMix64-v1 streams and carry the workload seed in the worker context.
  Rationale: this preserves the upstream interfaces, gives every thread stable
  independent randomness, and prevents pacing jitter from consuming workload
  RNG state.
  Date/Author: 2026-08-09 / Codex.
- Decision: publish structured client results by syncing a same-directory
  temporary file and hard-linking it to the final path.
  Rationale: POSIX hard-link creation is atomic and refuses an existing target,
  avoiding `rename`'s overwrite semantics while retaining a complete temporary
  file on every successful publish path.
  Date/Author: 2026-08-09 / Codex.
- Decision: pre-create the schema outside capture, tell patched go-ycsb not to
  create it, and load all rows after capture starts with analysis disabled.
  Rationale: the checker starts from an empty user-key history. Starting it
  after loading would make existing values look unexplained.
  Date/Author: 2026-08-09 / Codex.
- Decision: use `batch.size=1` in the first supported profile.
  Rationale: this qualifies the ordinary point/scan SQL paths first and avoids
  conflating YCSB batching with TiDB store batching or BatchCop.
  Date/Author: 2026-08-09 / Codex.
- Decision: implement each bounded digest page as a query-only explicit
  transaction instead of requesting TiDB's no-op SQL `READ ONLY`
  characteristic.
  Rationale: this preserves a distinct optimistic-RR snapshot and the hard
  page bound without expanding the frozen DSN or presenting a compatibility
  no-op as an enforcement mechanism. Tests and the gate ensure the helper owns
  the only statement in that transaction.
  Date/Author: 2026-08-09 / Codex.
- Decision: run the complete `pkg/store/copr` race suite through TiDB's
  failpoint-aware Go-test wrapper; retain plain `go test -race` for
  `pkg/store/driver/txn`.
  Rationale: this is the repository-defined mechanism that supplies the test
  build tags and rewrites `failpoint.Inject` markers together. Either part in
  isolation changes the meaning of existing tests and produces false failures.
  Date/Author: 2026-08-09 / Codex.
- Decision: treat the YCSB-filtered ordinary-source Bazel target plus the
  failpoint-aware Go-wrapper regressions as Milestone 4's Bazel policy evidence;
  retain the failed complete Bazel attempt as an explicit toolchain limitation
  rather than changing production or existing test source to accommodate the
  `_curpkg_` sentinel.
  Rationale: the complete ordinary-source target proves all code compiles and
  isolates exactly three injection-dependent failures, while the repository
  wrapper proves the modified capture branches under real failpoint rewriting.
  A workaround in product code would be unrelated and would weaken the
  repository's intended test semantics.
  Date/Author: 2026-08-09 / Codex.
- Decision: define checked-in `co-checker-ycsb-core-v1` A-F properties. A, B,
  C, E, and F use Zipfian request selection; D uses `latest`; E uses uniform
  scan lengths from 1 through 100. Every result records the properties and
  hashes, including scan bounds.
  Rationale: these settings match the names normally associated with YCSB Core
  while avoiding the contradictory properties in the pinned client's bundled
  files.
  Date/Author: 2026-08-09 / Codex.
- Decision: use fixed-operation warm-up and measurement commands with distinct
  deterministic seeds. Use successful top-level YCSB operations, not DB calls,
  as the primary throughput numerator.
  Rationale: fixed work makes mode comparisons and drain accounting unambiguous;
  Workload F expands one top-level RMW into two DB transactions.
  Date/Author: 2026-08-09 / Codex.
- Decision: require zero client errors for the first supported benchmark. Do
  not silently whitelist optimistic Error 9007.
  Rationale: one-statement autocommit operations are retryable inside TiDB, and
  a zero-error initial policy is simpler and fail-closed. If evidence later
  requires a continuation policy, version it in this plan and in result
  manifests before changing acceptance.
  Date/Author: 2026-08-09 / Codex.
- Decision: keep the YCSB runner/analyzer separate from the proven TPC-C files.
  Port their immutable-build, telemetry, drain, lifecycle, append-only-attempt,
  and resume behavior without refactoring shared shell code in this milestone.
  Rationale: a broad harness refactor would couple qualification to unrelated
  risk. Consolidation can follow behavioral parity.
  Date/Author: 2026-08-09 / Codex.
- Decision: report A-F independently. Never aggregate different workloads into
  one overhead number.
  Rationale: point reads, updates, inserts, scans, and RMW expansion exercise
  materially different capture/checker costs.
  Date/Author: 2026-08-09 / Codex.
- Decision: freeze one million initial records, 16 clients, and workload-local
  operation counts equal to the observed pristine smoke rate times 60 seconds
  for warm-up and 600 seconds for measurement. Use 1 GiB queue, 2 GiB capture,
  and 12 GiB retained-state capacity for every paired mode and trial.
  Rationale: the functional smoke is also a proportional full-mode capacity
  pilot over the exact profiles. A conservative per-row/write projection with
  30% headroom fits the preferred profile, while the reduced fixed capacities
  pass the runner's host-memory preflight and retain at least 1.74x the largest
  projection. The frozen derivation and raw input hashes are recorded in
  `benchmark/ycsb_online_e2e/calibration_20260809T144157Z.json`.
  Date/Author: 2026-08-09 / Codex.
- Decision: set and capability-gate `tidb_retry_limit=100` for every load,
  control, baseline, and checked connection; record it in calibration and the
  experiment manifest. Continue to require zero client-visible errors and do
  not add a client-side retry or Error 9007 allowlist.
  Rationale: the failure was the expected database-internal resolution of
  concurrent optimistic autocommit writes reaching an undersized default
  attempt bound, not an unsupported transaction semantic or checker failure.
  Raising TiDB's uniform session limit preserves one logical YCSB operation,
  makes every physical abort/replay visible to checked capture, retains bounded
  jittered backoff, and follows TiDB's documented remedy. The value 100 matches
  TiDB's existing internal `kv.MaxRetryCnt` convention and a repository
  transaction stress test. Applying it equally to all four modes preserves the
  paired comparison. The changed DSN/calibration requires a new immutable
  artifact and experiment identity; the failed root is retained rather than
  resumed or overwritten.
  Date/Author: 2026-08-09 / Codex.
- Decision: publish the five-trial matrix at 500,000 initial rows rather than
  the preferred one million, while retaining 16 clients and the previously
  frozen fixed warm-up/measured operation counts. Continue using 1/2/12 GiB
  queue/capture/retained-state caps.
  Rationale: the complete matrix retains every fresh store and raw log; the
  source-observed storage projection exceeds current capacity at one million
  rows but leaves about 59 GB decimal reserve at 500,000. The fixed operation
  counts still project at least 180 seconds of pristine execution per workload,
  and retained-state projections remain below 6.21 GiB including 30% headroom.
  This follows the plan's explicit capacity fallback and will be reported as a
  500,000-row profile, never as the preferred one-million-row profile. The
  original calibration remains immutable historical evidence; a new
  500,000-row calibration freezes the corrected capacity decision.
  Date/Author: 2026-08-09 / Codex.
- Decision: give every YCSB schema/load/post-check digest an explicit 10-minute
  whole-operation deadline, expose it as `--digest-timeout`, and record it in
  the immutable experiment manifest. Keep the existing 1,000-row transaction
  and memory bound for every keyset page.
  Rationale: a fixed 30-second deadline was an undocumented helper default, not
  a semantic or resource bound in this plan, and source plus the preserved
  successful-load artifact prove it can be shorter than a valid 500,000-row
  digest under ordinary compaction. Ten minutes remains a fail-closed finite
  liveness bound while leaving digest content, pagination, and untimed status
  unchanged. The runner source and experiment identity change, so the stopped
  root is evidence only and cannot be resumed into the corrected run.
  Date/Author: 2026-08-09 / Codex.
- Decision: supersede the intermediate 500,000-row calibration with a
  400,000-row calibration after preserving the failed startup root. Change no
  other workload, concurrency, operation-count, seed, capacity, or CPU input.
  Rationale: evidence retention is mandatory and consumed 19.63 GB. The new
  profile restores nearly the previous storage safety margin (56.44 GB versus
  58.15 GB) while remaining above the independently qualified 100,000-key
  retry pilot and continuing to enforce at least 180 seconds of observed
  pristine work. It will be reported only as a 400,000-row profile.
  Date/Author: 2026-08-09 / Codex.
- Decision: permit an explicit mode subset only when `--diagnostic-only` is
  present. Require baseline first plus at least one unique checked mode; derive
  rotation, selection, and summaries from the declared subset. Keep the exact
  four-mode requirement for all publishable runs and ordinary smoke suites.
  Rationale: the user requested a quick baseline-versus-full estimate before
  spending time on capture/order modes. Making the subset manifest-bound and
  diagnostically labeled avoids ad hoc skipped directories or accidental use
  as publication evidence. Syntax, 9/9 fixtures, 5/5 CTests, positive exact
  validation, and negative non-diagnostic rejection pass.
  Date/Author: 2026-08-09 / Codex.
- Decision: default successful runs to `final-results-only` retention and TiDB
  log level `error`. Delete each successful attempt's store immediately after
  analyzer validation; after the suite's atomic summaries and valid marker are
  written, delete compact per-attempt/reference evidence and build snapshots.
  Record every pruned path in `retention.json`. Keep failed details only for
  diagnosis, with `--retain-details` and a higher log level available for an
  explicit debugging/profiling rerun.
  Rationale: the user explicitly requested final results rather than tens of
  gigabytes of routine raw data. Store contents are required while TiDB runs but
  do not contribute to reported performance after digest, lifecycle, telemetry,
  and result validation have succeeded. This also removes the disk-capacity
  reason for the 400,000-row fallback, so diagnostic comparison can return to
  the preferred one-million-row calibration without retaining all stores.
  Date/Author: 2026-08-09 / Codex.
- Decision: permanently use load seed `7732542672685676034`, warm-up seed
  `6300581044758642952`, and measured seed `1251957853004150076` for every
  workload, mode, trial, and future run. Start every mode from an empty store
  and execute the same seeded YCSB load SQL. Keep one compact pristine digest
  reference per suite and never use a preloaded physical store as full-checker
  initialization.
  Rationale: a seed changes operation choices, keys, scan lengths, and values.
  Fixing it removes client-input variance from baseline/full pairing and makes
  repeated trials measure system variance. Fresh captured inserts preserve the
  required checker history and are outside the measured interval.
  Date/Author: 2026-08-09 / Codex.
- Decision: retain only compact pprof/perf/blocking-stack reports from the
  explicit C/E profiling run and prune its generated stores, build snapshots,
  and ordinary logs after recording hashes and conclusions.
  Rationale: profiling required raw evidence temporarily, but the user does not
  want multi-gigabyte routine logs. The 5.2 MiB compact root contains the raw
  CPU/perf profiles needed to revisit symbol attribution plus human-readable
  reports, exact run summaries, and headline/TPC-C reference CSVs.
  Date/Author: 2026-08-09 / Codex.

## Outcomes & Retrospective

Planning and Milestone 0 baseline qualification are complete. The baseline is:

- co-checker commit `5e78b27a0abb015dfab46743427ddded9f9a84db`,
  tracked-diff SHA-256
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`,
  and untracked-input aggregate SHA-256
  `09805d1b8a6dd9f8867d1d1330a74cc365cd53c1e6e84565020d1b12a25f6bae`;
- tidb-online commit `a570cb08e95e5fd544b669eed508c54255b2d7ac`,
  tracked-diff SHA-256
  `574b8f1ce47a35caa9c6648e649637c99d70f1c6f33609fb18e7fa6fa7e19a58`,
  and untracked-input aggregate SHA-256
  `1deea59753cfe7737b4e122de05b94f0fe97ea000ce6446e3ac01ed107b8c430`;
- checker build toolchain Go 1.22.2, GCC/G++ 13.3.0, CMake 3.28.3,
  Ninja 1.11.1; TiDB module Go 1.25.10;
- corrected failpoint-enabled retry test passed, the two focused coprocessor/
  no-op-DDL tests passed, and the freshly configured and rebuilt native suite
  passed 4/4.

Milestone 1 added the real-start-TS runtime predicate, disabled both MaxTS
execution shortcuts only for active checked capture, rejected pessimistic and
non-RR activation, and kept checked retry replay optimistic. `make
bazel_prepare` completed with temporary cached Bazelisk v1.25.0/Bazel 7.7.1.
The failpoint-aware isolation package test, all `TestOnlineTxnCapture*` session
tests, the six focused point/mode/retry/serialized/TPC-C integration tests, and
the complete 12-test `TestOnlineCapture*` integration selection passed. The
prepared present/missing regressions prove nonzero, non-MaxTS registered start
timestamps, exact snapshot read evidence, and closed publication ownership.

Milestone 2 added the 2,469-line patch
`online/tools/go_ycsb_v1.0.3_online_checker.patch` with SHA-256
`5c42eec49f1561e1a9e6db0430bcfaa50cac92eea692a8e5511d573e491f6c83`.
From a fresh pinned export, the exact five-package test command, targeted race
command, and `go vet` command passed. A strict five-operation basic-DB run exited
zero with `requested=attempted=successful=5`; an initialization failure exited
1 with `requested=failed=3`. Both emitted versioned structured results. The
binary and full `go version -m` evidence are retained under
`benchmark/ycsb_online_e2e/milestone2_build_20260809T131514Z/`, whose
`build-artifacts.json` records Go 1.22.2, linux/amd64, CGO enabled, the clean base
commit, patch hash, tests, and matching independent build hashes.

The first strict live runner warm-up subsequently exposed the upstream
Zipfian inclusive-bound error described above. The corrected current patch is
2,495 lines with SHA-256
`8bc3b361130b6a159a5847ccb126a1ebc1cdd9ff383002099467bc35f52bb6d5`;
the original Milestone 2 artifact remains historical evidence and is
superseded for every E2E run. A fresh export again passed the five-package,
race, and vet selections. The current immutable live diagnostic client has
SHA-256 `d6de1f2a202f2146d41c90eb9dd02cb0fce2c8479efb353499aa4e57ee8b145f`.

Milestone 3 added the six `co-checker-ycsb-core-v1` property profiles with
hard-coded SHA-256 expectations, exact schema/emptiness actions, a canonical
keyset-paginated digest, and a YCSB capability gate. The gate validates exact
table columns/index/nonpartitioning and all checked session variables, explains
text and prepared variants of the six client SQL shapes, executes real text and
prepared point/missing/range reads, and executes text/prepared update,
`INSERT IGNORE`, and delete probes inside one rolled-back transaction. From the
TiDB module, the exact helper unit test and `go vet` commands passed. A live
single-row UniStore qualification with coprocessor cache disabled produced 12
accepted plans and 12 successful probes. Digest page sizes 1 and 2 produced the
same schema, data, and combined digest
`7d33ec8de832ec72891bfbede2925148d6d7156c4de131590f18debb47707609`;
the page counts were 2 and 1, respectively, with no page exceeding its bound.
The immutable source/binary/profile hashes, JSON results, logs, and manifest are
under `benchmark/ycsb_online_e2e/milestone3_live_20260809T132641Z/`.

Milestone 4 added `pkg/session/test/txn/online_ycsb_capture_test.go`. Its
binding test executes captured load inserts, duplicate `INSERT IGNORE`, text
and prepared present/missing point reads, prepared-cache reuse, changed and
unchanged updates, several/selected/short range scans, and present/missing
deletes. It asserts exact reads/writes, affected-row no-op behavior, terminal
attempts, and zero fatal state. A remote-scan regression proves missing
cop-read evidence fails closed. The native end-to-end test starts from the
empty schema, captures eight ordered operations, and closes with authoritative
`outcome=pass`, exact accepted/ordered/checked and event conservation, zero
violations/errors, and no active state. Unsupported-mode tests now also cover
stale and preset-snapshot reads.

The exact focused `go test -tags=intest` YCSB selection, native-library YCSB
test, session unsupported-mode units, executor reader-plan units, the combined
YCSB/TPC-C/point/mode/coprocessor integration regression, plain transaction
driver race suite, and failpoint-wrapper coprocessor race suite all passed.
`make bazel_prepare` registered the new source. The ordinary-source Bazel
target passed with `--test_filter='^TestOnlineCaptureYCSB'`; the full target
built but had only the three documented failpoint-inert failures. Attempts to
use either supported failpoint rewriter failed at Bazel/nogo before tests due
to unresolved `_curpkg_`, and cleanup restored sources with refcount zero.

Milestones 5 and 6 added
`online/scripts/run_ycsb_global_lock_comparison.sh` and
`online/tools/analyze_ycsb_global_lock.py`, registered the analyzer in CTest,
and added malformed/valid fixtures. The runner rebuilds manifest-bound Release
checker, pristine/instrumented TiDB, SQL helper, patched client, profiles,
runner, and analyzer from captured source snapshots without modifying the
development worktrees. It validates the exact four-mode matrix, calibration,
seeds, fresh stores, pristine load references, strict client conservation,
bounded digests, capability gate, telemetry cutoff/coverage, live drain,
graceful checker close, capture-off post-check, append-only retry, and exact
resume identity. The analyzer selects only declared matrix entries, keeps
missing/invalid rows, computes fixed and completion rates plus paired overhead,
reports measured physical abort counts/rates from start-to-cutoff adapter
deltas, and atomically writes all five required summaries.

Eight analyzer/runner fixture tests cover valid and invalid attempts, missing
modes, duplicate append-only attempts, mismatched manifests, Workload F DB-call
expansion, D/E row growth, lifecycle imbalance, failed verdict, telemetry gaps,
paired overhead, atomic replacement, and malformed runner arguments. The
current syntax/unit run passed, and the freshly reconfigured native CTest tree
passed 5/5 including both TPC-C and YCSB analyzers. The live diagnostic root
`/home/dyf/code/db-consistency/ycsb-online-results/milestone5_diag_20260809T143500Z`
contains four valid Workload A attempts. Full mode closed with `outcome=pass`,
105 accepted/ordered/checked transactions, 176 accepted/checked events, zero
active ownership, zero violations/errors, and identical 20-row initial load
digest across modes. These tiny throughput numbers are functional evidence,
not performance results.

Milestone 7's required functional smoke is complete at
`/home/dyf/code/db-consistency/ycsb-online-results/milestone7_smoke_20260809T144500Z`.
All 24 A-F/four-mode attempts are valid, with zero retries or client errors;
each serialized-full result closed with `outcome=pass`, complete telemetry,
zero residual/active ownership, and no sustainability warning. Workload E
returned scan KV evidence (611,696 accepted and checked events), D/E grew by
627/632 rows including warm-up, and F reported 4,966 logical RMW operations as
10,000 reads plus 4,966 updates. The summary input/output SHA-256 values are
recorded in the frozen calibration.

The first one-million-row calibration remains immutable historical evidence at
`benchmark/ycsb_online_e2e/calibration_20260809T144157Z.json`, SHA-256
`0ffe126973644551305885c50fe56b8f9911893126de00e227d451c2cc8085d2`.
It is superseded for publication by the storage-capacity decision above.
The intermediate 500,000-row calibration is
`benchmark/ycsb_online_e2e/calibration_20260809T161500Z_500k.json`, SHA-256
`38b18ca6cdd1d2ec9884a1cd07aea47c24129f8fd8d638c58652e5ccd8b784a7`.
It freezes 500,000 records, ten 100-byte fields, 16 clients, CPU sets 0-7 and
8-15, SplitMix64 seeds, `tidb_retry_limit=100`, five-trial rotation, unchanged
positive A-F operation counts, the 1/2/12 GiB checker capacities, observed
retry/capacity pilot hashes, and the 429 GB result-root projection. The runner's
measured `--validate-only` gate passed with this file.
It is superseded after preserving the failed matrix-start root. The publishable
calibration is
`benchmark/ycsb_online_e2e/calibration_20260809T165300Z_400k.json`, SHA-256
`92e60af596cb5203e5c09a9ffdf08bd6646c2a283c9e56c67663d330d687b07f`.
It freezes 400,000 records, the same ten 100-byte fields, 16 clients, exact
A-F fixed operation counts, retry limit, seeds, CPU sets, rotation, and 1/2/12
GiB capacities. Its storage evidence records 465,282,539,520 bytes available,
a 408.84 GB complete-root projection, and 56.44 GB reserve. The exact measured
`--validate-only` gate passes and the file is mode 0444.

The retry qualification root is
`/home/dyf/code/db-consistency/ycsb-online-results/milestone7_retry_pilot_20260809T160000Z`.
All four 16-thread A modes are valid for 200,000 warm-up plus 2,000,000 measured
operations over 100,000 keys. Baseline completed in 63.628 seconds;
capture-only/order-only/full completed in 210.785/182.766/193.271 seconds with
43,082/42,438/43,278 measured physical aborts (2.11/2.08/2.12% of registered
attempts), zero client errors, and complete drain. Full retained-state peak was
1,331,232,720 bytes, full outcome was `pass`, and the new analyzer fixtures and
fresh native CTest suite pass 8/8 and 5/5. These are qualification observations,
not publishable five-trial performance results.

The first 500,000-row matrix root at
`/home/dyf/code/db-consistency/ycsb-online-results/milestone7_measured_500k_20260809T162000Z`
passed its mandatory 24-cell fresh-store smoke and created six valid 500,000-row
references. Its first measured cell then failed closed before timing: load
reported `attempted=successful=500000`, `failed=0`, but the post-load digest's
default 30-second context expired on its final page. This root was not resumed
and was pruned after diagnosis under the later user-directed retention policy.
The corrected runner passes `bash -n`, 8/8 analyzer/runner
fixtures, 5/5 native CTests, the explicit-file helper unit test and `go vet`
from the TiDB module, and exact 500,000-row `--validate-only`.

At user request, superseded/incomplete raw roots and successful raw attempt
trees were explicitly pruned after diagnosis/summary generation. The external
results directory fell from about 113 GB to 332 KB, retaining compact accepted
summaries for the Milestone-5 diagnostic, A-F smoke, and retry pilot. The new
default retention policy passed `bash -n`, 9/9 analyzer/runner fixtures, 5/5
CTest, and exact diagnostic validation. Its live baseline/full proof at
`/home/dyf/code/db-consistency/ycsb-online-results/retention_smoke_20260809T181500Z`
has two valid rows, zero invalid rows, error-level TiDB logging, an explicit
retention record, no store/attempt/build directories, and total size 64 KB.

The short-lived physical-store reuse experiment also produced a valid two-cell
smoke, but full mode could pass only when it performed its own captured load;
opening the preloaded store directly produced the expected missing-history
violation. The clone feature and its 261 MB debug cache were therefore removed,
not retained as a supported workflow. Future work uses the immutable fixed-seed
one-million-row calibration
`benchmark/ycsb_online_e2e/calibration_20260809_fixed_seed_1m.json`, SHA-256
`bd76f1591d7e45529288a8f4cccd319b3222a684b10950bf3736ec65580ef7bb`,
mode 0444. Exact measured diagnostic validation passes with this file.

At user request, the initial estimate instead uses the immutable 100,000-row
basic calibration
`benchmark/ycsb_online_e2e/calibration_20260809_fixed_seed_100k_basic.json`,
SHA-256
`70403c31bcae1b85859b61ee2488975177be370c8e7ef803628508461af30b21`,
mode 0444. The completed root is
`/home/dyf/code/db-consistency/ycsb-online-results/milestone7_baseline_full_fixed_100k_basic_20260809T193500Z`.
All 12 measured rows are valid, every full verdict is `pass`, and paired fixed
throughput overhead is A 65.11%, B 66.62%, C 57.29%, D 60.61%, E 13.56%, and
F 64.86%. The final-results-only root is 244 KB and retains the four atomic
summaries plus experiment, calibration, validation, and retention metadata;
its attempt stores, raw evidence, and build snapshots were pruned only after
analysis succeeded. SHA-256 values are `bd325ac229b7d54c4217285546abed948ee2639b057c462619e10b56bb45963c`
for `ycsb-trials.csv`, `08f1c3aedd5b645b5930d382f70b58acca6d4690f8e10c064e67829f3c34974e`
for `ycsb-performance.csv`, `13ee45680d5f9a5a341f3b08c021eafcd91fc6e045cbfaf87087595646e40b3d`
for `ycsb-summary.json`, and `f2afa5ca5609dbeda78364e11de5d5b9a261aa15d1c8d76e47093c7ae5b123ac`
for `ycsb-report.md`.

The follow-up C/E profile reproduced the headline shape at 100,000 rows and 16
clients: unprofiled/delayed-snapshot C lost 56.99% and separately profiled E
lost 12.38%. Headline data, not the profiler-perturbed samples, remains the
performance result. The diagnosis and exact per-workload decomposition are in
`benchmark/ycsb_online_e2e/overhead_profile_20260809.md` and `.csv`. Raw CPU,
perf, and blocking-stack evidence is retained under
`/home/dyf/code/db-consistency/ycsb-online-results/ycsb_overhead_profile_20260809T200930Z`;
the `SHA256SUMS` file hashes to
`b2e072b27d03d9546090d6abc80830749046e025ba4a93041444016f4d0f262e`.

No complete five-trial performance result exists yet.
When each milestone finishes, record the commands, test outcomes, artifact
paths, and deviations here. At completion, summarize the supported workload
matrix, exact commits and patches, smoke and measured result roots, valid trial
counts, median paired overhead, capacity behavior, and any remaining topology
limits. Under final-results-only retention, record any failed diagnosis in this
plan before explicitly pruning its temporary raw tree.

## Context and Orientation

The checker repository is `/home/dyf/code/db-consistency/co-checker`. Its
generic online C++ runtime is under `online/`. The instrumented TiDB repository
is `/home/dyf/code/db-consistency/tidb-online`. The supported existing live
workflow is documented in `online/doc/tpcc_workflow.md`, implemented by
`online/scripts/run_tpcc_global_lock_comparison.sh`, and summarized by
`online/tools/analyze_tpcc_global_lock.py`. Use those files as proven behavior,
not as code to modify for YCSB.

The checker receives transaction attempts containing raw physical KV read/write
events plus commit/completion ordering. It treats transaction start time as
unknown and asks whether all snapshot reads admit a compatible commit-order
prefix. TiDB may obtain a numerical start TSO to implement MVCC, but that number
remains execution metadata rather than checker evidence.

The first YCSB support target is Linux with cgo, one TiDB server, one producer,
and `--store unistore`. Session settings are optimistic
`REPEATABLE-READ`; async commit, 1PC, low-resolution TSO, stale reads, store
batching, BatchCop, and coprocessor cache are disabled. Tables are
nonpartitioned and noncached. Returned KVs from an admitted scan are ordinary
snapshot read events. Empty ranges produce no inferred absence, and neither the
producer nor checker represents a range predicate.

The controlled schema is `ycsb.usertable`, with a clustered `VARCHAR(64)`
primary key named `YCSB_KEY` and ten `VARCHAR(100)` fields named `FIELD0`
through `FIELD9`. The runner exposes record count, field count, and field length
as manifest inputs, but the publishable `co-checker-ycsb-core-v1` profile fixes
the field shape at ten by 100 bytes. A different shape is a different
experiment and cannot be resumed into the same result root.

The required SQL surface is prepared point SELECT, point UPDATE, `INSERT
IGNORE`, range SELECT with `YCSB_KEY >= ? LIMIT ?`, and the binding's point
DELETE. Workloads A-F use those operations as follows: A is 50% read and 50%
update; B is 95% read and 5% update; C is read-only; D is 95% read and 5%
insert with latest-key selection; E is 95% scan and 5% insert; F is 50% read
and 50% logical RMW. Workload F's RMW remains two autocommit TiDB transactions
and is labeled that way in every report.

## Supported-v1 Contract and Explicit Non-goals

Version 1 is complete only for the exact topology, isolation, client patch,
schema, and property profiles in this document. Point and UniStore scan events
are checked at the physical TiKV key/value level. Secondary index keys, if a
future profile adds them, are separate physical keys and require a new gate and
test before that profile is supported.

External TiKV scan capture, TiFlash, MPP, BatchCop, IndexMerge, partitioned
tables, cached or temporary tables, pessimistic transactions, read committed,
stale/weak reads, bulk load, `ANALYZE` inside the epoch, and `LOAD DATA` remain
fail-closed. Predicate semantics, scan exhaustion, missing ranges, and phantoms
are deliberate non-goals. Supporting a true multi-statement transactional YCSB
RMW is also a separate client/workload extension; this plan measures the pinned
client's actual transaction boundaries.

No native C++ checker change is planned. A native change is justified only if
implementation evidence proves the existing raw-KV event, ordering, or
capacity APIs insufficient. Such a discovery must be entered in `Decision Log`
before modifying the native contract.

## Plan of Work

### Milestone 0: Freeze the baseline and remove pre-existing uncertainty

Before editing behavior, capture both worktrees' commit, status, full diff hash,
compiler/Go versions, and current focused test output. The TiDB tree is already
dirty with the online instrumentation; preserve every unrelated user change.
Do not reset, clean, or switch either worktree.

Reproduce `TestOnlineCaptureRealSQLLifecycleAndRetry` alone. Diagnose why the
retry attempt is absent and correct the underlying test or behavior without
weakening lifecycle assertions. Record the root cause in `Surprises &
Discoveries`. The YCSB work may not claim a green baseline while this relevant
test still fails.

From the TiDB repository, run the failpoint-using retry test through the
repository wrapper, then run the non-failpoint focused cases directly:

    cd /home/dyf/code/db-consistency/tidb-online
    ./tools/check/failpoint-go-test.sh pkg/session/test/txn \
      -run '^TestOnlineCaptureRealSQLLifecycleAndRetry$' -count=1
    go test -tags=intest ./pkg/session/test/txn \
      -run 'TestOnlineCapture(CoprocessorReadsAndUnsupportedPaths|NoOpCreateTableStaysOutOfScope)$' \
      -count=1

From the checker repository, rebuild before CTest so stale development
artifacts cannot qualify the baseline:

    cd /home/dyf/code/db-consistency/co-checker
    cmake -S online -B online/build-release -G Ninja \
      -DCMAKE_BUILD_TYPE=Release
    cmake --build online/build-release -j2
    ctest --test-dir online/build-release --output-on-failure

Acceptance is a recorded baseline with all relevant tests passing. If unrelated
tests fail, preserve their complete output, establish ownership, and do not
silently exclude them.

### Milestone 1: Keep capture-active optimistic point reads on normal SI

Edit TiDB `pkg/session/online_txn_capture.go`. Add an exported duck-typed method
on `*session` named `TxnCaptureRequiresRealStartTS`. It returns true only when
the statement is capture-eligible and `s.txnCapture` is non-nil. Do not use only
the user-session marker: all MySQL sessions have that marker even when the
runtime is disabled, and disabling MaxTS in the pristine/instrumented-off path
would contaminate the baseline.

The intended method is:

    func (s *session) TxnCaptureRequiresRealStartTS() bool {
        return s.captureEligible() && s.txnCapture != nil
    }

`OnTxnCaptureLogicalStart` runs before provider initialization and ensures that
an active runtime has opened the per-session capture state. With no runtime,
`ensureTxnCaptureSession` leaves `s.txnCapture` nil.

Edit TiDB `pkg/sessiontxn/isolation/optimistic.go`, inside
`OptimisticTxnContextProvider.AdviseOptimizeWithPlan`. After
`IsPointGetWithPKOrUniqueKeyByAutoCommit` returns true and before
`forcePrepareConstStartTS(math.MaxUint64)`, query the anonymous interface:

    interface {
        TxnCaptureRequiresRealStartTS() bool
    }

If it returns true, return nil without setting `constStartTS` or
`optimizeWithMaxTS`. The caller's normal warm-up then obtains a real TSO,
activates the driver transaction, and attaches the recorder. If it returns
false, preserve the existing MaxTS behavior byte-for-byte. Retain the existing
`OnTxnCaptureUnsupported` call after the new branch as a compatibility/fail-
closed fallback for capture-aware contexts that do not request the supported
bypass. Retain `validateTxnCaptureMode`'s `StartTS()==math.MaxUint64` rejection
as a second backstop.

Prepared point plans also enter the `stmt.PsStmt != nil` execution short path in
`pkg/session/session.go`. That branch invalidates the transaction immediately
after constructing a lazy result and therefore cannot own a normal capture
attempt. Gate the branch with `!s.TxnCaptureRequiresRealStartTS()` so checked
prepared point reads use `runStmt` and complete through the ordinary result-set
close/autocommit path. The runtime-disabled MaxTS path remains unchanged.

TiDB's ordinary retry replay selects a pessimistic provider. In
`pkg/session/session.go`, function `decideTxnMode`, preserve that behavior when
capture is disabled, but return `ast.Optimistic` for `RetryInfo.Retrying` when
`TxnCaptureRequiresRealStartTS()` is true. This keeps every accepted checked
attempt within the supported optimistic SI semantics and preserves transparent
autocommit retry. The retry lifecycle test is the regression for logical versus
physical attempt identity.

Also make the semantic boundary explicit in
`session.validateTxnCaptureMode`: reject pessimistic transaction contexts and
reject any isolation other than repeatable-read. Use the session-variable
isolation helper rather than assuming `TxnCtx.Isolation` was eagerly populated.
The error messages must name `pessimistic transaction` or `non-repeatable-read
transaction`, flow through `ErrTxnCapture`, and poison the checked run before
read evidence is accepted.

Update positive existing tests that relied on TiDB's default pessimistic mode
to set optimistic RR explicitly. Convert positive pessimistic capture cases
into negative fail-closed tests. Capture plumbing unit tests may still test raw
pessimistic-cache helpers in isolation, but no full checked test may treat a
pessimistic transaction as supported.

Add tests proving all of the following:

- Capture disabled: the existing optimistic provider test still returns
  `math.MaxUint64` for an autocommit point get.
- Capture enabled on a marked user session: direct and prepared autocommit PK
  point SELECTs complete with nonzero, non-MaxTS registered start timestamps,
  exact present/missing read evidence, and no fatal error.
- Capture enabled: pessimistic RR and non-RR attempts fail before publication.
- Internal or unmarked sessions remain outside the population.
- Explicit optimistic transactions, including all TPC-C shapes, are unchanged.

Run:

    cd /home/dyf/code/db-consistency/tidb-online
    make bazel_prepare
    go test ./pkg/sessiontxn/isolation \
      -run '^TestOptimisticTxnContextProviderTS$' -count=1
    go test ./pkg/session -run 'TestOnlineTxnCapture' -count=1
    go test -tags=intest ./pkg/session/test/txn \
      -run 'TestOnlineCapture(OptimisticAutocommitPointGet|RealSQL|TPCC)' \
      -count=1

`make bazel_prepare` is mandatory because new top-level Go tests and import
changes require Bazel metadata regeneration under TiDB policy. Acceptance is
normal TSO capture with the runtime on, unchanged MaxTS with it off, explicit
semantic rejection, and a green prior TPC-C/capture suite.

### Milestone 2: Build a deterministic and strict pinned go-ycsb

Add `online/tools/go_ycsb_v1.0.3_online_checker.patch`. Its only valid base is
go-ycsb commit `f030f9942393a8febbf4365c2d582711723159f5`, tag v1.0.3. The
runner accepts `--go-ycsb-source PATH`, requires that checkout's HEAD to equal
the pinned commit and its tracked tree to be clean, exports that commit into an
artifact build directory, applies the checked-in patch, runs tests, and builds
with `go build -trimpath`. It never edits the supplied checkout.

The patch must preserve upstream defaults outside the controlled runner and add
these properties:

- `load.analyze`, default true. The runner sets false. If true, propagate
  `Analyze` errors instead of ignoring them.
- `mysql.create_table`, default true. The runner sets false after explicit
  schema bootstrap.
- `mysql.params`, a raw DSN query containing TiDB session variables. Construct
  DSNs through `go-sql-driver/mysql.Config` and parsed query parameters for both
  single-address and `tidb.instances` paths. Include parameters in the mux
  driver registration hash so replacement physical connections receive the
  same session settings.
- `randomseed`, optional for compatibility but mandatory and nonzero in the
  controlled runner. Derive stable independent per-thread seeds from the
  master seed and thread ID. Remove wall-clock RNG consumption in Zipfian and
  skewed-latest constructors, and use a derived worker RNG for target-throttle
  jitter.
- `strict`, default false. With true, any operation, initialization, Analyze,
  cleanup, or context error cancels peer workers, is preserved in a structured
  result, and makes the process exit nonzero.
- `mysql.strict_rows`, default false. The runner sets true. Point reads must
  find one row, and point update/insert/delete must affect exactly one row.
  Scans may legitimately return fewer rows than their requested limit.
- `result.path`, empty by default. The runner supplies a new path. Write a
  versioned JSON result atomically and refuse to overwrite an existing file.

Refactor `pkg/client.Client.Run` to return a result and error. Workers must
conserve `requested = successful + failed`; a failed operation is never counted
as successful. Convert load/run Cobra handlers to `RunE`, perform cleanup, and
make `main` return nonzero for command or close failures. Always emit diagnostic
measurements and the structured result when possible, even on failure.

The JSON result must contain format version, client/base-patch identity,
command, master seed and derivation version, properties digest, requested,
attempted, successful and failed top-level operations, start/end monotonic
timestamps and elapsed nanoseconds, per-error-class counts, and per-operation
measurement counts and p50/p95/p99/p99.9 latencies. For F, record both
successful top-level RMW operations and their READ/UPDATE DB-call counts. Fix
RMW measurement so a failed component cannot appear as a successful logical
RMW.

Add patch tests with fake workloads/DBs for Analyze suppression and propagation,
first-error cancellation, nonzero CLI status, operation conservation,
strict-row handling, single/multi-instance DSN parameters, deterministic
uniform/Zipfian/latest/scan sequences, different thread seeds, and atomic
result refusal. Do not enable `dataintegrity=true`: its fixed value for a given
key/field can turn updates into unchanged-row writes. Seed normal random values
instead.

Build acceptance from a clean pinned checkout is:

    git -C /absolute/path/to/go-ycsb status --short
    test "$(git -C /absolute/path/to/go-ycsb rev-parse HEAD)" = \
      f030f9942393a8febbf4365c2d582711723159f5
    cp -a /absolute/path/to/go-ycsb /tmp/go-ycsb-online-build
    patch -d /tmp/go-ycsb-online-build -p1 --forward --batch < \
      /home/dyf/code/db-consistency/co-checker/online/tools/go_ycsb_v1.0.3_online_checker.patch
    cd /tmp/go-ycsb-online-build
    go test ./pkg/client ./pkg/generator ./pkg/workload ./db/mysql ./cmd/go-ycsb
    go build -trimpath -o /tmp/go-ycsb-online ./cmd/go-ycsb

The current upstream `pkg/workload` test has a working-directory-relative
workload path; the patch must make the test self-contained rather than exclude
it. Record `go version -m`, build environment, base commit, patch SHA-256, and
binary SHA-256 in the immutable artifact manifest.

### Milestone 3: Add YCSB schema, profiles, digest, and capability gate

Create `online/scripts/ycsb/workloada.properties` through
`workloadf.properties`. Give the profile family the explicit ID
`co-checker-ycsb-core-v1`. Store only semantic workload settings in these files;
the runner supplies connection, schema size, operation count, seeds, paths, and
threads. Fix and record `fieldcount=10`, `fieldlength=100`,
`readallfields=true`, `writeallfields=false`, `insertorder=hashed`, and
`batch.size=1`. Use the A-F proportions and distributions stated in `Context
and Orientation`, including E scan lengths 1 through 100. Tests and manifests
hash every properties file.

Extend `online/tools/tidb_sql.go` and `tidb_sql_test.go` with four actions:

- `ycsb-schema` creates the database and exact clustered-primary-key table from
  validated field-count/length parameters. It runs only on the checker-off
  schema-bootstrap server.
- `assert-ycsb-empty` proves `usertable` has zero rows before the checked epoch.
- `ycsb-digest` reads bounded keyset pages in `YCSB_KEY` order and hashes one
  canonical stream of schema, key, null/present state, field lengths, and
  bytes. Each page is a separate query-only explicit transaction using
  `WHERE YCSB_KEY > ? ORDER BY YCSB_KEY LIMIT ?`; never use `OFFSET`. Make the
  page size an explicit argument with a conservative default such as 4,096
  rows, so digest validation cannot create one capture attempt containing the
  entire million-row table. It emits schema, data, and combined SHA-256 plus
  row count, page count, configured page size, and maximum observed page size
  in versioned JSON. Unit tests prove the digest is invariant across page sizes.
- `ycsb-gate` reads session variables on a newly opened connection, verifies
  table shape/nonpartitioning, and checks text plus actual prepared variants of
  point SELECT, missing SELECT, UPDATE, `INSERT IGNORE`, range SELECT with
  LIMIT, and DELETE. Mutating probes run in a transaction that is rolled back.

The gate requires `REPEATABLE-READ`, `OPTIMISTIC`, async commit and 1PC off,
low-resolution TSO off, no stale/preset snapshot, store batching and BatchCop
off, forced index-lookup pushdown policy, and the ordinary UniStore/TiKV reader
plans admitted by `pkg/executor/compiler.go`. It must execute a real prepared
point SELECT, not only `EXPLAIN`, so the E2E gate proves MaxTS suppression.

Use identical encoded DSN parameters in pristine and checked modes:

    transaction_isolation=%27REPEATABLE-READ%27&tidb_txn_mode=%27OPTIMISTIC%27&tidb_enable_async_commit=0&tidb_enable_1pc=0&tidb_low_resolution_tso=0&tidb_store_batch_size=0&tidb_allow_batch_cop=0&tidb_index_lookup_pushdown_policy=%27FORCE%27

Set coprocessor cache capacity to zero in the TiDB configuration for every
mode. Do not issue `SET GLOBAL` after capture begins.

Run SQL-helper tests from the TiDB module so its dependencies resolve:

    cd /home/dyf/code/db-consistency/tidb-online
    go test \
      /home/dyf/code/db-consistency/co-checker/online/tools/tidb_sql.go \
      /home/dyf/code/db-consistency/co-checker/online/tools/tidb_sql_test.go

Acceptance is deterministic schema/digest output independent of page size,
bounded rows per digest transaction, rejection of malformed or unexpected
schema/session/plans, and successful text/prepared gates on the supported
server.

### Milestone 4: Qualify exact YCSB SQL through TiDB and the native checker

Add TiDB `pkg/session/test/txn/online_ycsb_capture_test.go` and register it in
the corresponding Bazel target. Create schema and any setup rows through an
unmarked setup session before installing capture. Execute all user rows and
workload operations through a marked optimistic-RR session.

Focused tests must cover the exact patched binding shapes:

- prepared and text common-handle point SELECT, including present and missing
  keys, with nonzero/non-MaxTS start TSO and exact read evidence;
- point UPDATE of one field, `INSERT IGNORE`, and DELETE, including affected-row
  behavior and no-write completion where applicable;
- range `YCSB_KEY >= ? LIMIT ?` with several returned rows, selection rejection,
  and a short final range, verifying captured events equal the returned/
  consumed physical KVs and do not invent range absences;
- prepared-plan cache reuse and repeated session operations;
- `ANALYZE`, pessimistic RR, RC/stale/weak reads, MaxTS leakage, BatchCop,
  partitioning, unsupported readers, and remote clients without cop-read
  evidence all fail closed;
- attempt, fragment, publication, and event lifecycle conservation with no
  active ownership or fatal error.

Add a native full-mode test modeled after
`TestOnlineCaptureSerializedNativeEndToEnd`. It starts the native adapter while
`usertable` is empty, loads rows through captured inserts, executes a small
read/update/insert/scan sequence, drains and closes, and requires authoritative
`outcome=pass`, zero violations/errors, exact accepted/ordered/checked
conservation, and zero active state. Gate the test with
`TIDB_ONLINE_CHECKER_TEST_LIBRARY` so absence of a library is explicit rather
than a false pass.

Run:

    cd /home/dyf/code/db-consistency/tidb-online
    make bazel_prepare
    go test -tags=intest ./pkg/session/test/txn \
      -run '^TestOnlineCaptureYCSB' -count=1
    TIDB_ONLINE_CHECKER_TEST_LIBRARY=/home/dyf/code/db-consistency/co-checker/online/build-release/libco_checker_online_c.so \
      go test -tags=intest ./pkg/session/test/txn \
      -run '^TestOnlineCaptureYCSBNativeEndToEnd$' -count=1
    go test -race ./pkg/store/driver/txn
    ./tools/check/failpoint-go-test.sh pkg/store/copr -race -count=1

Then run all prior affected package and Bazel tests required by TiDB policy.
Acceptance is exact evidence for every supported SQL shape, continued negative
coverage, a native full pass, and no regression in TPC-C.

### Milestone 5: Implement the immutable four-mode E2E runner

Create `online/scripts/run_ycsb_global_lock_comparison.sh`. Port the proven
artifact capture, source-state hashing, detached pristine worktree, process
supervision, telemetry, drain, result validation, append-only attempts, and
resume behavior from the TPC-C runner. Do not source or rewrite the TPC-C shell
script. Create `online/tools/analyze_ycsb_global_lock.py`,
`online/tests/test_analyze_ycsb_global_lock.py`, and register the analyzer test
in `online/CMakeLists.txt`.

The required headline modes are, in conceptual stage order:

1. `baseline-original`, built from the same pristine TiDB commit used by the
   TPC-C comparison and with no checker environment.
2. `serialized-capture-only`.
3. `serialized-order-only`.
4. `serialized-full`.

An optional `instrumented-disabled` diagnostic may isolate compiled-but-off
overhead, but it is not a headline mode and cannot replace the pristine paired
baseline. Pristine versus full is total tool overhead. Capture-only includes
the normal-TSO cost that checked YCSB requires; do not remove that cost from the
headline.

The runner accepts explicit absolute paths for checker root, TiDB root,
go-ycsb source, result root, TiDB CPU set, and client CPU set. It supports
`--smoke`, `--trials`, `--workloads`, `--calibration`, and `--resume`.
Smoke-only overrides include `--record-count`, `--threads`,
`--warmup-operations`, and `--operations`. A normal measured run requires a
versioned calibration JSON and all A-F profiles with at least five trials unless
an explicit diagnostic-only flag is recorded. Arbitrary missing or reordered
headline mode sets fail configuration validation.

The runner creates immutable Release artifacts once and records checker and
TiDB commit/diff/untracked-source manifests, pristine commit, compiler/Go/host
information, go-ycsb base commit and patch, property hashes, commands, and
SHA-256 for every binary/script/helper. Existing development binaries are not
reused because they may predate sources. No source worktree is modified during
artifact creation.

Every workload/mode/trial attempt follows this phase machine:

1. Create an append-only attempt directory and a new empty UniStore directory.
2. Start a checker-off schema server, run `ycsb-schema`, prove the table empty,
   record schema digest, and stop it gracefully.
3. Start pristine TiDB for baseline or instrumented TiDB with a fresh checker
   configuration for a checked mode. Use a unique result path and cluster epoch.
4. Run patched go-ycsb load with `mysql.create_table=false`,
   `load.analyze=false`, `strict=true`, `mysql.strict_rows=true`, one thread,
   `batch.size=1`, and the fixed load seed. Require successful operations and
   affected inserts to equal `recordcount`, with zero failures.
5. Run paginated `ycsb-digest`, compare exact schema/data/combined digest and
   count with the immutable pristine load reference, verify every page stayed
   within the configured bound, then drain the checked pipeline.
6. Run `ycsb-gate` and drain again. The gate is outside timed measurement.
7. Stabilize the server, then run a separate fixed-operation warm-up with its
   own seed and strict result. Drain again. For D/E, read successful inserted
   count from the structured result, verify row-count conservation, and pass
   the resulting current record count into the measured process so its insert
   counter does not reuse warm-up keys. Do not require cross-mode content digest
   equality after concurrent updates; require exact initial-load digest and
   row-count/invariant conservation.
8. Start per-second checker telemetry, `pidstat`, process/resource snapshots,
   and monotonic timing. Run the fixed-operation measured command with a
   distinct matched seed and `warmuptime=0`.
9. At client exit, record an immediate metrics cutoff. Validate
   `requested=successful`, `failed=0`, actual mix counts, monotonic timestamps,
   and output completeness.
10. Drain until accepted publications/events reach the mode-specific terminal
    counters and attempts, finalizers, fragments, queue, and reorder ownership
    are zero. Full mode also requires an available live pass verdict.
11. Stop telemetry, SIGTERM TiDB, wait for process exit, and validate the atomic
    authoritative result. Forced kill, timeout, missing result, or nonzero close
    status invalidates the attempt.
12. Restart the same store with all checker variables cleared. Record final
    row count, schema, and digest; require row count equal initial rows plus
    successful warm-up/measured inserts minus successful deletes. Checksum or
    diagnostic scans occur only with capture off.
13. Validate all logs, write phase/result validation JSON, and create the
    completed marker only after every required file and invariant passes.

Checked runtime capacities are explicit inputs. Before a measured suite, use a
small full-mode pilot to measure retained/capture bytes per loaded row and per
successful update/insert. Project the frozen operation count, add at least 30%
headroom, and reject the experiment before starting if requested capacities plus
server/client headroom do not fit host RAM or disk. Never retry a failed trial
by silently increasing capacity under the same experiment manifest.

For each workload and trial, use identical load/warm-up/run seeds and operation
counts across modes. Rotate the four mode order deterministically by trial to
balance host drift, while pairing results by workload and trial. Each mode still
gets a fresh store and checker. Never clone a running store. Do not delete or
replace failed attempt directories; `--resume` creates a new attempt and skips
only the newest analyzer-proven valid completed mode under a byte-identical
experiment/artifact manifest.

### Milestone 6: Implement validation and reporting

The analyzer reads only the workload/trial/mode matrix declared in
`experiment.json`; it must not discover and adopt arbitrary directories. It
selects the newest valid completed attempt and emits explicit invalid rows for
missing or failed attempts. Writes use temporary files plus atomic rename.

For one attempt, let `N` be successful top-level YCSB operations, `T` the
monotonic measured-client interval, and `D` the interval from client exit until
the checked mode reaches its exact terminal drain target. Define:

    fixed_ops_per_second = N / T
    completion_ops_per_second = N / (T + D)
    paired_overhead = 1 - checked_rate / pristine_rate_for_same_workload_trial

Call `completion_ops_per_second` end-to-verdict only for full mode. For
capture-only and order-only, call it end-to-completion. An earlier prefix pass
does not end `D`. Graceful server shutdown and post-check occur after the live
drain boundary and are reported separately, not included in `D`.

The analyzer produces:

- `ycsb-trials.csv`, one detailed row per workload/trial/mode with validity,
  rates, latency, resource, checker, capacity, and drain fields;
- `ycsb-performance.csv`, a compact paper-facing workload/mode summary with
  valid counts, median fixed/completion rates, and median paired overhead;
- `ycsb-summary.csv` and `ycsb-summary.json`, complete aggregate statistics;
- `ycsb-report.md`, a human-readable report with configuration, invalid trials,
  workload-specific results, and sustainability diagnostics.

Report medians plus minimum/maximum and median absolute deviation for five
trials. Compute overhead per paired trial first, then summarize those paired
values. Never treat one-second telemetry samples as independent replicates and
never remove outliers automatically. A bootstrap interval may be supplementary
with ten or more trials but cannot replace raw paired values.

Required client fields are top-level requested/success/failed operations,
actual operation mix, DB calls, and p50/p95/p99/p99.9 by READ, UPDATE, INSERT,
SCAN, and RMW. Report Workload F's DB-transaction expansion explicitly. Required
process fields include TiDB/client CPU, peak RSS/HWM, I/O, and context switches.
Required checker phase deltas include accepted/terminal publications and events,
service rates, maximum transaction/event backlog, queue/capture/retained peaks,
backpressure, global-lock wait/hold, finalization wait, and drain residual/time
when the runtime exposes them.

Telemetry acceptance requires monotonic counters, at least 95% expected
one-second sample coverage, no gap over three seconds, and a cutoff observation
within one second of client exit. A tail over 1%, drain over 30 seconds, or
sustained backlog growth is a visible sustainability warning, not grounds to
erase a correctly drained trial; the complete tail remains charged to the
completion denominator.

Add analyzer fixtures for valid and invalid attempts, missing modes, duplicate
append-only attempts, mismatched manifests, F operation expansion, D/E row
growth, counter imbalance, failed verdict, telemetry gaps, paired overhead,
and atomic output replacement. Run:

    cd /home/dyf/code/db-consistency/co-checker
    python3 -m unittest online/tests/test_analyze_ycsb_global_lock.py
    bash -n online/scripts/run_ycsb_global_lock_comparison.sh
    ctest --test-dir online/build-release --output-on-failure

Acceptance is deterministic analyzer output from fixtures and fail-closed
runner validation for every malformed case.

### Milestone 7: Run functional smoke, calibrate, and measure

The measured command automatically runs a fresh A-F four-mode smoke first. The
smoke uses the exact built artifacts and profiles but small fixed counts, for
example 10,000 initial records, 2,000 warm-up operations, 10,000 measured
operations, four client threads, and one trial. These are functional values,
not performance evidence. Every mode must load exactly, every workload must
finish with zero client errors, all checked lifecycles must drain, and every
full result must pass. No measured trial begins if any smoke attempt is invalid.

Next run disposable pristine calibration per workload to choose a fixed
operation count targeting approximately 600 seconds at the publishable thread
count and record count. Run a proportional full-mode capacity pilot to verify
retained-state projection. Freeze record count, fields, threads, warm-up
operations, measured operations, seed bases, capacities, CPU sets, and mode
rotation in `calibration.json` and copy its SHA-256 into `experiment.json`
before trial 1. The calibration schema is:

    {
      "format_version": 1,
      "profile_id": "co-checker-ycsb-core-v1",
      "record_count": 1000000,
      "field_count": 10,
      "field_length": 100,
      "threads": 16,
      "seed_policy": "fixed-v1",
      "seeds": {
        "load": 7732542672685676034,
        "warmup": 6300581044758642952,
        "measured": 1251957853004150076
      },
      "workloads": {
        "A": {"warmup_operations": 0, "measured_operations": 0},
        "B": {"warmup_operations": 0, "measured_operations": 0},
        "C": {"warmup_operations": 0, "measured_operations": 0},
        "D": {"warmup_operations": 0, "measured_operations": 0},
        "E": {"warmup_operations": 0, "measured_operations": 0},
        "F": {"warmup_operations": 0, "measured_operations": 0}
      }
    }

Calibration replaces every zero operation count with a measured positive value
and then makes the file immutable. The runner uses the three fixed phase seeds
above for every workload, mode, trial, and future run. The patched client
deterministically derives each thread's RNG from the fixed phase seed. Thus
paired modes and repeated trials receive the same logical client request
stream; differences between trials measure runtime and scheduling variance.
Tests lock this policy and its version.

The preferred measured profile is one million initial records, ten 100-byte
fields, 16 clients, no target throttle, 60 seconds' worth of fixed warm-up work,
and approximately 600 seconds' worth of fixed measured work. If host capacity
cannot sustain that profile, choose a smaller fixed profile that provides at
least 180 seconds of pristine measured runtime and record the capacity decision;
do not relabel it as the one-million-record profile.

The intended invocation shape is:

    cd /home/dyf/code/db-consistency/co-checker
    online/scripts/run_ycsb_global_lock_comparison.sh \
      --trials 5 \
      --workloads A,B,C,D,E,F \
      --calibration /absolute/path/to/frozen-calibration.json \
      --result-root /absolute/path/to/ycsb-online-results \
      --tidb /home/dyf/code/db-consistency/tidb-online \
      --go-ycsb-source /absolute/path/to/clean/go-ycsb-v1.0.3 \
      --tidb-cpus 0-7 \
      --client-cpus 8-15

The runner validates every calibration field, refuses zero/unknown workload
counts, hashes the file into the experiment identity, and rejects CLI overrides
that would change measured parameters. The positive operation counts themselves
are intentionally chosen only after calibration has measured the target host.

Run `perf` or pprof only in separate diagnostic attempts after headline trials.
If capture-only overhead is unexpectedly large, profile TSO wait, prepared
point capture, value copying, fragment allocation, publication, and queueing.
If full mode accumulates backlog, distinguish producer rate from checker service
rate before optimizing. Diagnostic profiles never replace unprofiled headline
trials.

## Validation and Acceptance

The feature is complete only when all of the following are observable:

1. All prior relevant native, TiDB, SQL-helper, race, and Bazel tests pass,
   including the formerly failing retry lifecycle test.
2. With checker runtime disabled, an optimistic autocommit point SELECT still
   uses the existing MaxTS fast path. With capture active, the same direct and
   prepared SELECT uses a nonzero normal TSO and publishes exact read evidence.
3. Pessimistic, non-RR, stale/weak, async-commit, 1PC, unsupported plan, and
   unsupported topology cases fail before incomplete evidence can pass.
4. The patched go-ycsb build is reproducible from the pinned clean commit;
   deterministic tests reproduce operation/key/value sequences; Analyze/table
   creation can be disabled; any client error yields conserved failure counts
   and nonzero exit.
5. Every mode starts with empty `usertable`, reaches the exact same captured
   initial-load digest, and has zero load errors. Checked modes include every
   initial user row write in checker history.
6. A-F smoke workloads pass separately under full mode. E records returned KV
   evidence on UniStore. F is reported as two DB transactions per logical RMW,
   not a multi-statement SI transaction.
7. Baseline emits no checker result or checker metrics. Capture-only has balanced
   capture/publication/recycling; order-only has ordered equal accepted; full has
   checked transactions/events equal accepted, zero violations, and final pass.
   Every checked mode has zero fatal/capture/publication error and zero active
   attempt/finalizer/fragment/queue/reorder ownership at close.
8. Post-check row conservation holds, all processes exit cleanly, and every
   authoritative checked result is finalized by graceful SIGTERM.
9. At least five valid paired trials exist for each reported workload. The
   report includes fixed and completion/end-to-verdict throughput, paired
   overhead, operation latency, CPU/RSS, capacity, queue/backlog, and drain
   evidence. Performance has no arbitrary pass threshold; correctness and
   evidence determine validity, and observed overhead is reported as measured.
10. Successful roots retain final summaries, experiment/build identity, and a
    retention record identifying pruned paths. Failed details remain only while
    needed for diagnosis. `--retain-details` is explicit opt-in, and manifests
    prevent experiment mixing before pruning.

## Concrete Full Validation Commands

After all source changes, run the focused commands in the milestones, then the
complete relevant suites. At minimum:

    cd /home/dyf/code/db-consistency/co-checker
    cmake -S online -B online/build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build online/build-release -j2
    ctest --test-dir online/build-release --output-on-failure

    cd /home/dyf/code/db-consistency/tidb-online
    make bazel_prepare
    go test ./pkg/sessiontxn/isolation ./pkg/session
    go test ./pkg/store/driver/txn ./pkg/distsql ./pkg/store/copr
    go test ./pkg/store/mockstore/unistore/tikv/dbreader
    go test ./pkg/store/mockstore/unistore/cophandler ./pkg/executor
    go test -race ./pkg/store/driver/txn
    ./tools/check/failpoint-go-test.sh pkg/store/copr -race -count=1
    go test -tags=intest ./pkg/session/test/txn
    make server TARGET=/absolute/path/to/tidb-server-instrumented

Run affected Bazel targets with the repository's normal flags after
`bazel_prepare`; record exact target names and output in this plan when
implementation reaches that milestone. Do not call validation complete based
only on targeted `go test` commands.

Finally run the automatic smoke through the new runner. Expected terminal
evidence includes zero invalid attempts, A-F full verdicts of `pass`, balanced
lifecycle counters, and an analyzer report with one valid smoke row for each
workload/mode. Only then run calibration and measured trials.

## Result Layout and Evidence Retention

During execution or explicit `--retain-details`, the result root has this
conceptual layout:

    RESULT_ROOT/
      artifacts/
      build-artifacts.json
      experiment.json
      calibration.json
      preparation-reference.json
      smoke/
      workloads/A/trials/trial-001/baseline-original/attempt-001/
      workloads/A/trials/trial-001/serialized-capture-only/attempt-001/
      ...
      ycsb-trials.csv
      ycsb-performance.csv
      ycsb-summary.csv
      ycsb-summary.json
      ycsb-report.md

Each live attempt writes its manifest/configuration, phase log, schema/empty/digest
reports, load/warm-up/workload raw output and structured results, gate report,
drain JSON and raw Prometheus snapshots, one-second telemetry, process/resource
samples, monotonic timing, checker result and validation, post-check digest,
server logs, exit statuses, and completed marker. An interrupted or invalid
attempt is never renamed to completed. With the default policy, a successful
attempt's multi-gigabyte store is deleted immediately after analyzer validation.
After all rows validate and atomic summaries are written, attempt/reference
trees and build snapshots are pruned and `retention.json` records the exact
paths. `--retain-details` keeps the full layout for debugging/profiling.

## Idempotence and Recovery

All build and experiment outputs go to new result/build-attempt directories.
Never overwrite a checker result path, clone a running UniStore directory, or
mutate the supplied go-ycsb checkout. Schema/load/workload commands are not
replayed against a partially used attempt. If a phase fails, gracefully stop
any live TiDB process, record failure and exit status, and retain the attempt
only through diagnosis. A detailed rerun uses a new store/root; diagnosed raw
data may then be explicitly pruned.

Artifact creation verifies source state before and after build. If either
development worktree changes concurrently, discard only the incomplete artifact
build directory and start another build attempt; never reset the worktree.
`--resume` requires identical experiment JSON, artifact hashes, workload hashes,
host/cpu configuration, and capacity settings. A capacity overflow, forced
kill, unavailable authoritative result, missing telemetry, client error, or
digest mismatch invalidates that attempt and cannot be repaired in place.

Temporary pristine TiDB worktrees use an exact validated path under `/tmp` or a
runner-controlled result directory. Add and remove only that explicit worktree
through noninteractive `git worktree` commands. Never clean either development
tree.

## Interfaces and Dependencies

At milestone completion, TiDB `*session` exposes:

    TxnCaptureRequiresRealStartTS() bool

`OptimisticTxnContextProvider.AdviseOptimizeWithPlan` uses that method only
after proving the plan is an autocommit PK/unique point get. The later session
mode validator remains the authoritative fail-closed backstop.

The patched go-ycsb binary accepts the new properties documented in Milestone 2
and writes one versioned result JSON per invocation. The runner treats that JSON
as authoritative for top-level operation conservation and treats histogram/raw
output as latency evidence, not success authority.

`tidb-sql` exposes `ycsb-schema`, `assert-ycsb-empty`, `ycsb-digest`, and
`ycsb-gate` with JSON outputs suitable for manifests and validators. The runner
owns orchestration and calls the analyzer after each complete mode matrix. The
analyzer is pure Python standard library unless an explicit dependency is added
and pinned in this plan.

The native checker dynamic library remains the C ABI defined by
`online/include/co_checker/online/c_api.h`. TiDB loads it through the existing
`TIDB_ONLINE_CHECKER_*` environment contract. The runner uses serialized global
lock ordering and exactly one required producer. It obtains live metrics through
TiDB's status-port Prometheus endpoint and obtains the authoritative final JSON
only after graceful close.

## Revision Note

2026-08-09: Initial plan created after source audits of the checker, current
instrumented TiDB worktree, TPC-C workflow, and pinned go-ycsb v1.0.3. The plan
fixes optimistic-SI semantics, returned-KV scan interpretation, deterministic
strict client behavior, A-F profiles, four-mode E2E validation, and paired
performance reporting.

2026-08-09: Milestone 0 execution corrected two invalid baseline assumptions:
failpoint tests require TiDB's rewrite wrapper, and CTest requires a current
Release rebuild. Source inspection also showed that checked automatic retry
must explicitly remain optimistic, and corrected the description of go-ycsb
histogram warm-up timing. The original fail-closed scope and fixed-operation
warm-up decisions are unchanged.

2026-08-09: Milestone 1 source validation found an additional prepared
point-get execution short path that invalidated real transactions before lazy
result completion. The plan now gates that path only for an active checked
runtime and records the stronger observation that unsupported explicit modes
fail at `BEGIN`, before any SELECT or attempt registration.

2026-08-09: Milestone 2 implemented the pinned client patch and froze a
reproducible build artifact. Strict cancellation accounts unattempted requested
operations as failures, while `attempted` remains the number actually issued;
therefore `requested = successful + failed` remains invariant without hiding
peer cancellation. Successful RMW is the only path that records the logical
RMW histogram, while READ/UPDATE DB-call measurements remain independently
visible.
