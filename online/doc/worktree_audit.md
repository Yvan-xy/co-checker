# Worktree and integration audit

Audit date: 2026-07-12 UTC.

## Repository state

Both repositories are on `online-checker`:

- `co-checker`: based on `3990430d1cc5b670b0511ebd3059c1da049f989c`.
- `tidb-online`: based on `b94006d3abffd428634ecc5c499f4bcb7b4d06d1`.

Before the branch was created, `co-checker` already contained a modified
`README.md` and untracked `doc/online_engine_plan.md` and
`doc/online_checker_architecture.svg`. Those files were preserved. The TiDB
worktree was clean. No repository was pushed, reset, or cleaned.

The request forbids offline-checker modifications, while one implementation
phase in the design document proposes changing the offline wrapper. The online
implementation therefore lives entirely below `online/`; the unchanged
offline executable is used only as an external oracle on their common semantic
subset. Frozen cases that the legacy checker does not implement are validated
against an independent reference model.

## Baseline validation

The following passed before online implementation changes:

```sh
cmake --build build -j2
ctest --test-dir build --output-on-failure

cd /home/dyf/code/db-consistency/tidb-online
go test ./pkg/store/driver/txn -count=1
```

The root checker had one CTest entry, `checker_tests`. The TiDB store-driver
package completed with no failures.

## Offline checker and trace corpus

The legacy SOWW implementation is in `src/checker/so_ww_checker.cpp`. It scans
commit-ordered history backward, stops at session-order and write-write lower
bounds, and applies final writes even after a failure. It is a valid differential
oracle for ordinary tagged committed traces, but not for first-session
read-only checking, unconditional repeated-read disagreement, malformed tags,
or online protocol errors.

The local corpus contains 16 processed TPC-C binary traces (about 7.9 GiB) and
their YAML/raw counterparts. The online tooling reads the binary files as a
stream and does not load a complete trace into memory.

## TiDB integration map

The read-only reference instrumentation is the ten-commit series
`e32ea24862..12cdb311ef` in `/home/dyf/code/db-consistency/tidb`. Its useful
hook locations port to the current TiDB tree, but its synchronous YAML sink is
not reused.

The supported driver seams are:

- transaction allocation and final write commit in
  `pkg/store/driver/txn/txn_driver.go`;
- snapshot reads in `pkg/store/driver/txn/snapshot.go`;
- local reads, mutations, staging, and savepoints in
  `pkg/store/driver/txn/unionstore_driver.go`;
- transaction activation in `pkg/sessiontxn/isolation/base.go`;
- read-only completion and retry in `pkg/session/session.go`.

A successful outer TiKV commit exposes the final `CommitTS()` for normal 2PC,
pessimistic, optimistic, async-commit, 1PC, and pipelined paths. The initial
checked configuration nevertheless keeps unvalidated fast paths disabled.
`ErrResultUndetermined` is fatal because the commit outcome is unknowable.

Real TiKV exposes `GetMinSafeTS(global)` as a candidate resolved frontier, but
its use still requires local delivery barriers and active read-only guards.
Unistore returns zero, so the first local TPC-C integration must use an explicit
quiescent closure instead of a delay-derived watermark.

## Tool availability

- GCC 13.3, Clang 18, CMake 3.28.3, Ninja 1.11.1.
- Go 1.25.10 and `go-tpc` are installed.
- `perf` is available with `perf_event_paranoid=1`.
- Go pprof endpoints are exposed by TiDB's status server.
- Bazel is not installed; generated `BUILD.bazel` changes require validation
  in an environment with Bazel.
- TSan binaries compile. The runtime intermittently aborts at startup in this
  container with `unexpected memory mapping`; direct retries completed cleanly,
  but aggregate CTest is not reliable on this host.
