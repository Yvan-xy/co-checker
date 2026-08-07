# Trace Generator

Generate TiDB transaction trace fixtures for the checker:

```bash
./trace-gen/generate.sh
```

Generated fixtures:

- `1s-3t-7e-basic.yaml`
- `1s-3t-5e-delete-miss.yaml`
- `2s-4t-10e-mutual.yaml`
- `2s-10t-28e.yaml`
- `2s-12t-56e-mixed.yaml`
- `1s-4t-18e-overwrite-chain.yaml`
- `2s-5t-8e-delete-reinsert.yaml`
- `3s-5t-18e-fanout.yaml`
- `3s-30t-186e-large-roundrobin.yaml`
- `3s-35t-233e-large-mixed.yaml`
- `3s-252t-2430e-very-large-roundrobin.yaml`
- `3s-234t-1956e-very-large-mixed.yaml`
- `20s-402t-3500e-many-session-roundrobin.yaml`
- `30s-440t-3353e-many-session-mixed.yaml`
- `100s-502t-3800e-many-session-roundrobin.yaml`

By default the script expects the instrumented TiDB checkout at `../tidb` relative
to this repo, writes YAML files into `./traces`, and writes matching binary
trace files into `./bin_traces`.

Override paths when needed:

```bash
TIDB_REPO=/path/to/tidb ./trace-gen/generate.sh /path/to/output/traces
BIN_TRACE_DIR=/tmp/bin-traces ./trace-gen/generate.sh
```

The generator temporarily copies `co_checker_tracegen_test.go` into TiDB's
`pkg/session/test/txn` package, runs real TiDB `testkit` workloads with
`TIDB_TXN_TRACE_ENABLE=1`, removes the temporary test file on exit, and
preprocesses the generated traces in place so the final YAML files contain only
committed transactions sorted by commit timestamp. The preprocessor also
normalizes TiDB connection IDs to per-file `session_id` values starting at `1`
and renames generated files to
`<sessions>s-<transactions>t-<events>e[-suffix].yaml` based on the
checker-ready trace contents. After preprocessing, each YAML trace is converted
to a compact binary `.trace` file. Binary output mirrors the YAML path under
`./bin_traces`: for example, `./traces/3s-5t-18e-fanout.yaml` becomes
`./bin_traces/3s-5t-18e-fanout.trace`.

## Preprocess a Trace

To preprocess a raw trace manually, remove rolled-back or otherwise uncommitted
transactions and sort the remaining transactions by commit order:

```bash
python3 ./trace-gen/preprocess_trace.py traces/1s-3t-7e-basic.yaml -o traces/1s-3t-7e-basic.committed.yaml
```

Add `--rename` with `--in-place` or `--output` to derive the output filename
from the postprocessed trace counts:

```bash
python3 ./trace-gen/preprocess_trace.py --in-place --rename traces/3s-large-mixed.yaml
```

The preprocessor reads the YAML document stream emitted by TiDB, keeps only
transactions with `status: "committed"`, a valid `commit_ts`, and a `session_id`,
then drops the `status` field from the checker-ready output because all remaining
transactions are committed. Older trace documents without a `status` field are
kept only if they have a valid `commit_ts` and `session_id`. It also removes
same-transaction local-buffer probe reads (`op: R`, `source: mem_buffer`) and
drops remaining `source` metadata because the checker validates global snapshot
visibility. Any `session_id` values are remapped to a dense per-file range
starting at `1`.

## Generate TPC-C Traces

Generate real TiDB TPC-C traces with PingCAP `go-tpc`:

```bash
./trace-gen/generate_tpcc.sh
```

The script starts the instrumented TiDB server from `../tidb` with
`TIDB_TXN_TRACE_ENABLE=1`, waits for the SQL port, runs `go-tpc tpcc prepare`
and `go-tpc tpcc run`, stops TiDB, and preprocesses the raw trace into
`./traces/tpcc/`. Raw TiDB output and the TiDB log are kept under
`./traces/tpcc/raw/`. The checker-ready YAML is also converted to a binary trace
under the matching `./bin_traces/tpcc/` path.

Useful knobs:

```bash
WAREHOUSES=1 THREADS=1 RUN_TIME=5s ./trace-gen/generate_tpcc.sh
TRANSACTION_WEIGHTS=1,0,0,0,99 RUN_TIME=60s ./trace-gen/generate_tpcc.sh
OUTPUT_DIR=/tmp/tpcc-traces PORT=4030 STATUS_PORT=10300 ./trace-gen/generate_tpcc.sh
BIN_TRACE_DIR=/tmp/bin-traces ./trace-gen/generate_tpcc.sh
```

Use `THREADS` to control TPC-C client concurrency. Multi-threaded runs create
overlapping transactions; the SO-only checker handles those by accepting older
viable snapshots when the transaction snapshot is not the latest commit-order
prefix.

`TRANSACTION_WEIGHTS` configures the five go-tpc weights in NewOrder, Payment,
OrderStatus, Delivery, and StockLevel order. It must contain five nonnegative
integers summing to 100. Non-default weights are embedded in generated names so
they cannot silently overwrite a full-mix trace.

For large raw TPC-C traces, use the C++ preprocessor instead of the Python
preprocessor:

```bash
cmake --build build --target trace-preprocess
./build/trace-preprocess -o traces/tpcc/tpcc.yaml --rename traces/tpcc/raw/tpcc.raw.yaml
```
