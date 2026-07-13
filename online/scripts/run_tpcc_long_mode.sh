#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: run_tpcc_long_mode.sh MODE

MODE is one of: baseline, capture-only, order-only, full.

The output directory is RESULT_ROOT/TRIAL/MODE. All workload parameters and
paths can be overridden through the environment; the defaults describe the
recorded 8-warehouse, 16-client, 60-second-warm-up, 10-minute experiment.
EOF
}

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 2
fi

MODE=$1
RUNNER_PATH=$(realpath "$0")
case "$MODE" in
  baseline | capture-only | order-only | full) ;;
  *)
    usage >&2
    exit 2
    ;;
esac

CHECKER=${CHECKER:-/home/dyf/code/db-consistency/co-checker}
TIDB=${TIDB:-/home/dyf/code/db-consistency/tidb-online}
GO_TPC=${GO_TPC:-/home/dyf/go/bin/go-tpc}
TIDB_SERVER=${TIDB_SERVER:-$TIDB/bin/tidb-server}
NATIVE_LIBRARY=${NATIVE_LIBRARY:-$CHECKER/online/build-release/libco_checker_online_c.so}
TEMPLATE=${TEMPLATE:-$CHECKER/online/results/tpcc-live/schema-template}
RESULT_ROOT=${RESULT_ROOT:-$CHECKER/online/results/tpcc-live-10m-w8-t16}
TRIAL=${TRIAL:-trial-01}

SQL_HOST=${SQL_HOST:-127.0.0.1}
SQL_PORT=${SQL_PORT:-4400}
STATUS_PORT=${STATUS_PORT:-14080}
DATABASE=${DATABASE:-tpcc}
WAREHOUSES=${WAREHOUSES:-8}
PREPARE_THREADS=${PREPARE_THREADS:-8}
RUN_THREADS=${RUN_THREADS:-16}
WARMUP_TIME=${WARMUP_TIME:-60s}
RUN_TIME=${RUN_TIME:-600s}
OUTPUT_INTERVAL=${OUTPUT_INTERVAL:-10s}
STABILIZE_SECONDS=${STABILIZE_SECONDS:-30}
TIDB_CPUSET=${TIDB_CPUSET:-0-7}
CLIENT_CPUSET=${CLIENT_CPUSET:-8-15}
DSN_PARAMS=${DSN_PARAMS:-tidb_enable_async_commit=0&tidb_enable_1pc=0&tidb_txn_mode=optimistic&tidb_low_resolution_tso=0}

# These are admission ceilings, not eager allocations. UniStore does not
# advance a resolved frontier, so order/full mode retains the complete epoch
# until the graceful close barrier. The defaults leave headroom on the
# recorded 62 GiB host while preserving fail-closed boundedness.
QUEUE_CAPACITY_BYTES=${QUEUE_CAPACITY_BYTES:-2147483648}
REORDER_CAPACITY_BYTES=${REORDER_CAPACITY_BYTES:-34359738368}
CAPTURE_CAPACITY_BYTES=${CAPTURE_CAPACITY_BYTES:-34359738368}
RETAINED_STATE_CAPACITY_BYTES=${RETAINED_STATE_CAPACITY_BYTES:-34359738368}
QUEUE_TIMEOUT=${QUEUE_TIMEOUT:-5m}
CAPTURE_TIMEOUT=${CAPTURE_TIMEOUT:-5m}
CHUNK_SEGMENT_BYTES=${CHUNK_SEGMENT_BYTES:-4096}
MAX_CACHED_CHUNK_SEGMENTS=${MAX_CACHED_CHUNK_SEGMENTS:-256}
MONITOR_INTERVAL_SECONDS=${MONITOR_INTERVAL_SECONDS:-5}

MODE_DIR=$(realpath -m "$RESULT_ROOT/$TRIAL/$MODE")
TIDB_PID=
MONITOR_PIDS=()

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

stop_monitors() {
  local pid
  for pid in "${MONITOR_PIDS[@]}"; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  for pid in "${MONITOR_PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
  done
  MONITOR_PIDS=()
}

cleanup() {
  local status=$?
  trap - EXIT INT TERM
  stop_monitors
  if [[ -n "$TIDB_PID" ]] && kill -0 "$TIDB_PID" 2>/dev/null; then
    kill -TERM "$TIDB_PID" 2>/dev/null || true
    wait "$TIDB_PID" 2>/dev/null || true
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

wait_for_server() {
  local status_file=$1
  local attempt
  for attempt in $(seq 1 180); do
    if ! kill -0 "$TIDB_PID" 2>/dev/null; then
      wait "$TIDB_PID" || true
      echo "TiDB exited before becoming ready" >&2
      return 1
    fi
    if curl --fail --silent "http://$SQL_HOST:$STATUS_PORT/status" >"$status_file"; then
      python3 - "$status_file" "$EXPECTED_TIDB_COMMIT" <<'PY'
import json
import pathlib
import sys

status = json.loads(pathlib.Path(sys.argv[1]).read_text())
if status.get("git_hash") != sys.argv[2]:
    raise SystemExit(
        f"unexpected TiDB server identity: {status.get('git_hash')!r} != {sys.argv[2]!r}"
    )
PY
      return 0
    fi
    sleep 1
  done
  echo "TiDB did not become ready within 180 seconds" >&2
  return 1
}

start_server() {
  local log_file=$1
  local stdout_file=$2
  local temp_dir=$3
  local status_file=$4
  mkdir -p "$temp_dir"
  taskset -c "$TIDB_CPUSET" "$TIDB_SERVER" \
    --store unistore \
    --path "$MODE_DIR/store" \
    --temp-dir "$temp_dir" \
    --host "$SQL_HOST" -P "$SQL_PORT" \
    --status-host "$SQL_HOST" --status "$STATUS_PORT" \
    --log-file "$log_file" \
    >"$stdout_file" 2>&1 &
  TIDB_PID=$!
  wait_for_server "$status_file"
}

stop_server() {
  local phase=$1
  local started_ns ended_ns status
  started_ns=$(date +%s%N)
  kill -TERM "$TIDB_PID"
  set +e
  wait "$TIDB_PID"
  status=$?
  set -e
  ended_ns=$(date +%s%N)
  printf 'exit_status=%d\nshutdown_nanoseconds=%d\n' \
    "$status" "$((ended_ns - started_ns))" >"$phase-shutdown.txt"
  TIDB_PID=
  if [[ $status -ne 0 ]]; then
    echo "TiDB $phase shutdown failed with status $status" >&2
    return "$status"
  fi
}

record_process_snapshot() {
  local output=$1
  {
    date --utc --iso-8601=ns
    sed -n \
      -e '/^Name:/p' \
      -e '/^Pid:/p' \
      -e '/^Threads:/p' \
      -e '/^VmPeak:/p' \
      -e '/^VmSize:/p' \
      -e '/^VmHWM:/p' \
      -e '/^VmRSS:/p' \
      -e '/^RssAnon:/p' \
      -e '/^RssFile:/p' \
      -e '/^voluntary_ctxt_switches:/p' \
      -e '/^nonvoluntary_ctxt_switches:/p' \
      "/proc/$TIDB_PID/status"
    cat "/proc/$TIDB_PID/io"
  } >"$output"
}

start_monitors() {
  pidstat -h -u -r -d -p "$TIDB_PID" "$MONITOR_INTERVAL_SECONDS" \
    >tidb-pidstat.txt 2>&1 &
  MONITOR_PIDS+=("$!")
  mpstat -P ALL "$MONITOR_INTERVAL_SECONDS" >host-mpstat.txt 2>&1 &
  MONITOR_PIDS+=("$!")
  iostat -dx "$MONITOR_INTERVAL_SECONDS" >host-iostat.txt 2>&1 &
  MONITOR_PIDS+=("$!")
}

validate_go_tpc_run() {
  local expected_duration=$1
  local output_interval=$2
  local input=$3
  local output=$4
  python3 - "$expected_duration" "$output_interval" "$input" >"$output" <<'PY'
import json
import math
import pathlib
import re
import sys

def duration_seconds(text):
    units = {"h": 3600.0, "m": 60.0, "s": 1.0, "ms": 0.001}
    pattern = re.compile(r"([0-9]+(?:\.[0-9]+)?)(ms|h|m|s)")
    position = 0
    total = 0.0
    for match in pattern.finditer(text):
        if match.start() != position:
            raise SystemExit(f"unsupported duration {text!r}")
        total += float(match.group(1)) * units[match.group(2)]
        position = match.end()
    if position != len(text) or total <= 0:
        raise SystemExit(f"unsupported duration {text!r}")
    return total

expected = duration_seconds(sys.argv[1])
interval = duration_seconds(sys.argv[2])
text = pathlib.Path(sys.argv[3]).read_text(errors="replace")
finished = sum(line.strip() == "Finished" for line in text.splitlines())
current_intervals = 0
summary = None
error_summary = None
final = None
malformed_json_records = 0
for line in text.splitlines():
    stripped = line.strip()
    if not stripped.startswith(("[{", "{")):
        continue
    try:
        decoded = json.loads(stripped)
    except json.JSONDecodeError:
        malformed_json_records += 1
        continue
    records = decoded if isinstance(decoded, list) else [decoded]
    for record in records:
        if not isinstance(record, dict):
            continue
        prefix = str(record.get("Prefix", "")).strip()
        if prefix == "[Current]" and record.get("Operation") == "NEW_ORDER":
            current_intervals += 1
        elif prefix == "[Summary]" and record.get("Operation") == "NEW_ORDER":
            summary = record
        elif prefix == "[Summary]" and record.get("Operation") == "NEW_ORDER_ERR":
            error_summary = record
        elif "tpmC" in record:
            final = record

if finished != 1:
    raise SystemExit(f"expected one Finished marker, found {finished}")
if malformed_json_records:
    raise SystemExit(f"found {malformed_json_records} malformed JSON records")
if summary is None or final is None:
    raise SystemExit("go-tpc final summary is incomplete")
takes = float(summary["Takes(s)"])
tolerance = max(2.0, expected * 0.01)
if abs(takes - expected) > tolerance:
    raise SystemExit(f"summary duration {takes} differs from expected {expected}")
minimum_intervals = max(1, math.floor(expected / interval) - 1)
if current_intervals < minimum_intervals:
    raise SystemExit(
        f"only {current_intervals} intervals, expected at least {minimum_intervals}"
    )

print(json.dumps({
    "expected_seconds": expected,
    "summary_seconds": takes,
    "current_interval_count": current_intervals,
    "minimum_interval_count": minimum_intervals,
    "finished_marker_count": finished,
    "malformed_json_record_count": malformed_json_records,
    "new_order_count": int(summary["Count"]),
    "new_order_error_count": int(error_summary["Count"]) if error_summary else 0,
    "tpm_c": float(final["tpmC"]),
    "go_tpc_ignore_error": True,
}, indent=2, sort_keys=True))
PY
}

for command in realpath git sha256sum curl taskset python3 pidstat mpstat iostat \
  lscpu free df uname go ss /usr/bin/time; do
  command -v "$command" >/dev/null
done
test -x "$GO_TPC"
test -x "$TIDB_SERVER"
test -r "$NATIVE_LIBRARY"
test -d "$TEMPLATE/store"
test "$(git -C "$CHECKER" branch --show-current)" = online-checker
test "$(git -C "$TIDB" branch --show-current)" = online-checker
test ! -e "$MODE_DIR"
if [[ -n "$(ss -H -ltn "sport = :$SQL_PORT")" ]]; then
  echo "SQL port $SQL_PORT is already listening" >&2
  exit 1
fi
if [[ -n "$(ss -H -ltn "sport = :$STATUS_PORT")" ]]; then
  echo "status port $STATUS_PORT is already listening" >&2
  exit 1
fi
EXPECTED_TIDB_COMMIT=$(git -C "$TIDB" rev-parse HEAD)

mkdir -p "$MODE_DIR"
cp -a --reflink=auto "$TEMPLATE/store" "$MODE_DIR/store"
cd "$MODE_DIR"

export MODE CHECKER TIDB GO_TPC TIDB_SERVER NATIVE_LIBRARY TEMPLATE RESULT_ROOT
export RUNNER_PATH
export TRIAL MODE_DIR SQL_HOST SQL_PORT STATUS_PORT DATABASE WAREHOUSES
export PREPARE_THREADS RUN_THREADS WARMUP_TIME RUN_TIME OUTPUT_INTERVAL
export STABILIZE_SECONDS TIDB_CPUSET CLIENT_CPUSET DSN_PARAMS
export QUEUE_CAPACITY_BYTES REORDER_CAPACITY_BYTES CAPTURE_CAPACITY_BYTES
export RETAINED_STATE_CAPACITY_BYTES QUEUE_TIMEOUT CAPTURE_TIMEOUT
export CHUNK_SEGMENT_BYTES MAX_CACHED_CHUNK_SEGMENTS MONITOR_INTERVAL_SECONDS

python3 - <<'PY'
import datetime
import hashlib
import json
import os
import pathlib
import platform
import subprocess

def git(repo, *args):
    return subprocess.check_output(["git", "-C", repo, *args], text=True).strip()

def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

integer_names = {
    "SQL_PORT", "STATUS_PORT", "WAREHOUSES", "PREPARE_THREADS",
    "RUN_THREADS", "STABILIZE_SECONDS", "QUEUE_CAPACITY_BYTES",
    "REORDER_CAPACITY_BYTES", "CAPTURE_CAPACITY_BYTES",
    "RETAINED_STATE_CAPACITY_BYTES", "CHUNK_SEGMENT_BYTES",
    "MAX_CACHED_CHUNK_SEGMENTS", "MONITOR_INTERVAL_SECONDS",
}
setting_names = [
    "MODE", "TRIAL", "SQL_HOST", "SQL_PORT", "STATUS_PORT", "DATABASE",
    "WAREHOUSES", "PREPARE_THREADS", "RUN_THREADS", "WARMUP_TIME",
    "RUN_TIME", "OUTPUT_INTERVAL", "STABILIZE_SECONDS", "TIDB_CPUSET",
    "CLIENT_CPUSET", "DSN_PARAMS", "QUEUE_CAPACITY_BYTES",
    "REORDER_CAPACITY_BYTES", "CAPTURE_CAPACITY_BYTES",
    "RETAINED_STATE_CAPACITY_BYTES", "QUEUE_TIMEOUT", "CAPTURE_TIMEOUT",
    "CHUNK_SEGMENT_BYTES", "MAX_CACHED_CHUNK_SEGMENTS",
    "MONITOR_INTERVAL_SECONDS",
]
settings = {
    name.lower(): int(os.environ[name]) if name in integer_names else os.environ[name]
    for name in setting_names
}
settings["go_tpc_ignore_error"] = True
settings["transaction_weights"] = [100, 0, 0, 0, 0]
checker = os.environ["CHECKER"]
tidb = os.environ["TIDB"]
server = os.environ["TIDB_SERVER"]
library = os.environ["NATIVE_LIBRARY"]
config = {
    "schema_version": 1,
    "created_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "host": {
        "node": platform.node(),
        "platform": platform.platform(),
        "kernel": platform.release(),
        "cpu_count": os.cpu_count(),
    },
    "sources": {
        "checker_commit": git(checker, "rev-parse", "HEAD"),
        "checker_status": git(checker, "status", "--short"),
        "tidb_commit": git(tidb, "rev-parse", "HEAD"),
        "tidb_status": git(tidb, "status", "--short"),
    },
    "binaries": {
        "tidb_server": {"path": server, "sha256": sha256(server)},
        "native_library": {"path": library, "sha256": sha256(library)},
        "go_tpc": {"path": os.environ["GO_TPC"], "sha256": sha256(os.environ["GO_TPC"])},
        "benchmark_runner": {
            "path": os.environ["RUNNER_PATH"],
            "sha256": sha256(os.environ["RUNNER_PATH"]),
        },
    },
    "template": str(pathlib.Path(os.environ["TEMPLATE"]).resolve()),
    "settings": settings,
}
pathlib.Path("configuration.json").write_text(json.dumps(config, indent=2, sort_keys=True) + "\n")
PY

lscpu >lscpu.txt
free -b >memory-before.txt
df -B1 "$MODE_DIR" >filesystem-before.txt
uname -a >uname.txt

clear_online_checker_environment
if [[ "$MODE" != baseline ]]; then
  export TIDB_ONLINE_CHECKER_MODE=$MODE
  export TIDB_ONLINE_CHECKER_LIBRARY=$NATIVE_LIBRARY
  export TIDB_ONLINE_CHECKER_RESULT_PATH=$MODE_DIR/checker-result.json
  export TIDB_ONLINE_CHECKER_PRODUCER_ID=1
  export TIDB_ONLINE_CHECKER_REQUIRED_PRODUCERS=1
  export TIDB_ONLINE_CHECKER_CLUSTER_EPOCH
  TIDB_ONLINE_CHECKER_CLUSTER_EPOCH=$(date +%s%N)
  export TIDB_ONLINE_CHECKER_QUEUE_BYTES=$QUEUE_CAPACITY_BYTES
  export TIDB_ONLINE_CHECKER_REORDER_BYTES=$REORDER_CAPACITY_BYTES
  export TIDB_ONLINE_CHECKER_CAPTURE_BYTES=$CAPTURE_CAPACITY_BYTES
  export TIDB_ONLINE_CHECKER_RETAINED_STATE_BYTES=$RETAINED_STATE_CAPACITY_BYTES
  export TIDB_ONLINE_CHECKER_QUEUE_TIMEOUT=$QUEUE_TIMEOUT
  export TIDB_ONLINE_CHECKER_CAPTURE_TIMEOUT=$CAPTURE_TIMEOUT
  export TIDB_ONLINE_CHECKER_CHUNK_SEGMENT_BYTES=$CHUNK_SEGMENT_BYTES
  export TIDB_ONLINE_CHECKER_MAX_CACHED_CHUNK_SEGMENTS=$MAX_CACHED_CHUNK_SEGMENTS
fi

start_server "$MODE_DIR/server.txt" "$MODE_DIR/server-stdout.txt" \
  "$MODE_DIR/tmp" "$MODE_DIR/status.json"

/usr/bin/time -v -o prepare-time.txt \
  taskset -c "$CLIENT_CPUSET" "$GO_TPC" tpcc prepare \
    -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
    -T "$PREPARE_THREADS" --warehouses "$WAREHOUSES" \
    --no-check --conn-params "$DSN_PARAMS" --output json \
    >prepare.txt 2>&1

if ! (cd "$TIDB" && go run "$CHECKER/online/tools/tidb_sql.go" \
  -action new-order-gate -address "$SQL_HOST:$SQL_PORT" \
  -database "$DATABASE" -params "$DSN_PARAMS") \
  >capability-gate.json 2>capability-gate-error.txt
then
  echo "NewOrder capability gate failed; workload was not started" >&2
  exit 1
fi

sleep "$STABILIZE_SECONDS"

/usr/bin/time -v -o warmup-time.txt \
  taskset -c "$CLIENT_CPUSET" "$GO_TPC" tpcc run \
    -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
    -T "$RUN_THREADS" --warehouses "$WAREHOUSES" \
    --ignore-error \
    --weight 100,0,0,0,0 --time "$WARMUP_TIME" --interval "$OUTPUT_INTERVAL" \
    --conn-params "$DSN_PARAMS" --output json \
    >warmup.txt 2>&1
validate_go_tpc_run "$WARMUP_TIME" "$OUTPUT_INTERVAL" warmup.txt \
  warmup-validation.json

record_process_snapshot tidb-proc-before.txt
start_monitors
set +e
/usr/bin/time -v -o workload-time.txt \
  taskset -c "$CLIENT_CPUSET" "$GO_TPC" tpcc run \
    -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
    -T "$RUN_THREADS" --warehouses "$WAREHOUSES" \
    --ignore-error \
    --weight 100,0,0,0,0 --time "$RUN_TIME" --interval "$OUTPUT_INTERVAL" \
    --conn-params "$DSN_PARAMS" --output json \
    >workload.txt 2>&1
WORKLOAD_STATUS=$?
set -e
printf '%d\n' "$WORKLOAD_STATUS" >workload-exit-status.txt
stop_monitors
record_process_snapshot tidb-proc-after.txt

if [[ $WORKLOAD_STATUS -eq 0 ]]; then
  validate_go_tpc_run "$RUN_TIME" "$OUTPUT_INTERVAL" workload.txt \
    workload-validation.json
fi

stop_server checked
rm -rf "$MODE_DIR/tmp"

if [[ $WORKLOAD_STATUS -ne 0 ]]; then
  echo "go-tpc measured workload failed with status $WORKLOAD_STATUS" >&2
  exit "$WORKLOAD_STATUS"
fi

if [[ "$MODE" = baseline ]]; then
  test ! -e checker-result.json
  printf '{"mode":"baseline","checker_result":"disabled"}\n' \
    >checker-result-validation.json
else
  test -s checker-result.json
  python3 - "$MODE" checker-result.json >checker-result-validation.json <<'PY'
import json
import pathlib
import sys

mode = sys.argv[1]
result = json.loads(pathlib.Path(sys.argv[2]).read_text())
adapter = result["adapter_metrics"]
native = result["native_metrics"]
expected = {
    "capture-only": "capture-only-complete",
    "order-only": "order-only-complete",
    "full": "pass",
}[mode]

def require(condition, message):
    if not condition:
        raise SystemExit(message)

require(result.get("format_version") == 1, "unexpected result format")
require(result.get("mode") == mode, "result mode mismatch")
require(result.get("outcome") == expected, f"unexpected outcome: {result.get('outcome')}")
require(result.get("close_status") == 0, "checker close failed")
require(native.get("closed") is True and native.get("fatal") is False, "native runtime did not close cleanly")
require(native.get("active_attempts") == 0, "active attempts remain at close")
require(adapter["attempts_registered"] == adapter["attempts_published"] + adapter["attempts_aborted"], "adapter attempt conservation failed")
require(adapter["attempts_published"] == native["captured_transactions"], "published/captured transaction mismatch")
require(adapter["captured_events"] == native["captured_events"], "adapter/native event mismatch")
require(adapter["fatal_calls"] == 0 and adapter["publish_errors"] == 0 and adapter["abort_errors"] == 0, "adapter errors were recorded")
require(native["capture_backpressure_wait_count"] == 0, "capture backpressure occurred")
require(native["queue_backpressure_wait_count"] == 0, "queue backpressure occurred")
require(native["queue_depth"] == 0 and native["reorder_bytes"] == 0, "runtime did not drain at close")
if mode in {"order-only", "full"}:
    require(native["ordered_transactions"] == native["captured_transactions"], "captured/ordered mismatch")
if mode == "full":
    require(native["checked_transactions"] == native["captured_transactions"], "captured/checked transaction mismatch")
    require(native["checked_events"] == native["captured_events"], "captured/checked event mismatch")
    require(native["violating_transactions"] == 0, "consistency violation requires investigation")
    require(result.get("verdict", {}).get("status") == "pass", "full checker verdict is not Pass")

print(json.dumps({
    "mode": mode,
    "outcome": result["outcome"],
    "attempt_conservation": True,
    "captured_transactions": native["captured_transactions"],
    "captured_events": native["captured_events"],
    "ordered_transactions": native["ordered_transactions"],
    "checked_transactions": native["checked_transactions"],
    "checked_events": native["checked_events"],
    "violating_transactions": native["violating_transactions"],
    "capture_backpressure_wait_count": native["capture_backpressure_wait_count"],
    "queue_backpressure_wait_count": native["queue_backpressure_wait_count"],
}, indent=2, sort_keys=True))
PY
fi

clear_online_checker_environment
start_server "$MODE_DIR/post-check-server.txt" \
  "$MODE_DIR/post-check-server-stdout.txt" "$MODE_DIR/tmp" \
  "$MODE_DIR/post-check-status.json"

/usr/bin/time -v -o post-check-time.txt \
  taskset -c "$CLIENT_CPUSET" "$GO_TPC" tpcc check \
    -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
    -T 1 --warehouses "$WAREHOUSES" --check-all \
    --conn-params "$DSN_PARAMS" --output json \
    >post-check.txt 2>&1

python3 - "$WAREHOUSES" post-check.txt >post-check-validation.json <<'PY'
import json
import pathlib
import re
import sys

warehouses = int(sys.argv[1])
text = pathlib.Path(sys.argv[2]).read_text(errors="replace")
lower = text.lower()
failure_markers = [
    marker for marker in ("execute check failed", "panic:", "fatal error")
    if marker in lower
]
if failure_markers:
    raise SystemExit(f"TPC-C post-check reported failure markers: {failure_markers}")
if sum(line.strip() == "Finished" for line in text.splitlines()) != 1:
    raise SystemExit("TPC-C post-check did not finish exactly once")

pattern = re.compile(r"^begin to check warehouse (\d+) at condition (3\.3\.2\.(\d+))$")
observed = []
for line in text.splitlines():
    match = pattern.match(line.strip())
    if match:
        observed.append((int(match.group(1)), match.group(2)))
expected = {
    (warehouse, f"3.3.2.{condition}")
    for warehouse in range(1, warehouses + 1)
    for condition in range(1, 13)
}
observed_set = set(observed)
if observed_set != expected or len(observed) != len(expected):
    missing = sorted(expected - observed_set)
    unexpected = sorted(observed_set - expected)
    raise SystemExit(
        f"TPC-C post-check coverage mismatch: missing={missing}, "
        f"unexpected={unexpected}, records={len(observed)}, expected={len(expected)}"
    )
print(json.dumps({
    "outcome": "pass",
    "warehouses": warehouses,
    "conditions_per_warehouse": 12,
    "checks_completed": len(observed),
    "failure_markers": failure_markers,
}, indent=2, sort_keys=True))
PY

stop_server post-check
rm -rf "$MODE_DIR/tmp"
free -b >memory-after.txt
df -B1 "$MODE_DIR" >filesystem-after.txt

python3 - <<'PY'
import datetime
import json
import pathlib

pathlib.Path("completed.json").write_text(json.dumps({
    "schema_version": 1,
    "status": "complete",
    "completed_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
}, indent=2, sort_keys=True) + "\n")
PY

trap - EXIT INT TERM
