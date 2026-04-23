# Trace Generator

Generate small and medium TiDB transaction trace fixtures for the checker:

```bash
./trace-gen/generate.sh
```

Generated fixtures:

- `small-basic.yaml`
- `small-delete-miss.yaml`
- `small-two-session-mutual.yaml`
- `medium-mixed.yaml`

By default the script expects the instrumented TiDB checkout at `../tidb` relative
to this repo and writes YAML files into `./traces`.

Override paths when needed:

```bash
TIDB_REPO=/path/to/tidb ./trace-gen/generate.sh /path/to/output/traces
```

The generator temporarily copies `co_checker_tracegen_test.go` into TiDB's
`pkg/session/test/txn` package, runs real TiDB `testkit` workloads with
`TIDB_TXN_TRACE_ENABLE=1`, removes the temporary test file on exit, and
preprocesses the generated traces in place so the final YAML files contain only
committed transactions sorted by commit timestamp. The preprocessor also
normalizes TiDB connection IDs to per-file `session_id` values starting at `1`.

## Preprocess a Trace

To preprocess a raw trace manually, remove rolled-back or otherwise uncommitted
transactions and sort the remaining transactions by commit order:

```bash
python3 ./trace-gen/preprocess_trace.py traces/small-basic.yaml -o traces/small-basic.committed.yaml
```

The preprocessor reads the YAML document stream emitted by TiDB, keeps only
transactions with `status: "committed"` and a valid `commit_ts`, and writes a new
YAML stream sorted by ascending `commit_ts`. Older trace documents without a
`status` field are kept only if they have a valid `commit_ts`. Any `session_id`
values are remapped to a dense per-file range starting at `1`.
