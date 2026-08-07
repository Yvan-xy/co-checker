# Local UniStore TPC-C workflow

This workflow is for the controlled, single-process validation configuration.
It keeps the schema outside the checked epoch, starts every measured mode from
an empty user keyspace, and closes checked results only through graceful TiDB
shutdown. Commands below use the paths and 16-vCPU layout of the recorded test
host; override the variables rather than editing the commands.

## Supported full-mix runner

Use `online/scripts/run_tpcc_global_lock_comparison.sh`. It builds immutable
Release artifacts for the checker, instrumented TiDB, a deterministic patched
go-tpc v1.0.12, and the SQL validation helper. It also creates a detached
pristine TiDB worktree at commit
`b94006d3abffd428634ecc5c499f4bcb7b4d06d1` under `/tmp`; it never switches,
resets, or cleans either development worktree.

The runner executes these modes sequentially:

1. `baseline-original`
2. `serialized-capture-only`
3. `serialized-order-only`
4. `serialized-full`

The default NewOrder, Payment, OrderStatus, Delivery, and StockLevel weights are
`45,43,4,4,4`. Measured comparisons keep that mix fixed. Short diagnostic smoke
runs may override it with five nonnegative weights summing to 100, provided the
NewOrder weight remains positive so go-tpc still emits its tpmC record. The
runner records the exact weights and rejects the override outside `--smoke`.
For example, an eight-warehouse exact NewOrder-only baseline/full diagnostic is:

```sh
SMOKE_WAREHOUSES=8 \
online/scripts/run_tpcc_global_lock_comparison.sh \
  --smoke \
  --baseline-full-only \
  --transaction-weights 100,0,0,0,0 \
  --result-root /absolute/path/to/new-order-diagnostic \
  --tidb /home/dyf/code/db-consistency/tidb-online \
  --go-tpc /home/dyf/go/bin/go-tpc \
  --tidb-cpus 0-7 \
  --client-cpus 8-15
```

Use a positive NewOrder remainder for a near-isolated non-NewOrder type, such as
`1,96,1,1,1` for Payment. These smoke diagnostics are not headline throughput
comparisons.

Set `TIDB_ONLINE_CHECKER_DUPLICATE_READ_DIAGNOSTICS=1` on the runner invocation
to enable the opt-in duplicate audit. The runner records that setting in the
experiment and checked-server environment, clears it for schema bootstrap and
post-check servers, and leaves it disabled by default.

A short validation run is:

```sh
online/scripts/run_tpcc_global_lock_comparison.sh \
  --smoke \
  --result-root /absolute/path/to/tpcc-smoke \
  --tidb /home/dyf/code/db-consistency/tidb-online \
  --go-tpc /home/dyf/go/bin/go-tpc \
  --tidb-cpus 0-7 \
  --client-cpus 8-15
```

A measured comparison is:

```sh
online/scripts/run_tpcc_global_lock_comparison.sh \
  --trials 5 \
  --warmup-time 60s \
  --run-time 600s \
  --warehouses 8 \
  --threads 16 \
  --prepare-threads 1 \
  --result-root /absolute/path/to/tpcc-results \
  --tidb /home/dyf/code/db-consistency/tidb-online \
  --go-tpc /home/dyf/go/bin/go-tpc \
  --tidb-cpus 0-7 \
  --client-cpus 8-15
```

A normal measured invocation first gates on its own exact four-mode smoke suite
under `RESULT_ROOT/smoke`. For a direct pristine-versus-full comparison, add:

```sh
  --baseline-full-only
```

That profile permits only the ordered pair `baseline-original`,
`serialized-full`; it records the same pair in both the measured and smoke
manifests and still starts each mode from an empty store. Arbitrary subsets and
reordered pairs fail closed. The four-mode matrix remains the default required
by the complete plan.

`--resume` requires an identical experiment and artifact manifest, preserves
previous attempts, and skips only modes that the analyzer proves valid.

## Schema and deterministic preparation boundary

Every mode creates a new empty physical UniStore directory. Schema bootstrap and
data preparation use two explicit server lifecycles:

1. With every checker environment variable cleared, patched go-tpc runs
   `prepare --ddl-only`. The runner proves that all nine TPC-C tables are empty,
   records the exact schema/bootstrap digest, and stops the server.
2. The runner starts pristine TiDB for the baseline or instrumented TiDB with a
   fresh checker for a checked mode. Patched go-tpc then runs
   `prepare --skip-ddl` using one worker, seed `20260804`, load time
   `2024-01-01 00:00:00`, retries disabled, and a fixed SQL/write order.

The ordered preparation manifest hashes every successful data-load SQL sink
flush. The runner also records and compares exact schema, data, and combined
database digests against the same-trial pristine reference. It drains the
checked pipeline after preparation before warm-up. Preparation, validation,
drains, stabilization, warm-up, shutdown, and post-check are all outside the
measured interval.

Schema DDL is an explicit capability boundary: checked history starts with the
deterministic user-data load, not system or TPC-C schema bootstrap. Fresh
`CREATE TABLE` under an active checked user session fails closed, while TiDB's
DDL job and worker transactions are internal and are not capture-backed. Only
`CREATE TABLE IF NOT EXISTS` for an already-existing table is accepted as a
proven no-op. Digest equality proves identical initialized state; it is not DDL
read/write evidence.

## Fixed comparison controls and gates

All modes use the same fixed preparation/workload seeds and weighted deck,
optimistic repeatable-read transactions, disabled async commit/1PC/low-resolution
TSO, zero coprocessor cache capacity, disabled store batching and BatchCop, one
preparation worker, identical CPU affinity, compiler settings, and storage
settings. Foreign keys and partitioning are disabled. The measured and warm-up
runs keep generic go-tpc `--ignore-error` disabled and enable the patched
strict continuation policy, which accepts only a concrete MySQL Error 9007,
plus silence so ordinary optimistic write conflicts do not terminate workers
or add mode-dependent per-error logging overhead. For the standard full-mix
comparison, all five structured success and `*_ERR` summaries remain mandatory,
and the analyzer reports both counts and error rates. A smoke-only diagnostic
weight of zero suppresses that operation's go-tpc summaries; the validator
requires summaries only for positive-weight operations and records explicit
zero counts for the others. Throughput is computed from successful NewOrder
transactions only, so diagnostic weights must keep NewOrder positive.
The runner requires one versioned policy record, zero rejected run errors, and
exact equality between its classified Error 9007 count and the sum of all five
`*_ERR` counts; every other client error remains fatal.

This does not relax checker failures: capture/runtime fatal and error metrics,
server fatal markers, process status, lifecycle conservation, live
drain/verdict gates, and the post-workload TPC-C check must all still pass.

After preparation, `tidb-sql -action tpcc-gate` checks the session variables
and text/prepared plan variants used by all five transaction types. The runner
drains after preparation, warm-up, and measured work; requires complete
summaries and positive successes for every positive-weight transaction type,
conserved structured conflict counts, and valid elapsed and
interval accounting; gracefully closes the checked server; then restarts with
capture disabled solely for the standard post-workload `go-tpc tpcc check`
(conditions 1–10 and 12; condition 11 is initialization-only and changes
after successful Delivery transactions).

Mode-specific acceptance is:

- baseline: go-tpc and the complete TPC-C post-check pass;
- capture-only: capture, fragment, and publication lifecycles advance and
  balance;
- order-only: capture and ordering lifecycles advance and balance;
- full: capture, ordering, and checking lifecycles balance and the final verdict
  is `pass`.

Every instrumented mode must have no fatal error, zero active attempts and
finalizers, and zero remaining fragment, queue, or reorder ownership.

After each complete configured mode matrix, the analyzer atomically refreshes
`tpcc-trials.csv`, the compact paper-oriented `tpcc-performance.csv`,
`tpcc-summary.csv`, `tpcc-summary.json`, and `tpcc-report.md`. The compact
CSV contains only median end-to-verdict throughput/overhead plus mode and
valid/invalid trial counts. For the pristine baseline, which has no checker
verdict, the completion boundary is workload-client exit; checked modes include
the complete drain through verdict availability.

Throughput is reported with two distinct denominators. Let `T` be the actual
measured client interval, `D` the time from client exit until the checked
pipeline has drained and its all-work-complete result is available, `B` the
baseline NewOrder success count, and `C` the checked NewOrder success count.
For checked modes, `D` ends only after every accepted publication and event
has reached that mode's terminal target and all fragment, queue, reorder,
attempt, and finalizer ownership is zero:

```text
baseline fixed NO/min = (B / T) * 60
checked fixed NO/min  = (C / T) * 60
checked E2V NO/min    = (C / (T + D)) * 60
fixed overhead        = 1 - checked fixed / baseline fixed
E2V overhead          = 1 - checked E2V / baseline fixed
```

The fixed-window go-tpc `tpmC` remains the database-facing TPC-C metric. The
second rate is labeled end-to-verdict NewOrder/min and is never called tpmC.
It is calculated directly from client start to checker completion, not by
substituting the configured 600-second window. An earlier full-mode prefix
verdict cannot end the denominator. The complete checker drain/result tail is
included; TiDB shutdown and TPC-C post-check validation occur afterward and are
excluded. A tail over 1%, a drain over 30 seconds, or sustained backlog growth
is reported as a failed live-sustainability diagnostic, but does not invalidate
an exactly drained run or remove the tail from end-to-verdict throughput.

## Capability and interpretation limits

Supported execution is one TiDB process, one producer, and in-process UniStore.
Capture-backed reads include PointGet/BatchPointGet and ordinary TiKV DAG
TableReader, IndexReader, IndexLookupReader, validated UPDATE/DELETE readers,
and validated join child readers. Marked storage processors cover table/index
scan, pushed index lookup, selection, Limit, TopN, hash/stream aggregation, and
projection.

TiFlash, external TiKV, streaming, BatchCop, IndexMerge, partitioned scan
shapes, analyze/checksum/background/internal readers, stale/RC reads,
unsupported DML, and dynamic foreign-key cascades fail closed. Evidence contains
only point KVs consumed by supported operators; the checker does not interpret
predicates, ranges, or phantom semantics. Post-check runs outside the epoch so
its diagnostic reads cannot alter checked evidence.

`online/scripts/run_tpcc_long_mode.sh` and the remaining instructions below
are retained only as historical NewOrder-only V1 evidence. They are not the
supported full-mix workflow.

## Historical NewOrder-only workflow

Use `online/scripts/run_tpcc_long_mode.sh` for performance comparisons. Its
defaults reproduce the sustained benchmark under
`online/results/tpcc-live-10m-w8-t16`:

- eight warehouses and 16 clients, after a concurrency sweep showed throughput
  saturating at 16 clients on the recorded host;
- TiDB pinned to CPUs `0-7` and go-tpc pinned to CPUs `8-15`;
- 30 seconds of server stabilization, a separate unmeasured 60-second warm-up,
  and a 600-second measured phase with 10-second diagnostic intervals;
- New-Order-only weights `100,0,0,0,0`, because the standard mixed workload
  uses unsupported coprocessor read paths;
- optimistic transactions with async commit, 1PC, and low-resolution TSO
  disabled in every mode;
- a 2 GiB queue ceiling and 32 GiB capture, reorder, and retained-state
  ceilings. These are fail-closed admission limits rather than eager
  allocations;
- `pidstat`, `mpstat`, and `iostat` sampling during the measured phase, with no
  concurrent `perf` or pprof collection in a headline throughput run.

The historical runner likewise validates the requested elapsed time, interval
count, exactly one `Finished` marker, clean checker close, count conservation,
zero capture/backpressure errors, and all 12 TPC-C post-check
conditions for each of the eight warehouses.

The recorded run used one four-mode trial and a reverse-order full/baseline
confirmation pair:

```sh
export RESULT_ROOT=/home/dyf/code/db-consistency/co-checker/online/results/tpcc-live-10m-w8-t16

for mode in baseline capture-only order-only full; do
  RESULT_ROOT="$RESULT_ROOT" TRIAL=trial-01 \
    online/scripts/run_tpcc_long_mode.sh "$mode"
done

for mode in full baseline; do
  RESULT_ROOT="$RESULT_ROOT" TRIAL=trial-02 \
    online/scripts/run_tpcc_long_mode.sh "$mode"
done

python3 online/tools/analyze_tpcc_long.py "$RESULT_ROOT"
```

Compare only complete baseline/mode pairs with identical configuration
fingerprints. Ten-second windows diagnose drift and stalls; they are correlated
parts of one run, not independent repetitions or a basis for confidence
intervals.

UniStore does not provide a continuously advancing authoritative frontier for
this integration. During the measured phase, full-mode tpmC therefore covers
capture, packing, native publication, queueing, and reorder retention. Ordered
transactions are checked only at the graceful shutdown barrier. Report native
checking time and total checked-shutdown time separately; do not describe the
measured tpmC gap as steady-state full rule-checking overhead.

The step-by-step commands below remain useful for short lifecycle diagnostics
and profiling. Their one-warehouse, 60-second defaults are not the current
headline performance methodology.

## Manual diagnostic inputs and build

```sh
export CHECKER=/home/dyf/code/db-consistency/co-checker
export TIDB=/home/dyf/code/db-consistency/tidb-online
export GO_TPC=/home/dyf/go/bin/go-tpc
export TIDB_SERVER="$TIDB/bin/tidb-server"
export NATIVE_LIBRARY="$CHECKER/online/build-release/libco_checker_online_c.so"
export WORK="$CHECKER/online/results/tpcc-live"
export TEMPLATE="$WORK/template"

export SQL_HOST=127.0.0.1
export SQL_PORT=4400
export STATUS_PORT=14080
export DATABASE=tpcc
export WAREHOUSES=1
export PREPARE_THREADS=4
export RUN_THREADS=8
export RUN_TIME=60s
export TIDB_CPUSET=0-7
export CLIENT_CPUSET=8-15

# Applied to every mode, including baseline, so the stage comparison does not
# confound capture overhead with commit-protocol changes.
export DSN_PARAMS='tidb_enable_async_commit=0&tidb_enable_1pc=0&tidb_txn_mode=optimistic&tidb_low_resolution_tso=0'

test "$(git -C "$CHECKER" branch --show-current)" = online-checker
test "$(git -C "$TIDB" branch --show-current)" = online-checker

cmake -S "$CHECKER/online" -B "$CHECKER/online/build-release" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$CHECKER/online/build-release" -j2
make -C "$TIDB" server
test -x "$TIDB_SERVER"
test -r "$NATIVE_LIBRARY"
```

Run the TiDB build only while no failpoint-enabled Go test is transforming the
worktree. A concurrent failpoint enable/disable can remove generated
`binding__failpoint_binding__.go` files during compilation. That is a build
coordination failure, not an online-checker result.

Use tracked extensions for evidence. The repository-wide ignore rules hide
`*.log` and `*.csv`, so server/workload/pprof summaries below use `.txt` or
`.json`. UniStore `store/`, `tmp/`, and `oom_record/` trees under
`online/results` are intentionally ignored.

## Capture environment reset

Run this before every unchecked server start, including template creation and
post-checking:

```sh
clear_online_checker_environment() {
  local name
  for name in \
    TIDB_ONLINE_CHECKER_MODE \
    TIDB_ONLINE_CHECKER_LIBRARY \
    TIDB_ONLINE_CHECKER_RESULT_PATH \
    TIDB_ONLINE_CHECKER_PRODUCER_ID \
    TIDB_ONLINE_CHECKER_REQUIRED_PRODUCERS \
    TIDB_ONLINE_CHECKER_CLUSTER_EPOCH \
    TIDB_ONLINE_CHECKER_QUEUE_BYTES \
    TIDB_ONLINE_CHECKER_REORDER_BYTES \
    TIDB_ONLINE_CHECKER_CAPTURE_BYTES \
    TIDB_ONLINE_CHECKER_RETAINED_STATE_BYTES \
    TIDB_ONLINE_CHECKER_QUEUE_TIMEOUT \
    TIDB_ONLINE_CHECKER_CAPTURE_TIMEOUT \
    TIDB_ONLINE_CHECKER_CHUNK_SEGMENT_BYTES \
    TIDB_ONLINE_CHECKER_MAX_CACHED_CHUNK_SEGMENTS
  do
    unset "$name"
  done
}
```

## Prepare the empty schema template

The template is prepared once with capture disabled. `go-tpc prepare` is used
to create the exact schema, then every TPC-C table is truncated through one
single-threaded `rawsql` invocation. A separate SQL client verifies every row
count is zero before shutdown.

```sh
test ! -e "$TEMPLATE"
mkdir -p "$TEMPLATE/store" "$TEMPLATE/tmp"
clear_online_checker_environment

taskset -c "$TIDB_CPUSET" "$TIDB_SERVER" \
  --store unistore \
  --path "$TEMPLATE/store" \
  --temp-dir "$TEMPLATE/tmp" \
  --host "$SQL_HOST" -P "$SQL_PORT" \
  --status-host "$SQL_HOST" --status "$STATUS_PORT" \
  --log-file "$TEMPLATE/server.txt" \
  >"$TEMPLATE/server-stdout.txt" 2>&1 &
TIDB_PID=$!

for attempt in $(seq 1 180); do
  curl --fail --silent "http://$SQL_HOST:$STATUS_PORT/status" >/dev/null && break
  kill -0 "$TIDB_PID"
  sleep 1
done
curl --fail --silent "http://$SQL_HOST:$STATUS_PORT/status" \
  >"$TEMPLATE/status.json"

taskset -c "$CLIENT_CPUSET" "$GO_TPC" tpcc prepare \
  -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
  -T "$PREPARE_THREADS" --warehouses "$WAREHOUSES" \
  --no-check --conn-params "$DSN_PARAMS" --output json \
  >"$TEMPLATE/prepare.txt" 2>&1

TRUNCATE_FILES="$CHECKER/online/scripts/sql/truncate_customer.sql"
TRUNCATE_FILES+=",$CHECKER/online/scripts/sql/truncate_district.sql"
TRUNCATE_FILES+=",$CHECKER/online/scripts/sql/truncate_history.sql"
TRUNCATE_FILES+=",$CHECKER/online/scripts/sql/truncate_item.sql"
TRUNCATE_FILES+=",$CHECKER/online/scripts/sql/truncate_new_order.sql"
TRUNCATE_FILES+=",$CHECKER/online/scripts/sql/truncate_order_line.sql"
TRUNCATE_FILES+=",$CHECKER/online/scripts/sql/truncate_orders.sql"
TRUNCATE_FILES+=",$CHECKER/online/scripts/sql/truncate_stock.sql"
TRUNCATE_FILES+=",$CHECKER/online/scripts/sql/truncate_warehouse.sql"

taskset -c "$CLIENT_CPUSET" "$GO_TPC" rawsql run \
  -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" -T 1 --count 9 \
  --conn-params "$DSN_PARAMS" --query-files "$TRUNCATE_FILES" --output json \
  >"$TEMPLATE/truncate.txt" 2>&1

(cd "$TIDB" && go run "$CHECKER/online/tools/tidb_sql.go" \
  -action assert-empty -address "$SQL_HOST:$SQL_PORT" \
  -database "$DATABASE" -params "$DSN_PARAMS") \
  >"$TEMPLATE/empty-counts.json"

kill -TERM "$TIDB_PID"
wait "$TIDB_PID"
unset TIDB_PID
rm -rf "$TEMPLATE/tmp"
```

Never clone the template while its server is running. The checked epoch begins
after a mode clone starts, so the subsequent data load is part of the captured
history and the checker's modeled initial state is empty.

The rawsql multi-file behavior was smoke-tested independently on 2026-07-12:
one invocation with `--count 2` truncated two populated tables exactly once,
and an independent query returned zero for both. Evidence is under
`online/results/tpcc-workflow-smoke`.

## Run one measured mode

Set `MODE` to `baseline`, `capture-only`, `order-only`, or `full`. Use a fresh
directory and template clone for every repetition.

```sh
export MODE=full
export TRIAL=trial-01
export MODE_DIR="$WORK/runs/$TRIAL/$MODE"
test ! -e "$MODE_DIR"
mkdir -p "$MODE_DIR/tmp"
cp -a --reflink=auto "$TEMPLATE/store" "$MODE_DIR/store"

clear_online_checker_environment
if test "$MODE" != baseline; then
  export TIDB_ONLINE_CHECKER_MODE="$MODE"
  export TIDB_ONLINE_CHECKER_LIBRARY="$NATIVE_LIBRARY"
  export TIDB_ONLINE_CHECKER_RESULT_PATH="$MODE_DIR/checker-result.json"
  export TIDB_ONLINE_CHECKER_PRODUCER_ID=1
  export TIDB_ONLINE_CHECKER_REQUIRED_PRODUCERS=1
  export TIDB_ONLINE_CHECKER_CLUSTER_EPOCH="$(date +%s%N)"

  # Capacities are limits, not eager allocations. These explicit values leave
  # headroom for the one-warehouse load and keep resource exhaustion fatal.
  export TIDB_ONLINE_CHECKER_QUEUE_BYTES=1073741824
  export TIDB_ONLINE_CHECKER_REORDER_BYTES=1073741824
  export TIDB_ONLINE_CHECKER_CAPTURE_BYTES=1073741824
  export TIDB_ONLINE_CHECKER_RETAINED_STATE_BYTES=17179869184
  export TIDB_ONLINE_CHECKER_QUEUE_TIMEOUT=5m
  export TIDB_ONLINE_CHECKER_CAPTURE_TIMEOUT=5m
  export TIDB_ONLINE_CHECKER_CHUNK_SEGMENT_BYTES=4096
  export TIDB_ONLINE_CHECKER_MAX_CACHED_CHUNK_SEGMENTS=256
fi

taskset -c "$TIDB_CPUSET" "$TIDB_SERVER" \
  --store unistore \
  --path "$MODE_DIR/store" \
  --temp-dir "$MODE_DIR/tmp" \
  --host "$SQL_HOST" -P "$SQL_PORT" \
  --status-host "$SQL_HOST" --status "$STATUS_PORT" \
  --log-file "$MODE_DIR/server.txt" \
  >"$MODE_DIR/server-stdout.txt" 2>&1 &
TIDB_PID=$!

for attempt in $(seq 1 180); do
  curl --fail --silent "http://$SQL_HOST:$STATUS_PORT/status" >/dev/null && break
  kill -0 "$TIDB_PID"
  sleep 1
done
curl --fail --silent "http://$SQL_HOST:$STATUS_PORT/status" \
  >"$MODE_DIR/status.json"

# The schema already exists. This invocation loads all user data after checked
# admission has started; --no-check suppresses only go-tpc's post-load checks.
taskset -c "$CLIENT_CPUSET" "$GO_TPC" tpcc prepare \
  -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
  -T "$PREPARE_THREADS" --warehouses "$WAREHOUSES" \
  --no-check --conn-params "$DSN_PARAMS" --output json \
  >"$MODE_DIR/prepare.txt" 2>&1

# The timed workload is forbidden unless every NewOrder read shape stays on
# point-get evidence paths and the checked session knobs have their exact
# supported values. Preserve stderr if the gate closes.
if ! (cd "$TIDB" && go run "$CHECKER/online/tools/tidb_sql.go" \
  -action new-order-gate -address "$SQL_HOST:$SQL_PORT" \
  -database "$DATABASE" -params "$DSN_PARAMS") \
  >"$MODE_DIR/capability-gate.json" \
  2>"$MODE_DIR/capability-gate-error.txt"
then
  kill -TERM "$TIDB_PID"
  wait "$TIDB_PID"
  echo 'NewOrder capability gate failed; workload was not started' >&2
  exit 1
fi

# For the full mode, capture a Go CPU profile concurrently with the timed run.
PPROF_PID=
if test "$MODE" = full; then
  curl --fail --silent --show-error \
    "http://$SQL_HOST:$STATUS_PORT/debug/pprof/profile?seconds=30" \
    -o "$MODE_DIR/tidb-cpu.pprof" &
  PPROF_PID=$!
fi

taskset -c "$CLIENT_CPUSET" "$GO_TPC" tpcc run \
  -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
  -T "$RUN_THREADS" --warehouses "$WAREHOUSES" \
  --ignore-error --silence \
  --weight 100,0,0,0,0 --time "$RUN_TIME" --interval 1s \
  --conn-params "$DSN_PARAMS" --output json \
  >"$MODE_DIR/workload.txt" 2>&1

if test -n "$PPROF_PID"; then
  wait "$PPROF_PID"
  go tool pprof -top "$TIDB_SERVER" "$MODE_DIR/tidb-cpu.pprof" \
    >"$MODE_DIR/tidb-pprof-top.txt"
fi

# This is the only authoritative checked close. Do not use SIGKILL and do not
# accept a result copied before wait returns successfully.
kill -TERM "$TIDB_PID"
wait "$TIDB_PID"
unset TIDB_PID
```

For `baseline`, no checker result must exist. For checked modes, inspect the
atomically installed result after shutdown:

```sh
if test "$MODE" = baseline; then
  test ! -e "$MODE_DIR/checker-result.json"
else
  test -s "$MODE_DIR/checker-result.json"
  python3 - "$MODE" "$MODE_DIR/checker-result.json" <<'PY'
import json
import pathlib
import sys

mode, path = sys.argv[1], pathlib.Path(sys.argv[2])
result = json.loads(path.read_text())
expected = {
    "capture-only": "capture-only-complete",
    "order-only": "order-only-complete",
    "full": "pass",
}[mode]
if result.get("format_version") != 1 or result.get("mode") != mode:
    raise SystemExit(f"unexpected result identity: {result}")
if result.get("outcome") != expected:
    raise SystemExit(f"checked run did not complete as {expected}: {result}")
print(json.dumps(result, indent=2, sort_keys=True))
PY
fi
```

`capture-only-complete` and `order-only-complete` are stage-completion states,
not consistency verdicts. A full-mode `violation` must be preserved and
investigated; it is not a database violation until checker, ordering, and
instrumentation causes are excluded. An `error`, missing result, nonzero clean
shutdown, capability-gate failure, or incomplete evidence invalidates the run
and can never be reported as Pass.

## Post-check with capture disabled

Only after the checked server has stopped, restart the same store with every
checker environment variable cleared. TPC-C's invariant queries use
coprocessor scans that are intentionally outside the checked adapter's V1 read
capability, so they must not run inside the captured epoch.

```sh
clear_online_checker_environment
mkdir -p "$MODE_DIR/post-check-tmp"
taskset -c "$TIDB_CPUSET" "$TIDB_SERVER" \
  --store unistore \
  --path "$MODE_DIR/store" \
  --temp-dir "$MODE_DIR/post-check-tmp" \
  --host "$SQL_HOST" -P "$SQL_PORT" \
  --status-host "$SQL_HOST" --status "$STATUS_PORT" \
  --log-file "$MODE_DIR/post-check-server.txt" \
  >"$MODE_DIR/post-check-server-stdout.txt" 2>&1 &
POST_CHECK_PID=$!

for attempt in $(seq 1 180); do
  curl --fail --silent "http://$SQL_HOST:$STATUS_PORT/status" >/dev/null && break
  kill -0 "$POST_CHECK_PID"
  sleep 1
done

taskset -c "$CLIENT_CPUSET" "$GO_TPC" tpcc check \
  -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
  -T 1 --warehouses "$WAREHOUSES" --check-all \
  --conn-params "$DSN_PARAMS" --output json \
  >"$MODE_DIR/post-check.txt" 2>&1

kill -TERM "$POST_CHECK_PID"
wait "$POST_CHECK_PID"
unset POST_CHECK_PID
```

Repeat the mode section in the order `baseline`, `capture-only`, `order-only`,
and `full`, always cloning the stopped empty template. Record at least three
trials before drawing overhead conclusions; the go-tpc data generator and
workload choices are not bit-identical across modes.

## Capability and interpretation limits

- This is one TiDB process, one producer, and UniStore. It does not establish a
  multi-node TiDB/TiKV claim.
- UniStore provides no continuous authoritative resolved frontier for this
  integration. Results close only at the explicit quiescent shutdown barrier.
- No external writer may access the store during a checked epoch.
- V1 supports exact `Point_Get` and `Batch_Point_Get` read evidence. The gate
  rejects table/index readers and scans before NewOrder starts. The standard
  mixed TPC-C workload is unsupported because several transaction types use
  coprocessor scans.
- The controlled comparison uses optimistic repeatable-read transactions with
  async commit, 1PC, and low-resolution TSO disabled. Stale/RC/weak reads,
  pipelined DML, temporary/cached-table paths, and unsupported snapshot paths
  fail closed in the adapter.
- The checker models returned scan rows as point reads and does not prove
  predicate completeness or absence of phantoms.
- The post-check validates TPC-C data invariants; it is not an offline SOWW
  oracle and cannot repair or reinterpret an online result.
