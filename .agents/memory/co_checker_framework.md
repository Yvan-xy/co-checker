# Co-Checker Framework Notes

The C++ checker currently consumes preprocessed YAML only: committed documents,
sorted by ascending `commit_ts`, with dense per-file `session_id` values. Raw
TiDB traces must pass through `trace-gen/preprocess_trace.py` first because raw
documents can contain `status: "rolled_back"` and omit `commit_ts`.

Common checker infrastructure lives under `include/common` and
`namespace common`: trace model/parsing/indexing, diagnostics, logger,
and result helpers. `include/checker` is reserved for commit-order checker
contracts and concrete checker implementations.

Project decision: ignore TiDB event `source: snapshot|mem_buffer`; it should
not be part of the core checker semantics. The preprocessor removes
same-transaction local-buffer probe reads (`op: R`, `source: mem_buffer`) and
drops remaining `source` metadata before checker input is produced.

`TraceIndex::versions_by_key` stores one committed version per transaction/key:
the final write or delete for that key within the transaction. Intermediate
writes to the same key in the same transaction are intentionally ignored.

Current checker names registered by the CLI are `co-si-basic` and
`co-si-so-only`; the old `co-si` name is stale. Diagnostics are
message-oriented and no longer have `Diagnostic::code`.

Snapshot helpers now live in `include/common/trace/snapshot.h`.
`Snapshot` deliberately has no `operator[]`; use `get`, `has_key`, `put`,
`set`, and `erase` so missing/deleted keys remain explicit. `SnapshotOpt`
stores `Key -> optional<Value>` and is used when present-null/read-miss must be
distinguished from "not observed".

Formatting methods for `Event`, `Transaction`, `Snapshot`, and `SnapshotOpt`
are declared on the structs and implemented in `src/trace/trace.cpp`:
`to_string(indent)`, `print(os, indent)`, and zero-arg `print()` to
`std::cout`.

Tests are split by module:

- `test/test_trace.cpp`
- `test/test_snapshot.cpp`
- `test/test_checker.cpp`
- `test/test_index.cpp`
- `test/test_harness.{h,cpp}`
- `test/test_main.cpp` dispatches module runners.

`test/run_traces.py` runs `build/co-checker` over traces in `./traces`; it
supports `--trace` selectors, `--list`, `--fail-fast`, `--verbose`, and forwards
unknown args to `co-checker`.

Useful validation commands:

```bash
ctest --test-dir build --output-on-failure
python3 -m unittest trace-gen/test_preprocess_trace.py
build/co-checker --checker co-si-basic traces/2s-4t-10e-mutual.yaml
build/co-checker --checker co-si-so-only traces/2s-12t-67e-mixed.yaml
test/run_traces.py --checker all
```
