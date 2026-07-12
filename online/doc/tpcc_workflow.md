# Local UniStore TPC-C workflow

This workflow is for the controlled, single-process validation configuration.
It keeps the schema outside the checked epoch, starts every measured mode from
an empty user keyspace, and closes checked results only through graceful TiDB
shutdown. Commands below use the paths and 16-vCPU layout of the recorded test
host; override the variables rather than editing the commands.

## Fixed inputs and build

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
