# co-checker online runtime

This directory is an independently buildable implementation of the online
checker. The frozen protocol and consistency semantics are defined by the
repository's authoritative online-engine plan.

`SnapshotSOWWEngine::pruneHistoryThrough(frontier)` optionally reclaims reverse
deltas through a caller-proven lower bound. It preserves current state and
relevant WW positions, rejects invalid frontiers, and rejects later transactions needing
older history. The engine never prunes automatically. The black-box monitor's
default-off `--history-gc on|off` option supplies the session frontier; see
`si-monitoring/monitor/doc/history_gc.md` in the sibling repository.

Pruning also releases the delta's value references. The last reference frees
the interned payload and recycles its ValueId without moving other live IDs.
Current snapshot references and values shared across keys/deltas remain valid.
Absent keys whose last writer is strictly before the frontier are also freed;
their KeyIds and snapshot/last-writer slots are reused without moving live IDs.
Deletion exactly at the frontier is deferred until it advances, preserving a
WW bound that may still admit a new session. Pruning visits candidates from the
discarded deltas, not the full key dictionary. The deferred list covers at most
one boundary transaction. Cumulative `reclaimed_key_count/bytes` reports this
work. Historical WW diagnostic bounds below the frontier may be omitted after
key reclamation; effective bounds and admitted transaction verdicts are unchanged.
Hash/vector capacity is retained for reuse, not proactively shrunk.
`metrics(true)` requests an explicit linear-time
memory census; ordinary `metrics()` does not scan the retained state.

Build and run its tests from the checker repository root:

```sh
cmake -S online -B online/build -G Ninja
cmake --build online/build
ctest --test-dir online/build --output-on-failure
```

Sanitizer builds use the CMake options
`CO_CHECKER_ONLINE_ENABLE_ASAN`, `CO_CHECKER_ONLINE_ENABLE_UBSAN`, and
`CO_CHECKER_ONLINE_ENABLE_TSAN`.

The versioned native ABI is described in `doc/c_api_v1.md`. Reproducible native
and live TiDB validation commands are in `doc/reproducibility.md` and
`doc/tpcc_workflow.md`; recorded conclusions are indexed by `results/README.md`.
