# Plan: UniStore Coprocessor Read Instrumentation for Full TPC-C

Status: implementation and required functional validation complete
Scope: `tidb-online` with the existing UniStore TPC-C benchmark

Checker model: point key/value observations under optimistic repeatable-read/SI,
with transaction start timestamps intentionally unavailable to the checker

This ExecPlan is a living document. Keep `Progress`, `Surprises & Discoveries`,
`Decision Log`, and `Outcomes & Retrospective` up to date as implementation and
validation proceed. It is maintained according to
`/home/dyf/code/db-consistency/tidb-online/PLANS.md`.

## Progress

- [x] (2026-08-04) Read the complete plan and all applicable repository
  instructions; audited both worktrees, existing diffs, and the implementation
  delta from pristine TiDB commit `b94006d3abffd428634ecc5c499f4bcb7b4d06d1`.
- [x] (2026-08-04) Completed the required independent read-only
  `gpt-5.6-sol`/`xhigh` architecture review and independently reproduced its
  production-executor, lifecycle, and benchmark-initialization findings.
- [x] (2026-08-04) Implemented fragment ownership, recorder batching, atomic
  publication/acceptance, benign complete late discard, metrics, and focused
  race coverage.
- [x] (2026-08-04) Propagated per-attempt observers through session and DistSQL,
  with one staged fragment per physical coprocessor request/page and acceptance
  only after the selected high-level response passes all gates.
- [x] (2026-08-04) Implemented and capability-gated production UniStore
  consumption capture for forward/reverse scans, exact point ranges,
  present-empty values, SkipValue materialization, and extra readers.
- [x] (2026-08-04) Enabled only verified reader/operator plans and retained
  fail-closed behavior for unsupported stores, processors, isolation modes,
  dynamic foreign-key cascades, and execution paths.
- [x] (2026-08-04) Implemented deterministic full-mix TPC-C data preparation,
  ordered manifests, exact schema/data digests, plan preflight, four-mode
  runner/analyzer metrics, and focused harness tests.
- [x] (2026-08-04) Passed focused, race, native-checker, SQL-helper,
  shell/analyzer, microbenchmark, and affected failpoint-aware Bazel validation.
- [x] (2026-08-04) Completed the sequential four-mode full-mix smoke from
  fresh stores, independently audited the raw artifacts, and completed the
  final worktree/failpoint audit.

## Surprises & Discoveries

- Observation: ordinary production UniStore DAG requests no longer use the
  closure executor assumed by the Phase 4 examples. `handleCopDAGRequest` calls
  `buildAndRunMPPExecutor`; `buildClosureExecutor` is referenced only by tests.
  The production table scan can run asynchronously and the production index
  scan eagerly buffers chunks, so the actual `DBReader.ScanProcessor` boundary
  must be verified directly and limit/read-ahead cases must be tested against
  the category 1+2 contract.
  Evidence: `pkg/store/mockstore/unistore/cophandler/cop_handler.go`,
  `mpp_exec.go`, and repository-wide references to `buildClosureExecutor`.
- Observation: `DBReader.Scan` and `ReverseScan` currently use Badger
  `Item.IsEmpty()`, which tests payload length rather than the explicit delete
  metadata exposed by `Item.IsDeleted()`. This must change for present empty
  values to survive capture and execution.
  Evidence: `pkg/store/mockstore/unistore/tikv/dbreader/db_reader.go` and the
  pinned Badger `iterator.go` implementation.
- Observation: the existing four-mode runner clones a pristine-created,
  stopped empty-schema snapshot and documents that go-tpc has no seed option.
  It therefore cannot currently prove the required ordered preparation hash or
  exact post-prepare digest from a fresh checked store.
  Evidence: `online/scripts/run_tpcc_global_lock_comparison.sh` snapshot and
  experiment-manifest sections.
- Observation: `execStmtResult.Finish` passes the still-nil named return value
  to `finishStmt` instead of the underlying record-set finish error. That can
  publish a read-only attempt after coprocessor cleanup failed.
  Evidence: `pkg/session/session.go:3126`.
- Observation: `KeyRange.IsPoint()` is only a syntactic start/end test. A
  partial equality prefix of a clustered common handle also satisfies it, so
  treating every such range as an exact row creates false `Absent` evidence.
  Exact common-handle point capture must additionally require the decoded row
  key column count to equal `len(PrimaryColumnIds)`; partial handles remain
  ordinary forward/reverse scans.
  Evidence: the OrderStatus latest-order query in the first checked smoke and
  the focused common-handle prefix/point-miss regressions.
- Observation: go-tpc's `--check-all` enables initialization-only condition 11,
  which a valid Delivery workload necessarily changes. Post-workload TPC-C
  validation must use the standard conditions 1-10 and 12.
  Evidence: the first full-mix baseline smoke and the go-tpc check command's
  normal condition set.
- Observation: with more than one go-tpc worker, ordinary optimistic Error 9007
  write conflicts occur even in the short pristine smoke. With
  `--ignore-error=false`, each affected worker exits on its first conflict, so
  a nominal 600-second run no longer sustains its configured concurrency.
  Historical 8-warehouse/16-worker artifacts contain roughly 127,000--149,000
  NewOrder conflicts per 600-second attempt, so printing every conflict would
  itself materially distort the comparison. The full-mix runner must sustain
  workers with silent structured `*_ERR` accounting, report error rates, and
  continue to fail closed on checker/runtime metrics, logs, lifecycle, and post-check.

## Decision Log

- Decision: keep the C++ checker semantics, packed ABI, MPSC queue, and
  publication protocol unchanged. Resolve all discovered gaps in the Go
  producer, UniStore execution, and benchmark harness layers.
  Rationale: the existing driver capture and native adapter already implement
  the required point-observation and commit-order semantics; no contrary code
  evidence was found.
  Date/Author: 2026-08-04 / Codex.
- Decision: instrument and capability-check the processor actually passed to
  production `DBReader` calls; do not assume the test-only closure executor is
  the production path. Any production operator shape without a proven
  category 1+2 boundary remains fail-closed.
  Rationale: otherwise checked SQL could execute with no evidence or could
  silently include pre-consumption read-ahead.
  Date/Author: 2026-08-04 / Codex.
- Decision: preserve disabled execution byte-for-byte where practical and put
  all fragment allocation, arena growth, metrics, cache disabling, and
  store-batching disabling behind a nil observer check.
  Rationale: capture-disabled overhead is an explicit acceptance constraint.
  Date/Author: 2026-08-04 / Codex.
- Decision: keep generic go-tpc `--ignore-error=false`, run warm-up and
  measured phases with `--continue-on-write-conflict=true` and
  `--silence=true`, and require typed MySQL Error 9007 plus exact aggregate
  `*_ERR` conservation. Treat structured successful counts as the throughput
  numerator and report structured conflict counts/rates separately.
  Preparation and post-check still run fail-fast. Checked modes still reject
  every capture/runtime fatal or error metric, fatal server marker, lifecycle
  imbalance, drain/verdict failure, and failed TPC-C post-check.
  Rationale: optimistic conflicts are expected workload outcomes; terminating
  workers changes configured concurrency, and logging hundreds of thousands of
  conflicts creates mode-dependent benchmark work.
  Date/Author: 2026-08-04 / Codex.

## Outcomes & Retrospective

The implementation and requested functional validation are complete without
changing the C++ checker semantics, packed ABI, MPSC queue, or publication
protocol. All pre-existing untracked checker documentation/trace artifacts and
all implementation worktree changes were preserved.

The final smoke result is
`/tmp/co-checker-full-tpcc-smoke-20260804-r3`. It ran
`baseline-original`, `serialized-capture-only`, `serialized-order-only`, and
`serialized-full` sequentially. Every mode began with all nine TPC-C tables
empty, used one preparation worker and seed `20260804`, and reproduced ordered
manifest SHA-256
`137aa770189044e56a721f4a6f213945ec176ad4ff02d63cd461e2f510a3eaf7`
and exact database digest
`9a48ed1544e64025582c662af87ab33f17a8de0caf5190c793e302ff785bf916`.
The manifest contains 598 deterministic preparation transactions and 598,980
logical writes. All five transaction types executed with zero workload errors,
and standard TPC-C post-check conditions 1-10 and 12 passed in every mode.

All three instrumented modes had zero active attempts, finalizers, fragments,
fragment-owned bytes, queue ownership, and reorder ownership at close, with no
fatal or publication error. Capture-only accepted all 4,093 started fragments
and 4,791 publications; order-only accepted and ordered 4,803 publications;
full accepted, ordered, and checked 4,796 publications and checked all
2,376,267 accepted events. Full mode's final verdict was `pass`.

The canonical 12-target failpoint-aware Bazel suite passed with tags
`deadlock,intest`, including the 50-shard executor, session, and coprocessor
targets and the 21-shard transaction target. The failpoint refcount was returned
to zero before the smoke. DBReader microbenchmark
medians were 4,511 ns/op with capture disabled (0 B/op, 0 allocs), 8,472 ns/op
with capture enabled, and 8,585 ns/op for the SkipValue/COUNT path. The
single short smoke measured 8,703.1 baseline tpmC and 7,319.4 full-mode tpmC;
these smoke throughput values are functional evidence, not a statistically
meaningful multi-trial performance study.

## Goal

Support Payment, OrderStatus, Delivery, and StockLevel in checked mode by
capturing every MVCC-visible raw user KV actually consumed by their supported
coprocessor operators. NewOrder already uses supported point-read and write
paths.

The implementation must be correct before it is optimized. The disabled path
must remain effectively unchanged, and the enabled path should batch work so it
does not take a transaction-recorder lock or allocate an independent object for
every consumed KV.

No changes are planned for the C++ checker, packed event format, MPSC queue, or
transaction publication protocol.

## Fixed checker problem setting

The checker is not given a transaction start timestamp. It must decide whether
the observed point reads and writes admit a compatible snapshot under commit
order. This is the fixed checker semantics for this work, not an implementation
option to revisit as part of coprocessor instrumentation.

TiDB and UniStore still use their internal read timestamps to perform MVCC
visibility checks and return the value that the transaction actually observed.
Those timestamps are execution metadata only: they are not emitted as checker
evidence and the checker must not rely on them.

Theoretically you don't need to touch the source code of checker engine.

## Evidence contract

For each MVCC-visible raw user KV actually consumed by a supported operator in a
checked statement, record an ordinary transaction read event containing:

- the canonical TiDB user key;
- `Present` or `Absent`;
- the exact visible raw value when present;
- `Snapshot` or `Local` source;
- a transaction-global, strictly increasing event sequence.

The following rules define the supported semantics:

- A KV is consumed once the operator crosses any pre-consumption limit/break
  guard and decodes, evaluates, or otherwise uses that KV.
- Record both category 1 inputs that contribute to a result and category 2
  inputs that are evaluated but rejected by selection or used by aggregation,
  TopN, or join processing.
- Do not record category 3 KVs that an iterator sought, prefetched, or presented
  to a processor which returned an early limit/break before evaluating the
  current KV.
- Record both index and table KVs for a non-covering index lookup.
- Record both outer and inner reads produced by a join, including repeated inner
  probes. Do not create join-specific events.
- Preserve repeated visits. Equal duplicates are valid; conflicting values for
  one snapshot remain checker evidence.
- An explicitly consumed exact-point lookup miss records `Absent` for the
  requested key. Generic coprocessor `BatchGet` remains unsupported until it has
  the per-key consumption boundary defined below.
- An arbitrary empty scan records no events.
- Do not infer range bounds, scan exhaustion, cardinality, predicate meaning, or
  phantom information.
- Never silently truncate evidence. Allocation, capacity, normalization, or
  ordinary callback failure must fail the checked attempt. A complete fragment
  discarded because its transaction is already published is the explicit benign
  exception and must be counted separately.

Although the high-level requirement is to record which keys were visited, the
current SI checker also needs the exact value/state to identify a compatible
snapshot. A key-only event would require a different and weaker checker.
Consumption here means consumption by the supported UniStore processor, not
delivery of the final SQL result to the client.

## Architecture

```text
active transaction attempt
  -> DistSQL request
  -> one capture fragment per coprocessor RPC/page
  -> UniStore obtains MVCC-visible candidate key/value pairs
  -> a supported operator consumes a candidate
  -> the fragment records the consumed raw key/value pair
  -> successful response accepts the fragment
  -> failed/retried response discards the fragment
  -> accepted fragments become transaction capture events
  -> completed transaction follows the existing MPSC/checker path
```

The storage/operator path must not append directly to the shared transaction
recorder. Each RPC uses a fragment-local buffer. A successful response transfers
the whole buffer to the recorder under one lock; an unsuccessful response
discards it.

## Correctness invariants

1. A capture object is permanently bound to exactly one physical transaction
   attempt. A storage read timestamp must not become checker evidence or part of
   the checker decision.
2. A fragment is permanently bound to one physical coprocessor RPC/page.
3. A successful fragment must prove that an instrumented storage path attached
   to it. This distinguishes a valid empty scan from an instrumentation bypass.
4. Only MVCC-visible user KVs consumed by a supported operator enter the
   fragment. Locks, MVCC timestamp suffixes, iterator-only/prefetched KVs, raft
   metadata, and synthetic SQL rows must never enter the checker namespace.
5. A fragment is either accepted once or discarded; both operations are
   idempotent and mutually exclusive.
6. Region, lock, transport, cancellation, and `OtherError` responses do not
   contribute accepted events.
7. Fragment acceptance and transaction publication are ordered under the
   recorder state lock. If publication has already completed, `Accept` discards
   the entire late fragment, increments explicit late-discard counters, and
   returns a benign result. It must not append evidence, mutate the published
   transaction, or poison the checker verdict.
8. The normal lifecycle still waits for all coprocessor workers before read-only
   completion or transaction publication. The late-discard rule is a defensive
   race outcome, not a substitute for this barrier.
9. Callback failure becomes a SQL/capture error and poisons the checked run;
   publication-complete late discard is the explicit non-error exception.
10. Unsupported stores and execution paths remain fail-closed.
11. The capture-disabled path performs no capture allocation or locking.
12. For every capture-backed processor, `Process == nil` means the current KV
    was consumed, while `ErrScanBreak` means it was rejected before consumption.
    Any other error fails the request and causes its fragment to be discarded.

## Phase 1: Capture fragment API and recorder batching

Add a small, storage-neutral API in a new
`/home/dyf/code/db-consistency/tidb-online/pkg/kv/cop_read_capture.go`:

```go
type CopReadCapture interface {
    BeginFragment() CopReadCaptureFragment
}

type CopReadCaptureFragment interface {
    MarkStorageAttached() error
    ObservePresent(key, value []byte) error
    ObserveAbsent(key []byte) error
    Accept() error
    Discard()
}
```

Do not extend `kv.Transaction`; that would affect unrelated transaction
implementations and mocks.

Implement the interfaces in
`/home/dyf/code/db-consistency/tidb-online/pkg/store/driver/txn/capture.go`:

- expose an accessor that returns the observer attached to a concrete captured
  transaction;
- store fragment descriptors plus key/value bytes in a fragment-local arena;
- clone ephemeral DBReader bytes only after the operator consumes them;
- transfer ownership on `Accept` instead of cloning every key/value again;
- append all fragment events under one recorder lock;
- assign transaction event sequences in fragment order during acceptance;
- reject overflow and stale-attempt attachment;
- serialize `Accept` with publication so it either transfers the complete
  fragment before publication or drops the complete fragment after publication;
- treat the latter as a successful late discard, release its storage, and count
  late-discarded fragments, events, and bytes;
- account for retained fragment bytes and release them at publish or abort.

Acceptance tests:

- present, present-empty, and absent values remain distinct;
- accepted fragments appear in the batch and discarded fragments do not;
- concurrent fragment acceptance produces unique increasing sequences;
- input byte mutation after observation cannot change captured evidence;
- double accept/discard is safe and cannot duplicate evidence;
- an old observer after transaction retry cannot append to either attempt;
- an `Accept` racing publication is either wholly visible before publication or
  wholly late-discarded afterward, never partially visible;
- publication-complete late discard returns no capture error and cannot change
  an already published checker result;
- size/count overflow fails closed.

## Phase 2: Session and DistSQL propagation

Add `CopReadCapture` to:

- `pkg/distsql/context/context.go:DistSQLContext`;
- `pkg/kv/kv.go:ClientSendOption`.

In `pkg/session/session.go:GetDistSQLCtx`:

- obtain the observer from the active captured transaction;
- refresh the field on every call because the cached DistSQL context may have
  been created during optimization before lazy transaction activation;
- refresh an already-created context when an attempt activates or retries;
- capture the transaction/recorder in the observer rather than allowing worker
  goroutines to call mutable session methods;
- clear the observer only after the statement workers have stopped.

In `pkg/distsql/distsql.go:Select`, copy the observer into
`ClientSendOption`.

Acceptance tests:

- two concurrent sessions never share observers;
- a transaction retry installs a new observer;
- a detached or stale observer cannot append to a completed attempt; a fragment
  discovered after publication is dropped and counted without failing the run;
- capture-disabled and internal SQL requests carry no observer.

## Phase 3: One fragment per coprocessor RPC

In `pkg/store/copr/coprocessor.go:handleTaskOnce`:

1. Begin one fragment immediately before a physical RPC send.
2. Put only that fragment into the RPC context.
3. Send the request.
4. Validate transport, region, lock, and `OtherError` results.
5. After the response is otherwise accepted, call `Accept` on the fragment.
6. If the recorder is still unpublished, transfer the complete fragment exactly
   once. If publication already won the race, discard and count the complete
   fragment without failing the response. Discard on every rejected/error
   response path.

Ordinary paging remains enabled and naturally creates one terminal fragment per
successful page: normally accepted before publication, or late-discarded if
publication wins the defensive race. Lite workers already use `handleTaskOnce`
and should not need a separate implementation.

For the first correct version:

- disable coprocessor result caching whenever capture is active;
- disable store-batched cop requests whenever capture is active;
- reject TiFlash, streaming, BatchCop, and external TiKV capture;
- keep ordinary TiKV DAG requests over the in-process UniStore RPC path only.

Cache hits cannot be allowed until cached results also retain and replay their
raw read evidence. Store batching cannot be allowed until each child `TaskId`
has an independent fragment because child responses can succeed and retry
independently.

Fix the lifecycle error in `pkg/session/session.go:execStmtResult.Finish` before
enabling readers. The underlying record-set finish error must be passed into
`finishStmt` before read-only transaction completion. `copIterator.Close`
already cancels RPCs and waits for workers; tests must preserve that barrier.

Acceptance tests:

- region and lock retries discard rejected fragments;
- `OtherError`, transport error, panic, and cancellation discard fragments;
- paging accepts every successful page once;
- publication winning a race with fragment acceptance appends no event,
  increments late-fragment/event/byte counters, and does not fail the run;
- a late-discarded fragment is terminal and cannot later be accepted;
- early result close waits for worker cleanup;
- callback failure reaches SQL finalization and aborts capture;
- capture forces cache/store batching off while the disabled path retains its
  existing behavior;
- targeted `go test -race` reports no observer or backoffer-context races.

## Phase 4: UniStore operator-consumption instrumentation

Extract the fragment in `pkg/store/mockstore/unistore/tikv/server.go` and attach
it to every request-scoped DBReader:

- the primary reader created by `requestCtx.getDBReader`;
- every extra reader created by `requestCtx.GetExtraDBReaderByRegion` for
  index-lookup table reads.

Do not append a KV merely because `DBReader` or its iterator fetched it. The
capture boundary is successful operator consumption.

For `DBReader.Scan` and `DBReader.ReverseScan`:

1. Complete MVCC visibility and read-timestamp validation.
2. Skip deletion/tombstone MVCC versions according to their metadata. Never use
   `len(value) == 0` to infer absence; a present zero-length value remains
   `Present`.
3. Pass the same key and processor value used today to
   `ScanProcessor.Process`.
4. If `Process` returns `ErrScanBreak`, stop without recording the current KV.
5. If `Process` returns another error, fail the response; the RPC fragment is
   discarded.
6. If `Process` returns `nil`, obtain the exact raw value, normalize the key into
   the same canonical domain as transaction `Get`/`Set`/`Delete` capture, and
   append the key/value to the fragment.

This order records a row rejected by a pushed selection because selection has
already evaluated it and returns `nil`. It excludes the look-ahead row presented
after a Limit has already been satisfied because the supported processor returns
`ErrScanBreak` before decoding or evaluating that row.

When `ScanProcessor.SkipValue()` is true, preserve the existing processor input
and pass `nil`. Materialize the exact raw value only after `Process` confirms
consumption, then copy it into the fragment. Thus `COUNT(*)` inputs retain exact
checker evidence without loading the value for an unconsumed early-break KV.
This extra load is instrumentation overhead and must be measured.

The coprocessor point-range path in
`pkg/store/mockstore/unistore/cophandler/closure_exec.go` calls `DBReader.Get`
before `processor.Process`. Do not instrument `DBReader.Get` with an
unconditional append. For a present result, append only after `Process` returns
`nil`; exclude it on pre-consumption `ErrScanBreak`. For a successfully issued
explicit point lookup that is absent, record `Absent` because the operator
consumed the absence. Preserve an explicit presence bit so a present empty value
cannot be confused with an absent key.

Do not add unconditional fragment capture to `DBReader.BatchGet`. Ordinary
transaction `Get`/`BatchGet` operations retain their existing transaction-driver
instrumentation. If a future coprocessor path uses `BatchGet`, it must provide an
explicit per-key consumption/result boundary before the capability gate permits
it.

Only the first physical processor pass records evidence. Internal replay during
`TopN.Finish` must not duplicate the original KV read.

API-v2/keyspace decoding must use the normal TiDB user-key representation. A
normalization failure is a response/capture error, never a reason to drop the
event.

Acceptance tests use deterministic table/index fixtures and independently
constructed raw keys to cover:

- forward and reverse table scans;
- covering index scans;
- non-covering index lookup, including extra readers;
- pushed selection that returns zero rows while consuming several KVs; all of
  those category 2 KVs are captured;
- pushed `COUNT`, `SUM`, and `COUNT(DISTINCT)` inputs, including forced values
  for consumed `SkipValue` inputs;
- TopN inputs that lose heap admission, plus Limit/early-break look-ahead KVs
  that are not captured;
- repeated inner probes from an index nested-loop join;
- explicit missing point probes and arbitrary empty ranges;
- present-empty values and tombstones;
- API-v2 key normalization;
- UnionScan snapshot reads plus transaction-local overlays;
- processor error after earlier consumed inputs discards the whole RPC fragment;
- forward and reverse assertions prove that captured evidence is exactly
  category 1 plus category 2, with category 3 excluded.

## Phase 5: Capability-aware plan gate

Do not remove the existing reader rejection globally. Introduce an optional
capability marker and advertise it only through the instrumented UniStore RPC
client. The coprocessor client should delegate that capability from its
underlying RPC implementation.

Add a storage-side consumption-contract marker for the capture-backed table
scan, index scan, selection, TopN, hash aggregation, and count processors. When
capture is active, an unmarked processor fails closed instead of assuming that
its `ErrScanBreak` has pre-consumption semantics.

Define the marker in the DBReader package, for example:

```go
type CaptureConsumptionProcessor interface {
    ScanProcessor
    CopReadCaptureConsumptionContract()
}
```

Before a capture-active handler calls `Scan`, `ReverseScan`, or the
coprocessor point-range `Process` path, it must assert that the actual outer
processor implements this interface. Wrappers and composed processors must
declare the contract themselves; an inner marked processor cannot implicitly
authorize an unmarked wrapper.

Change `pkg/executor/compiler.go` so checked mode initially permits only
capture-backed:

- `PhysicalTableReader`;
- `PhysicalIndexReader`;
- `PhysicalIndexLookUpReader`;
- their scans nested under `UPDATE` and `DELETE`.

No special `IndexJoin` event or checker path is required: the join is supported
when every child reader is capture-backed.

Keep these paths rejected initially:

- TiFlash and external TiKV;
- streaming and BatchCop;
- IndexMerge until it has dedicated coverage;
- analyze, checksum, internal, detached, and background readers;
- stale reads, RC reads, and statements that do not use the transaction's normal
  optimistic repeatable-read snapshot; this restriction does not expose the
  internal snapshot timestamp to the checker;
- bulk, batch, and pipelined DML.

Acceptance tests prove that verified UniStore TPC-C plans pass and an
uninstrumented or unsupported reader still fails before execution.

## Phase 6: Full TPC-C integration

Add deterministic end-to-end tests for every transaction type:

- Payment: customer-by-last-name index scan and selected customer row;
- OrderStatus: customer count/range, lookup, latest order, and order-line scan;
- Delivery: empty/nonempty oldest-order scans, tuple/range DML, and order-line
  sum;
- StockLevel: order-line outer scan and repeated stock probes;
- NewOrder: ensure its existing point-read/write behavior is unchanged.

Compare the published raw read multiset with independently computed expected
keys. Include result-producing inputs, filtered-out/aggregate/TopN inputs, and
duplicates. Exclude iterator-only, prefetched, and pre-consumption early-break
look-ahead KVs. Do not assert predicate or range completeness.

Every integration test, smoke run, and measured trial starts from an empty
schema/store; every checked mode also starts with a fresh checker state. Use one
preparation worker, fixed parameters and seed, and a fixed insertion/transaction
order so every mode executes the same deterministic preparation sequence. In
checked modes, enable capture before preparation so the checker observes every
preparation write; wait for preparation publication and checker queues to drain
before starting the measured workload. Emit and compare an ordered manifest hash
of the logical preparation transactions/writes, and validate an exact
post-prepare database digest before accepting the run. Table counts alone are
not sufficient. Do not clone a preloaded store or seed hidden initial state into
the checker.

Update `online/tools/tidb_sql.go` to preflight all prepared-statement plan
variants, including the ID and last-name branches of Payment and OrderStatus.

Update `online/scripts/run_tpcc_global_lock_comparison.sh` and its analyzer to:

- parameterize transaction weights;
- run the standard `45,43,4,4,4` mix;
- parse success/error/latency summaries for all five transaction types;
- run the four modes sequentially, never concurrently;
- use identical preparation parameters, workload parameters, CPU affinity,
  compiler/build settings, storage settings, and disabled cache/store-batching
  optimizations in every mode;
- record the exact cache, store-batching, partition, foreign-key, isolation, and
  commit-protocol settings;
- reject a run if any capture/runtime error is hidden by go-tpc
  `--ignore-error`;
- retain queue, backlog, drain, CPU, memory, and checker-result validation.

A mixed-workload smoke run is accepted only when all five transaction types
execute and the preparation contract is satisfied. Apply mode-specific runtime
requirements:

- `baseline-original`: no capture/order/check counters are required;
- `serialized-capture-only`: capture, fragment, and publication counters
  advance and balance; order/check counters are not required;
- `serialized-order-only`: capture and ordering counters advance and balance;
  checking counters and a checker verdict are not required;
- `serialized-full`: capture, ordering, and checking counters advance and
  balance, and the final verdict is `pass`.

For every instrumented mode, active attempts, fragments, queue ownership, and
reorder ownership drain to zero, and no real capture/runtime fatal error occurs.
Publication-complete late-discard counters remain visible in results but are not
capture errors by themselves.

## Performance design and validation

### Hot-path requirements

- Capture disabled: one predictable nil check at most, zero capture allocations,
  and no capture lock.
- Capture enabled: fragment-local descriptors and a contiguous/geometrically
  growing byte arena.
- Clone ephemeral DBReader key/value bytes once into the fragment.
- Clone only after successful operator consumption; never clone category 3
  iterator/prefetch/look-ahead KVs.
- Transfer fragment ownership to the recorder without a second payload copy.
- Acquire the recorder mutex once per accepted fragment, not once per KV.
- Do not deduplicate in the producer; checker-side repeated-read validation must
  still see conflicting duplicates.
- Charge retained bytes to a tracker/budget and fail closed rather than applying
  backpressure while holding a DBReader or recorder lock.

### Metrics

Expose at least:

- fragments started, storage-attached, accepted, and discarded;
- captured events and payload bytes;
- forced value materializations;
- maximum fragment and transaction sizes;
- fragment-buffer allocation/high-water bytes;
- bulk-accept count and duration;
- capture errors;
- candidate KVs, processor-consumed KVs, and pre-consumption early breaks;
- publication-complete late-discarded fragments, events, and bytes, reported
  separately from capture errors;
- queue backlog, queue bytes, checked events, and drain tail.

### Measurement sequence

1. Add DBReader microbenchmarks for capture off, capture on, and
   `SkipValue`/COUNT materialization; run with `-benchmem`.
2. Require the disabled path to remain allocation-free and investigate a median
   regression greater than roughly 1%.
3. Run targeted tests under Go's race detector.
4. Run a short full-mix functional smoke test.
5. Run at least five interleaved measured trials of:
   - `baseline-original`;
   - `serialized-capture-only`;
   - `serialized-order-only`;
   - `serialized-full`.
6. Use identical cache and store-batching settings in every mode. If capture
   requires disabling either optimization, disable it in the paired pristine
   baseline too; do not substitute an auxiliary baseline for the same-trial
   paired baseline.
7. Report throughput, per-transaction latency, CPU, RSS, captured events/bytes,
   queue backlog, and drain tail for every run and the median.
8. If capture-only overhead is unexpectedly high, collect CPU and allocation
   profiles before changing the design. Separate forced value loading, byte
   copying, fragment allocation, recorder acceptance, queue publication, and
   checking costs.

Do not assume the NewOrder-only overhead applies to mixed TPC-C. Delivery and
StockLevel are substantially more read-intensive.

## Planned validation commands

Run focused Go tests first:

```bash
cd /home/dyf/code/db-consistency/tidb-online
go test ./pkg/store/driver/txn ./pkg/distsql ./pkg/store/copr
go test ./pkg/store/mockstore/unistore/tikv/dbreader
go test ./pkg/store/mockstore/unistore/cophandler
go test ./pkg/executor ./pkg/session
go test -race ./pkg/store/driver/txn ./pkg/store/copr
go test -tags=intest ./pkg/session/test/txn
```

Then rerun the existing native checker suite even though no C++ behavior is
expected to change:

```bash
cd /home/dyf/code/db-consistency/co-checker
./online/build-release/co_checker_online_tests
```

Add new files to the corresponding Bazel source/test lists and run the relevant
Bazel targets if that build path is used.

## Benchmark initialization contract

Starting from an empty database and observing the same deterministic preparation
is mandatory, not an unresolved bootstrap alternative. Each mode constructs its
own logically identical prepared state from empty storage using one preparation
worker and a fixed seed and operation order. Checked modes start the checker
before preparation and include those writes in its observed history; the
pristine baseline executes the identical preparation without instrumentation.
Require both the same ordered logical transaction/write manifest hash and the
same exact post-prepare database digest. Preparation is outside the measured
workload interval, and the measured interval cannot begin until preparation
validation and required checker drain complete.

Measured checked work is not complete at client exit or at an early prefix
verdict. It completes only after every accepted publication and event has
reached the selected mode's terminal target and all fragment, queue, reorder,
attempt, and finalizer ownership is zero. End-to-verdict throughput uses the
direct monotonic elapsed time from measured client start to that completion
point. An exactly drained long tail is reported and fully charged to this
denominator; it is not rejected solely for exceeding a percentage or duration
diagnostic threshold.

## Out of scope

- Predicate or range semantics and phantom detection.
- Join-specific events or reconstructed join results.
- C++ checker, packed ABI, MPSC queue, or publication changes.
- Per-statement identifiers.
- TiFlash support.
- External TiKV support. That requires a request flag and a response evidence
  sidecar across the TiDB/TiKV protocol.

## Definition of done

The work is complete only when:

1. All focused and prior relevant tests pass, including race tests.
2. Payment, OrderStatus, Delivery, StockLevel, and NewOrder execute in checked
   mode against UniStore.
3. Deterministic integration tests confirm the exact raw KV evidence expected
   from every supported reader path: category 1 plus category 2, excluding
   category 3.
4. Unsupported paths still fail closed.
5. No real capture error can be converted into a complete `Pass`; a fragment
   atomically discarded after transaction publication is counted as the defined
   benign late outcome rather than a capture error.
6. The disabled path has no material regression.
7. Every test and benchmark mode starts empty and reaches the same validated
   state through deterministic preparation observed by checked modes.
8. The mixed TPC-C smoke drains all workers, capture state, and checker queues.
9. Interleaved benchmark trials report all four modes, including capture-only,
   order-only, and full-checker overhead separately, with CPU, memory, event,
   queue, and latency evidence.
10. No checker input or decision depends on knowing a transaction start
    timestamp.
