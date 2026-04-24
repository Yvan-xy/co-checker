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

Useful validation commands:

```bash
ctest --test-dir build --output-on-failure
python3 -m unittest trace-gen/test_preprocess_trace.py
build/co-checker --checker co-si traces/2s-4t-10e-mutual.yaml
build/co-checker --checker co-si traces/2s-12t-67e-mixed.yaml
```
