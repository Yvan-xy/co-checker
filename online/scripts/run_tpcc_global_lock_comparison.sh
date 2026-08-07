#!/usr/bin/env bash

set -Eeuo pipefail

readonly PRISTINE_TIDB_COMMIT="b94006d3abffd428634ecc5c499f4bcb7b4d06d1"
readonly WORKLOAD_LABEL="TPC-C full mix"
readonly DEFAULT_TRANSACTION_WEIGHTS="45,43,4,4,4"
TRANSACTION_WEIGHTS="45,43,4,4,4"
readonly PREPARE_SEED=20260804
readonly WORKLOAD_SEED=14
readonly PREPARE_LOAD_TIME="2024-01-01 00:00:00"
readonly TPCC_PARTITION_TYPE=1
readonly TPCC_PARTS=1
readonly TPCC_USE_FK=false
readonly GO_TPC_VERSION="v1.0.12"
readonly -a FOUR_MODE_MATRIX=(
  "baseline-original"
  "serialized-capture-only"
  "serialized-order-only"
  "serialized-full"
)
MODES=("${FOUR_MODE_MATRIX[@]}")

usage() {
  cat <<'EOF'
Usage:
  run_tpcc_global_lock_comparison.sh \
    --trials 5 \
    --warmup-time 60s \
    --run-time 600s \
    --warehouses 8 \
    --threads 16 \
    --result-root /absolute/path/to/results

Required:
  --result-root PATH       Absolute, append-only result root.

Workload:
  --trials N               Paired trials for the selected matrix (default: 5; minimum: 5).
  --warmup-time DURATION   Unmeasured go-tpc warm-up (default: 60s).
  --run-time DURATION      Measured go-tpc duration (default: 600s).
  --warehouses N           TPC-C warehouses (default: 8).
  --threads N              Measured/warm-up client threads (default: 16).
  --prepare-threads N      Data-load threads; must be 1 (default: 1).
  --output-interval D      go-tpc reporting interval (default: 10s).
  --stabilize-seconds N    Pause between load and warm-up (default: 30).
  --transaction-weights W Diagnostic smoke weights for NewOrder, Payment,
                          OrderStatus, Delivery, and StockLevel. W must be five
                          nonnegative integers summing to 100, with a positive
                          NewOrder weight. Non-default weights require --smoke.

Paths and placement:
  --checker PATH           co-checker worktree.
  --tidb PATH              instrumented TiDB worktree.
  --go-tpc PATH            go-tpc executable.
  --analyzer PATH          comparison analyzer.
  --baseline-worktree PATH Detached pristine worktree under /tmp.
  --tidb-cpus LIST         taskset CPU list for TiDB.
  --client-cpus LIST       taskset CPU list for go-tpc.
  --sql-host HOST          TiDB SQL/status host (default: 127.0.0.1).
  --sql-port PORT          TiDB SQL port (default: 4400).
  --status-port PORT       TiDB status port (default: 14080).
  --build-jobs N           Parallel checker build jobs (default: 2).
  --monitor-interval N     pidstat interval in seconds (default: 1).

Control:
  --smoke                  Run only a fast one-trial selected-matrix smoke.
  --baseline-full-only     Run only the exact ordered pair baseline-original,
                           serialized-full. The default remains all four
                           modes.
  --resume                 Require an exact manifest match; skip valid modes
                           and append a new attempt for invalid/incomplete ones.
  -h, --help               Show this help.

  --checker-drain-timeout DURATION
                           Operational live-drain timeout (default: 30m).
A normal invocation automatically runs and gates on a short suite with the
selected mode matrix under RESULT_ROOT/smoke before starting the requested
trials. Set SMOKE_WAREHOUSES to override the smoke-only warehouse count
(default: 1). Runs are always sequential. The pristine TiDB tree is never
switched or reset.
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

require_value() {
  local option=$1
  local count=$2
  if (( count < 2 )); then
    die "$option requires a value"
  fi
}

SCRIPT_PATH=$(realpath "$0")
CHECKER_ROOT=$(realpath "$(dirname "$SCRIPT_PATH")/../..")
TIDB_ROOT=${TIDB_ROOT:-/home/dyf/code/db-consistency/tidb-online}
GO_TPC=${GO_TPC:-}
ANALYZER=${ANALYZER:-$CHECKER_ROOT/online/tools/analyze_tpcc_global_lock.py}
RESULT_ROOT=
BASELINE_WORKTREE=${BASELINE_WORKTREE:-/tmp/tidb-online-pristine-${PRISTINE_TIDB_COMMIT:0:12}-u$(id -u)}

TRIALS=5
WARMUP_TIME=60s
RUN_TIME=600s
WAREHOUSES=8
THREADS=16
PREPARE_THREADS=1
OUTPUT_INTERVAL=10s
STABILIZE_SECONDS=30
MONITOR_INTERVAL_SECONDS=1
CHECKER_DRAIN_TIMEOUT=30m
BUILD_JOBS=2
SMOKE_WAREHOUSES=${SMOKE_WAREHOUSES:-1}
DUPLICATE_READ_DIAGNOSTICS=${TIDB_ONLINE_CHECKER_DUPLICATE_READ_DIAGNOSTICS:-0}

SQL_HOST=127.0.0.1
SQL_PORT=4400
STATUS_PORT=14080
DATABASE=tpcc
TIDB_CPUSET=
CLIENT_CPUSET=

SMOKE=false
RESUME=false
BASELINE_FULL_ONLY=false

# These are fail-closed ceilings, not eager allocations. Reorder capacity and
# the frontier interval are deliberately absent: serialized-global-lock
# requires an effective reorder capacity of zero and has no frontier pump.
QUEUE_CAPACITY_BYTES=${QUEUE_CAPACITY_BYTES:-2147483648}
CAPTURE_CAPACITY_BYTES=${CAPTURE_CAPACITY_BYTES:-34359738368}
RETAINED_STATE_CAPACITY_BYTES=${RETAINED_STATE_CAPACITY_BYTES:-34359738368}
QUEUE_TIMEOUT=${QUEUE_TIMEOUT:-5m}
CAPTURE_TIMEOUT=${CAPTURE_TIMEOUT:-5m}
CHUNK_SEGMENT_BYTES=${CHUNK_SEGMENT_BYTES:-4096}
MAX_CACHED_CHUNK_SEGMENTS=${MAX_CACHED_CHUNK_SEGMENTS:-256}

while (( $# > 0 )); do
  case "$1" in
    --trials)
      require_value "$1" "$#"
      TRIALS=$2
      shift 2
      ;;
    --warmup-time)
      require_value "$1" "$#"
      WARMUP_TIME=$2
      shift 2
      ;;
    --run-time)
      require_value "$1" "$#"
      RUN_TIME=$2
      shift 2
      ;;
    --warehouses)
      require_value "$1" "$#"
      WAREHOUSES=$2
      shift 2
      ;;
    --threads)
      require_value "$1" "$#"
      THREADS=$2
      shift 2
      ;;
    --prepare-threads)
      require_value "$1" "$#"
      PREPARE_THREADS=$2
      shift 2
      ;;
    --output-interval)
      require_value "$1" "$#"
      OUTPUT_INTERVAL=$2
      shift 2
      ;;
    --stabilize-seconds)
      require_value "$1" "$#"
      STABILIZE_SECONDS=$2
      shift 2
      ;;
    --transaction-weights)
      require_value "$1" "$#"
      TRANSACTION_WEIGHTS=$2
      shift 2
      ;;
    --checker)
      require_value "$1" "$#"
      CHECKER_ROOT=$2
      shift 2
      ;;
    --tidb)
      require_value "$1" "$#"
      TIDB_ROOT=$2
      shift 2
      ;;
    --go-tpc)
      require_value "$1" "$#"
      GO_TPC=$2
      shift 2
      ;;
    --analyzer)
      require_value "$1" "$#"
      ANALYZER=$2
      shift 2
      ;;
    --result-root)
      require_value "$1" "$#"
      RESULT_ROOT=$2
      shift 2
      ;;
    --baseline-worktree)
      require_value "$1" "$#"
      BASELINE_WORKTREE=$2
      shift 2
      ;;
    --tidb-cpus)
      require_value "$1" "$#"
      TIDB_CPUSET=$2
      shift 2
      ;;
    --client-cpus)
      require_value "$1" "$#"
      CLIENT_CPUSET=$2
      shift 2
      ;;
    --sql-host)
      require_value "$1" "$#"
      SQL_HOST=$2
      shift 2
      ;;
    --sql-port)
      require_value "$1" "$#"
      SQL_PORT=$2
      shift 2
      ;;
    --status-port)
      require_value "$1" "$#"
      STATUS_PORT=$2
      shift 2
      ;;
    --build-jobs)
      require_value "$1" "$#"
      BUILD_JOBS=$2
      shift 2
      ;;
    --monitor-interval)
      require_value "$1" "$#"
      MONITOR_INTERVAL_SECONDS=$2
      shift 2
      ;;
    --checker-drain-timeout)
      require_value "$1" "$#"
      CHECKER_DRAIN_TIMEOUT=$2
      shift 2
      ;;
    --smoke)
      SMOKE=true
      shift
      ;;
    --resume)
      RESUME=true
      shift
      ;;
    --baseline-full-only)
      BASELINE_FULL_ONLY=true
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      die "unknown argument: $1"
      ;;
  esac
done

if [[ "$BASELINE_FULL_ONLY" = true ]]; then
  MODES=(
    "baseline-original"
    "serialized-full"
  )
fi
readonly -a MODES
MODE_CSV=${MODES[0]}
for mode in "${MODES[@]:1}"; do
  MODE_CSV+=",$mode"
done
readonly MODE_CSV

[[ -n "$RESULT_ROOT" ]] || die "--result-root is required"
[[ "$RESULT_ROOT" = /* ]] || die "--result-root must be absolute"
CHECKER_ROOT=$(realpath "$CHECKER_ROOT")
TIDB_ROOT=$(realpath "$TIDB_ROOT")
ANALYZER=$(realpath "$ANALYZER")
RESULT_ROOT=$(realpath -m "$RESULT_ROOT")
BASELINE_WORKTREE=$(realpath -m "$BASELINE_WORKTREE")
[[ "$BASELINE_WORKTREE" = /tmp/* && "$BASELINE_WORKTREE" != /tmp ]] ||
  die "--baseline-worktree must resolve to a specific directory under /tmp"
for source_root in "$CHECKER_ROOT" "$TIDB_ROOT" "$BASELINE_WORKTREE"; do
  if [[ "$RESULT_ROOT" = "$source_root" || "$RESULT_ROOT" = "$source_root/"* ]]; then
    die "--result-root must be outside source and build worktrees: $source_root"
  fi
done

if [[ -z "$GO_TPC" ]]; then
  GO_TPC=$(command -v go-tpc 2>/dev/null || true)
  if [[ -z "$GO_TPC" && -x /home/dyf/go/bin/go-tpc ]]; then
    GO_TPC=/home/dyf/go/bin/go-tpc
  fi
fi
[[ -n "$GO_TPC" ]] || die "go-tpc was not found; pass --go-tpc"
GO_TPC=$(realpath "$GO_TPC")

validate_positive_integer() {
  local name=$1
  local value=$2
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || die "$name must be a positive integer"
}

validate_port() {
  local name=$1
  local value=$2
  validate_positive_integer "$name" "$value"
  (( value <= 65535 )) || die "$name must be at most 65535"
}

validate_duration() {
  local name=$1
  local value=$2
  python3 - "$name" "$value" <<'PY'
import re
import sys

name, value = sys.argv[1:]
pattern = re.compile(r"(?:[0-9]+(?:\.[0-9]+)?(?:ms|h|m|s))+")
if not pattern.fullmatch(value):
    raise SystemExit(f"{name} must be a positive Go-style duration, got {value!r}")
parts = re.findall(r"([0-9]+(?:\.[0-9]+)?)(ms|h|m|s)", value)
units = {"ms": 0.001, "s": 1.0, "m": 60.0, "h": 3600.0}
if sum(float(number) * units[unit] for number, unit in parts) <= 0:
    raise SystemExit(f"{name} must be positive")
PY
}

validate_transaction_weights() {
  python3 - "$1" <<'PY'
import sys

parts = sys.argv[1].split(",")
if len(parts) != 5 or any(not part.isdigit() for part in parts):
    raise SystemExit(
        "transaction weights must be five comma-separated nonnegative integers"
    )
weights = [int(part) for part in parts]
if sum(weights) != 100:
    raise SystemExit("transaction weights must sum to 100")
if weights[0] <= 0:
    raise SystemExit("the NewOrder transaction weight must be positive")
PY
}

for item in \
  "trials:$TRIALS" \
  "warehouses:$WAREHOUSES" \
  "threads:$THREADS" \
  "prepare threads:$PREPARE_THREADS" \
  "smoke warehouses:$SMOKE_WAREHOUSES" \
  "build jobs:$BUILD_JOBS" \
  "monitor interval:$MONITOR_INTERVAL_SECONDS" \
  "queue capacity:$QUEUE_CAPACITY_BYTES" \
  "capture capacity:$CAPTURE_CAPACITY_BYTES" \
  "retained-state capacity:$RETAINED_STATE_CAPACITY_BYTES" \
  "chunk segment bytes:$CHUNK_SEGMENT_BYTES" \
  "maximum cached chunk segments:$MAX_CACHED_CHUNK_SEGMENTS"
do
  validate_positive_integer "${item%%:*}" "${item#*:}"
done
[[ "$STABILIZE_SECONDS" =~ ^[0-9]+$ ]] ||
  die "stabilize seconds must be a nonnegative integer"
validate_port "SQL port" "$SQL_PORT"
validate_port "status port" "$STATUS_PORT"
(( SQL_PORT != STATUS_PORT )) || die "SQL and status ports must differ"
validate_duration "warmup time" "$WARMUP_TIME"
validate_duration "run time" "$RUN_TIME"
validate_duration "output interval" "$OUTPUT_INTERVAL"
validate_duration "checker drain timeout" "$CHECKER_DRAIN_TIMEOUT"
validate_transaction_weights "$TRANSACTION_WEIGHTS"
[[ "$DUPLICATE_READ_DIAGNOSTICS" = 0 || "$DUPLICATE_READ_DIAGNOSTICS" = 1 ]] ||
  die "TIDB_ONLINE_CHECKER_DUPLICATE_READ_DIAGNOSTICS must be 0 or 1"
if [[ "$TRANSACTION_WEIGHTS" != "$DEFAULT_TRANSACTION_WEIGHTS" && "$SMOKE" != true ]]; then
  die "non-default --transaction-weights require --smoke"
fi
(( TRIALS >= 5 )) || die "measured TPC-C comparison requires --trials at least 5"
validate_duration "queue timeout" "$QUEUE_TIMEOUT"
validate_duration "capture timeout" "$CAPTURE_TIMEOUT"
(( PREPARE_THREADS == 1 )) || die "TPC-C deterministic preparation requires --prepare-threads 1"

for command in \
  bash cmake cp curl df git go lscpu make ninja patch pidstat python3 \
  realpath rg sha256sum ss taskset uname /usr/bin/time
do
  command -v "$command" >/dev/null || die "required command not found: $command"
done
[[ -x "$GO_TPC" ]] || die "go-tpc is not executable: $GO_TPC"
[[ -f "$ANALYZER" ]] || die "analyzer not found: $ANALYZER"
[[ -f "$CHECKER_ROOT/online/CMakeLists.txt" ]] ||
  die "checker online CMake project not found under $CHECKER_ROOT"
[[ -f "$CHECKER_ROOT/online/tools/tidb_sql.go" ]] ||
  die "TiDB SQL helper source not found"
[[ -f "$CHECKER_ROOT/online/tools/tidb_sql_test.go" ]] ||
  die "TiDB SQL helper tests not found"
GO_TPC_PATCH="$CHECKER_ROOT/online/tools/go_tpc_v1.0.12_deterministic_prepare.patch"
[[ -f "$GO_TPC_PATCH" ]] || die "deterministic go-tpc patch not found: $GO_TPC_PATCH"
[[ -f "$TIDB_ROOT/AGENTS.md" && -f "$TIDB_ROOT/Makefile" ]] ||
  die "instrumented TiDB worktree not found: $TIDB_ROOT"
git -C "$TIDB_ROOT" cat-file -e "$PRISTINE_TIDB_COMMIT^{commit}" ||
  die "pristine TiDB commit is unavailable in $TIDB_ROOT"
grep -q 'serialized-global-lock' \
  "$TIDB_ROOT/pkg/store/driver/txn/captureadapter/config.go" ||
  die "TiDB worktree does not contain the serialized global-lock adapter"

derive_cpu_sets() {
  python3 - <<'PY'
import pathlib

allowed = None
for line in pathlib.Path("/proc/self/status").read_text().splitlines():
    if line.startswith("Cpus_allowed_list:"):
        allowed = line.split(":", 1)[1].strip()
        break
if not allowed:
    raise SystemExit("cannot read Cpus_allowed_list from /proc/self/status")

cpus = []
for part in allowed.split(","):
    if "-" in part:
        first, last = (int(value) for value in part.split("-", 1))
        cpus.extend(range(first, last + 1))
    else:
        cpus.append(int(part))
cpus = cpus[:16]
if len(cpus) == 1:
    left = right = cpus
else:
    split = max(1, len(cpus) // 2)
    left, right = cpus[:split], cpus[split:]
    if not right:
        right = left
print(",".join(str(cpu) for cpu in left))
print(",".join(str(cpu) for cpu in right))
PY
}

if [[ -z "$TIDB_CPUSET" || -z "$CLIENT_CPUSET" ]]; then
  mapfile -t DERIVED_CPUSETS < <(derive_cpu_sets)
  [[ -n "$TIDB_CPUSET" ]] || TIDB_CPUSET=${DERIVED_CPUSETS[0]}
  [[ -n "$CLIENT_CPUSET" ]] || CLIENT_CPUSET=${DERIVED_CPUSETS[1]}
fi
taskset -c "$TIDB_CPUSET" true ||
  die "TiDB CPU set is unavailable: $TIDB_CPUSET"
taskset -c "$CLIENT_CPUSET" true ||
  die "client CPU set is unavailable: $CLIENT_CPUSET"

readonly DSN_PARAMS="tidb_enable_async_commit=0&tidb_enable_1pc=0&tidb_txn_mode='optimistic'&tidb_low_resolution_tso=0&tidb_store_batch_size=0&tidb_allow_batch_cop=0&tidb_index_lookup_pushdown_policy='force'"

clear_online_checker_environment() {
  local name
  for name in \
    TIDB_ONLINE_CHECKER_MODE \
    TIDB_ONLINE_CHECKER_ORDERING_PROTOCOL \
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
    TIDB_ONLINE_CHECKER_FRONTIER_INTERVAL \
    TIDB_ONLINE_CHECKER_CHUNK_SEGMENT_BYTES \
    TIDB_ONLINE_CHECKER_MAX_CACHED_CHUNK_SEGMENTS \
    TIDB_ONLINE_CHECKER_DUPLICATE_READ_DIAGNOSTICS
  do
    unset "$name"
  done
}

monotonic_ns() {
  python3 -c 'import time; print(time.monotonic_ns())'
}

ensure_ports_free() {
  if [[ -n "$(ss -H -ltn "sport = :$SQL_PORT")" ]]; then
    die "SQL port $SQL_PORT is already listening"
  fi
  if [[ -n "$(ss -H -ltn "sport = :$STATUS_PORT")" ]]; then
    die "status port $STATUS_PORT is already listening"
  fi
}

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

git_diff_sha256() {
  git -C "$1" diff --binary --no-ext-diff HEAD | sha256sum | awk '{print $1}'
}

capture_source_state() {
  local repository=$1
  local name=$2
  local output_directory=$3
  local patch_path="$output_directory/$name-tracked.patch"
  local state_path="$output_directory/$name-source-state.json"
  local snapshot_path="$output_directory/$name-untracked"
  git -C "$repository" diff --binary --no-ext-diff HEAD >"$patch_path"
  python3 - "$repository" "$name" "$patch_path" "$snapshot_path" "$state_path" <<'PY'
import hashlib
import json
import os
import pathlib
import shutil
import stat
import subprocess
import sys

repository, source_name, patch_name, snapshot_name, state_name = sys.argv[1:]
root = pathlib.Path(repository).resolve()
patch_path = pathlib.Path(patch_name).resolve()
snapshot_path = pathlib.Path(snapshot_name).resolve()
state_path = pathlib.Path(state_name).resolve()


def sha_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git(*args, binary=False):
    return subprocess.check_output(
        ["git", "-C", str(root), *args],
        text=not binary,
    )


snapshot_path.mkdir(parents=True)
raw_paths = sorted(
    value
    for value in git(
        "ls-files", "--others", "--exclude-standard", "-z", binary=True
    ).split(b"\0")
    if value
)
if source_name == "checker":
    selection = "online/**"
    selected_paths = [value for value in raw_paths if os.fsdecode(value).startswith("online/")]
    excluded_paths = [os.fsdecode(value) for value in raw_paths if value not in selected_paths]
elif source_name == "instrumented-tidb":
    selection = "all-untracked"
    selected_paths = raw_paths
    excluded_paths = []
else:
    raise SystemExit(f"unknown source snapshot name: {source_name!r}")
entries = []
for raw_path in selected_paths:
    relative = os.fsdecode(raw_path)
    parts = pathlib.PurePosixPath(relative).parts
    if not parts or pathlib.PurePosixPath(relative).is_absolute() or ".." in parts:
        raise SystemExit(f"unsafe untracked source path: {relative!r}")
    source = root.joinpath(*parts)
    destination = snapshot_path.joinpath(*parts)
    source_stat = source.lstat()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if stat.S_ISLNK(source_stat.st_mode):
        target = os.readlink(source)
        content = os.fsencode(target)
        os.symlink(target, destination)
        kind = "symlink"
        size = len(content)
        digest = hashlib.sha256(content).hexdigest()
    elif stat.S_ISREG(source_stat.st_mode):
        shutil.copy2(source, destination, follow_symlinks=False)
        kind = "file"
        size = source_stat.st_size
        digest = sha_file(source)
    else:
        raise SystemExit(f"unsupported untracked source type: {relative!r}")
    entries.append({
        "path": relative,
        "type": kind,
        "mode": stat.S_IMODE(source_stat.st_mode),
        "size": size,
        "sha256": digest,
    })

payload = {
    "commit": git("rev-parse", "HEAD").strip(),
    "diff_sha256": sha_file(patch_path),
    "untracked_selection": selection,
    "untracked_files": entries,
}
canonical = json.dumps(
    payload, sort_keys=True, separators=(",", ":")
).encode("utf-8")
record = {
    "format_version": 1,
    **payload,
    "source_state_sha256": hashlib.sha256(canonical).hexdigest(),
    "tracked_patch": {
        "path": str(patch_path),
        "sha256": payload["diff_sha256"],
    },
    "untracked_snapshot_path": str(snapshot_path),
    "excluded_untracked_paths": excluded_paths,
}
state_path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
PY
}

next_numbered_directory() {
  local parent=$1
  local prefix=$2
  local maximum=0
  local path base number
  if [[ -d "$parent" ]]; then
    for path in "$parent"/"$prefix"-*; do
      [[ -d "$path" ]] || continue
      base=${path##*/}
      number=${base#"$prefix"-}
      if [[ "$number" =~ ^[0-9]+$ ]] && (( 10#$number > maximum )); then
        maximum=$((10#$number))
      fi
    done
  fi
  printf '%s-%03d\n' "$prefix" "$((maximum + 1))"
}

if [[ -e "$RESULT_ROOT" && ! -d "$RESULT_ROOT" ]]; then
  die "result root exists and is not a directory: $RESULT_ROOT"
fi
if [[ -d "$RESULT_ROOT" && "$RESUME" != true ]] &&
  [[ -n "$(find "$RESULT_ROOT" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
  die "result root is not empty; use --resume only for an exact existing experiment"
fi
mkdir -p "$RESULT_ROOT"

ensure_baseline_worktree() {
  if [[ -e "$BASELINE_WORKTREE" ]]; then
    git -C "$BASELINE_WORKTREE" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
      die "baseline path exists but is not a Git worktree: $BASELINE_WORKTREE"
  else
    echo "Creating detached pristine TiDB worktree: $BASELINE_WORKTREE"
    git -C "$TIDB_ROOT" worktree add --detach \
      "$BASELINE_WORKTREE" "$PRISTINE_TIDB_COMMIT"
  fi
  local head
  head=$(git -C "$BASELINE_WORKTREE" rev-parse HEAD)
  [[ "$head" = "$PRISTINE_TIDB_COMMIT" ]] ||
    die "baseline worktree is at $head, expected $PRISTINE_TIDB_COMMIT"
  [[ -z "$(git -C "$BASELINE_WORKTREE" status --short --untracked-files=all)" ]] ||
    die "baseline worktree is not pristine; choose another /tmp worktree"
}

ARTIFACT_INDEX="$RESULT_ROOT/build-artifacts.json"

verify_existing_artifacts() {
  python3 - \
    "$ARTIFACT_INDEX" \
    "$CHECKER_ROOT" \
    "$TIDB_ROOT" \
    "$SCRIPT_PATH" \
    "$ANALYZER" \
    "$GO_TPC" \
    "$GO_TPC_PATCH" <<'PY'
import hashlib
import json
import os
import pathlib
import stat
import subprocess
import sys

index_path, checker, tidb, runner, analyzer, go_tpc, go_tpc_patch = sys.argv[1:]
index = json.loads(pathlib.Path(index_path).read_text())

def sha(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def git(repo, *args, binary=False):
    return subprocess.check_output(
        ["git", "-C", repo, *args],
        text=not binary,
    )

def source_state(repo, source_name):
    diff = git(
        repo, "diff", "--binary", "--no-ext-diff", "HEAD", binary=True
    )
    raw_paths = sorted(
        value
        for value in git(
            repo, "ls-files", "--others", "--exclude-standard", "-z", binary=True
        ).split(b"\0")
        if value
    )
    if source_name == "checker":
        selection = "online/**"
        selected_paths = [value for value in raw_paths if os.fsdecode(value).startswith("online/")]
    elif source_name == "instrumented-tidb":
        selection = "all-untracked"
        selected_paths = raw_paths
    else:
        raise RuntimeError(f"unknown source fingerprint name: {source_name!r}")
    entries = []
    root = pathlib.Path(repo).resolve()
    for raw_path in selected_paths:
        relative = os.fsdecode(raw_path)
        parts = pathlib.PurePosixPath(relative).parts
        source = root.joinpath(*parts)
        source_stat = source.lstat()
        if stat.S_ISLNK(source_stat.st_mode):
            content = os.fsencode(os.readlink(source))
            kind = "symlink"
            size = len(content)
            digest = hashlib.sha256(content).hexdigest()
        elif stat.S_ISREG(source_stat.st_mode):
            kind = "file"
            size = source_stat.st_size
            digest = sha(source)
        else:
            raise RuntimeError(f"unsupported untracked source type: {relative!r}")
        entries.append({
            "path": relative,
            "type": kind,
            "mode": stat.S_IMODE(source_stat.st_mode),
            "size": size,
            "sha256": digest,
        })
    payload = {
        "commit": git(repo, "rev-parse", "HEAD").strip(),
        "diff_sha256": hashlib.sha256(diff).hexdigest(),
        "untracked_selection": selection,
        "untracked_files": entries,
    }
    canonical = json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    payload["source_state_sha256"] = hashlib.sha256(canonical).hexdigest()
    return payload


def verify_source_snapshot(name, expected):
    if not isinstance(expected, dict) or expected.get("format_version") != 1:
        errors.append(f"{name} source fingerprint is missing or obsolete")
        return
    patch = expected.get("tracked_patch")
    if not isinstance(patch, dict):
        errors.append(f"{name} tracked source patch is missing")
    else:
        patch_path = pathlib.Path(str(patch.get("path", "")))
        if not patch_path.is_file() or sha(patch_path) != patch.get("sha256"):
            errors.append(f"{name} tracked source patch changed or is missing")
    snapshot_text = expected.get("untracked_snapshot_path")
    snapshot = pathlib.Path(snapshot_text) if isinstance(snapshot_text, str) else None
    for entry in expected.get("untracked_files", []):
        if snapshot is None or not isinstance(entry, dict):
            errors.append(f"{name} untracked source snapshot is malformed")
            break
        candidate = snapshot.joinpath(*pathlib.PurePosixPath(entry.get("path", "")).parts)
        try:
            candidate_stat = candidate.lstat()
            if entry.get("type") == "symlink" and stat.S_ISLNK(candidate_stat.st_mode):
                content = os.fsencode(os.readlink(candidate))
                digest = hashlib.sha256(content).hexdigest()
                size = len(content)
            elif entry.get("type") == "file" and stat.S_ISREG(candidate_stat.st_mode):
                digest = sha(candidate)
                size = candidate_stat.st_size
            else:
                raise OSError("type mismatch")
        except OSError:
            errors.append(f"{name} untracked snapshot entry is missing: {entry.get('path')}")
            continue
        if (
            digest != entry.get("sha256")
            or size != entry.get("size")
            or stat.S_IMODE(candidate_stat.st_mode) != entry.get("mode")
        ):
            errors.append(f"{name} untracked snapshot entry changed: {entry.get('path')}")


errors = []
inputs = index["inputs"]
for name, path in (("runner", runner), ("analyzer", analyzer), ("go_tpc_original", go_tpc), ("go_tpc_patch", go_tpc_patch)):
    got = sha(path)
    if got != inputs[name]["sha256"]:
        errors.append(f"{name} changed: {got} != {inputs[name]['sha256']}")
for name, repo, source_name in (
    ("checker", checker, "checker"),
    ("instrumented_tidb", tidb, "instrumented-tidb"),
):
    expected = inputs.get(name)
    current = source_state(repo, source_name)
    if not isinstance(expected, dict) or any(
        current.get(field) != expected.get(field)
        for field in ("commit", "diff_sha256", "source_state_sha256")
    ):
        errors.append(
            f"{name} source changed: commit={current['commit']}, "
            f"diff_sha256={current['diff_sha256']}, "
            f"source_state_sha256={current['source_state_sha256']}"
        )
    else:
        verify_source_snapshot(name, expected)
for name, artifact in index["artifacts"].items():
    path = pathlib.Path(artifact["path"])
    if not path.is_file():
        errors.append(f"{name} artifact is missing: {path}")
    elif sha(path) != artifact["sha256"]:
        errors.append(f"{name} artifact hash mismatch: {path}")
if errors:
    raise SystemExit("cannot resume with a different manifest:\n  " + "\n  ".join(errors))
PY
}

build_artifacts() {
  if [[ -s "$ARTIFACT_INDEX" ]]; then
    [[ "$RESUME" = true ]] ||
      die "build artifact index already exists without --resume"
    verify_existing_artifacts
    return
  fi

  ensure_baseline_worktree
  mkdir -p "$RESULT_ROOT/artifacts"
  local build_name build_dir
  build_name=$(next_numbered_directory "$RESULT_ROOT/artifacts" "build-attempt")
  build_dir="$RESULT_ROOT/artifacts/$build_name"
  mkdir -p "$build_dir"

  local checker_commit checker_diff checker_state tidb_commit tidb_diff tidb_state
  checker_commit=$(git -C "$CHECKER_ROOT" rev-parse HEAD)
  checker_diff=$(git_diff_sha256 "$CHECKER_ROOT")
  tidb_commit=$(git -C "$TIDB_ROOT" rev-parse HEAD)
  tidb_diff=$(git_diff_sha256 "$TIDB_ROOT")
  capture_source_state "$CHECKER_ROOT" checker "$build_dir"
  capture_source_state "$TIDB_ROOT" instrumented-tidb "$build_dir"
  checker_state=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["source_state_sha256"])' "$build_dir/checker-source-state.json")
  tidb_state=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["source_state_sha256"])' "$build_dir/instrumented-tidb-source-state.json")

  {
    echo "checker_commit=$checker_commit"
    echo "checker_diff_sha256=$checker_diff"
    echo "checker_source_state_sha256=$checker_state"
    echo "instrumented_tidb_commit=$tidb_commit"
    echo "instrumented_tidb_diff_sha256=$tidb_diff"
    echo "instrumented_tidb_source_state_sha256=$tidb_state"
    echo "pristine_tidb_commit=$PRISTINE_TIDB_COMMIT"
    echo "checker_build=cmake -S online -B <result-build> -G Ninja -DCMAKE_BUILD_TYPE=Release"
    echo "tidb_build=make server TARGET=<absolute-output>"
    go version
    go env GOOS GOARCH GOVERSION CGO_ENABLED CC CXX CGO_CFLAGS CGO_CPPFLAGS CGO_CXXFLAGS CGO_LDFLAGS
    cmake --version
    ninja --version
    "${CC:-cc}" --version | head -n 1
  } >"$build_dir/build-settings.txt"
  git -C "$CHECKER_ROOT" status --short --untracked-files=all \
    >"$build_dir/checker-status.txt"
  git -C "$TIDB_ROOT" status --short --untracked-files=all \
    >"$build_dir/instrumented-tidb-status.txt"
  git -C "$BASELINE_WORKTREE" status --short --untracked-files=all \
    >"$build_dir/pristine-tidb-status.txt"

  echo "Building checker native library (Release)"
  cmake -S "$CHECKER_ROOT/online" -B "$build_dir/checker-release" \
    -G Ninja -DCMAKE_BUILD_TYPE=Release \
    >"$build_dir/checker-configure.txt" 2>&1
  cmake --build "$build_dir/checker-release" --target co_checker_online_c \
    -j "$BUILD_JOBS" >"$build_dir/checker-build.txt" 2>&1
  cp "$build_dir/checker-release/libco_checker_online_c.so" \
    "$build_dir/libco_checker_online_c.so"

  echo "Building pristine TiDB at $PRISTINE_TIDB_COMMIT"
  make -C "$BASELINE_WORKTREE" server \
    TARGET="$build_dir/tidb-server-pristine" \
    >"$build_dir/pristine-tidb-build.txt" 2>&1

  echo "Building instrumented TiDB at $tidb_commit"
  make -C "$TIDB_ROOT" server \
    TARGET="$build_dir/tidb-server-instrumented" \
    >"$build_dir/instrumented-tidb-build.txt" 2>&1

  echo "Building SQL validation helper"
  (
    cd "$TIDB_ROOT"
    go test \
      "$CHECKER_ROOT/online/tools/tidb_sql.go" \
      "$CHECKER_ROOT/online/tools/tidb_sql_test.go"
    go build -trimpath -o "$build_dir/tidb-sql" \
      "$CHECKER_ROOT/online/tools/tidb_sql.go"
  ) >"$build_dir/tidb-sql-build.txt" 2>&1

  local go_tpc_module
  go_tpc_module="$(go env GOMODCACHE)/github.com/pingcap/go-tpc@$GO_TPC_VERSION"
  [[ -d "$go_tpc_module" ]] ||
    die "cached github.com/pingcap/go-tpc@$GO_TPC_VERSION source is required"
  echo "Building deterministic go-tpc $GO_TPC_VERSION"
  cp -a "$go_tpc_module" "$build_dir/go-tpc-source"
  chmod -R u+w "$build_dir/go-tpc-source"
  patch -d "$build_dir/go-tpc-source" -p1 --forward --batch \
    <"$GO_TPC_PATCH" >"$build_dir/go-tpc-patch.txt" 2>&1
  (
    cd "$build_dir/go-tpc-source"
    go test ./pkg/sink ./pkg/workload ./tpcc ./cmd/go-tpc
    go build -trimpath -o "$build_dir/go-tpc" ./cmd/go-tpc
  ) >"$build_dir/go-tpc-build.txt" 2>&1
  "$build_dir/go-tpc" --ignore-error=false --silence=false tpcc prepare --help >"$build_dir/go-tpc-prepare-help.txt"
  "$build_dir/go-tpc" --ignore-error=false --silence=false tpcc run --help >"$build_dir/go-tpc-run-help.txt"
  rg -q -- "--ddl-only" "$build_dir/go-tpc-prepare-help.txt"
  rg -q -- "--manifest" "$build_dir/go-tpc-prepare-help.txt"
  rg -q -- "--continue-on-write-conflict" "$build_dir/go-tpc-run-help.txt"
  rg -q -- "--seed" "$build_dir/go-tpc-run-help.txt"
  go version -m "$GO_TPC" >"$build_dir/go-tpc-original-version.txt"
  go version -m "$build_dir/go-tpc" >"$build_dir/go-tpc-built-version.txt"

  cp "$GO_TPC_PATCH" "$build_dir/go-tpc-deterministic.patch"
  cp "$SCRIPT_PATH" "$build_dir/run_tpcc_global_lock_comparison.sh"
  cp "$ANALYZER" "$build_dir/analyze_tpcc_global_lock.py"
  chmod 0555 \
    "$build_dir/libco_checker_online_c.so" \
    "$build_dir/tidb-server-pristine" \
    "$build_dir/tidb-server-instrumented" \
    "$build_dir/tidb-sql" \
    "$build_dir/go-tpc" \
    "$build_dir/run_tpcc_global_lock_comparison.sh" \
    "$build_dir/analyze_tpcc_global_lock.py"
  chmod 0444 "$build_dir/go-tpc-deterministic.patch"

  python3 - \
    "$build_dir" \
    "$checker_commit" \
    "$checker_diff" \
    "$tidb_commit" \
    "$tidb_diff" \
    "$build_dir/checker-source-state.json" \
    "$build_dir/instrumented-tidb-source-state.json" \
    "$PRISTINE_TIDB_COMMIT" \
    "$SCRIPT_PATH" \
    "$ANALYZER" \
    "$GO_TPC" \
    "$GO_TPC_PATCH" \
    "$GO_TPC_VERSION" >"$build_dir/manifest.json" <<'PY'
import hashlib
import json
import pathlib
import sys

(
    build_dir,
    checker_commit,
    checker_diff,
    tidb_commit,
    tidb_diff,
    checker_source_state_path,
    tidb_source_state_path,
    pristine_commit,
    runner_source,
    analyzer_source,
    go_tpc_source,
    go_tpc_patch,
    go_tpc_version,
) = sys.argv[1:]
build = pathlib.Path(build_dir).resolve()
checker_source_state = json.loads(pathlib.Path(checker_source_state_path).read_text())
tidb_source_state = json.loads(pathlib.Path(tidb_source_state_path).read_text())
if (
    checker_source_state.get("commit") != checker_commit
    or checker_source_state.get("diff_sha256") != checker_diff
    or tidb_source_state.get("commit") != tidb_commit
    or tidb_source_state.get("diff_sha256") != tidb_diff
):
    raise SystemExit("captured source state changed during artifact setup")

def sha(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

artifact_names = {
    "native_library": "libco_checker_online_c.so",
    "pristine_tidb_server": "tidb-server-pristine",
    "instrumented_tidb_server": "tidb-server-instrumented",
    "tidb_sql": "tidb-sql",
    "go_tpc": "go-tpc",
    "go_tpc_patch": "go-tpc-deterministic.patch",
    "runner": "run_tpcc_global_lock_comparison.sh",
    "analyzer": "analyze_tpcc_global_lock.py",
}
manifest = {
    "format_version": 2,
    "build_type": "Release",
    "tidb_build_command": "make server TARGET=<absolute-output>",
    "inputs": {
        "checker": checker_source_state,
        "instrumented_tidb": tidb_source_state,
        "pristine_tidb": {
            "commit": pristine_commit,
            "diff_sha256": hashlib.sha256(b"").hexdigest(),
        },
        "runner": {
            "path": str(pathlib.Path(runner_source).resolve()),
            "sha256": sha(runner_source),
        },
        "analyzer": {
            "path": str(pathlib.Path(analyzer_source).resolve()),
            "sha256": sha(analyzer_source),
        },
        "go_tpc_original": {
            "path": str(pathlib.Path(go_tpc_source).resolve()),
            "sha256": sha(go_tpc_source),
        },
        "go_tpc_patch": {
            "path": str(pathlib.Path(go_tpc_patch).resolve()),
            "sha256": sha(go_tpc_patch),
            "base_module": "github.com/pingcap/go-tpc@" + go_tpc_version,
        },
    },
    "artifacts": {
        name: {
            "path": str(build / filename),
            "sha256": sha(build / filename),
        }
        for name, filename in artifact_names.items()
    },
    "build_settings_path": str(build / "build-settings.txt"),
}
print(json.dumps(manifest, indent=2, sort_keys=True))
PY
  cp "$build_dir/manifest.json" "$ARTIFACT_INDEX"
  chmod 0444 "$build_dir/manifest.json" "$ARTIFACT_INDEX"
}

load_artifact_paths() {
  mapfile -t ARTIFACT_VALUES < <(
    python3 - "$ARTIFACT_INDEX" <<'PY'
import json
import pathlib
import sys

data = json.loads(pathlib.Path(sys.argv[1]).read_text())
for name in (
    "native_library",
    "pristine_tidb_server",
    "instrumented_tidb_server",
    "tidb_sql",
    "go_tpc",
    "analyzer",
):
    artifact = data["artifacts"][name]
    print(artifact["path"])
    print(artifact["sha256"])
print(data["inputs"]["instrumented_tidb"]["commit"])
PY
  )
  NATIVE_LIBRARY=${ARTIFACT_VALUES[0]}
  NATIVE_LIBRARY_SHA=${ARTIFACT_VALUES[1]}
  PRISTINE_TIDB_SERVER=${ARTIFACT_VALUES[2]}
  PRISTINE_TIDB_SERVER_SHA=${ARTIFACT_VALUES[3]}
  INSTRUMENTED_TIDB_SERVER=${ARTIFACT_VALUES[4]}
  INSTRUMENTED_TIDB_SERVER_SHA=${ARTIFACT_VALUES[5]}
  TIDB_SQL=${ARTIFACT_VALUES[6]}
  TIDB_SQL_SHA=${ARTIFACT_VALUES[7]}
  IMMUTABLE_GO_TPC=${ARTIFACT_VALUES[8]}
  IMMUTABLE_GO_TPC_SHA=${ARTIFACT_VALUES[9]}
  IMMUTABLE_ANALYZER=${ARTIFACT_VALUES[10]}
  IMMUTABLE_ANALYZER_SHA=${ARTIFACT_VALUES[11]}
  INSTRUMENTED_TIDB_COMMIT=${ARTIFACT_VALUES[12]}
}

wait_for_server_pid() {
  local pid=$1
  local expected_commit=$2
  local output=$3
  local count
  for count in $(seq 1 180); do
    if ! kill -0 "$pid" 2>/dev/null; then
      return 1
    fi
    if curl --fail --silent "http://$SQL_HOST:$STATUS_PORT/status" >"$output"; then
      python3 - "$output" "$expected_commit" <<'PY'
import json
import pathlib
import sys

status = json.loads(pathlib.Path(sys.argv[1]).read_text())
if status.get("git_hash") != sys.argv[2]:
    raise SystemExit(
        f"unexpected TiDB identity {status.get('git_hash')!r}; "
        f"expected {sys.argv[2]!r}"
    )
PY
      return 0
    fi
    sleep 1
  done
  return 1
}

validate_server_logs() {
  local mode=$1
  local attempt_directory=$2
  local output=$3
  python3 - "$mode" "$attempt_directory" "$output" <<'PY'
import json
import pathlib
import re
import sys

mode, attempt_name, output_name = sys.argv[1:]
attempt = pathlib.Path(attempt_name)
output = pathlib.Path(output_name)
expected_names = (
    "schema-bootstrap-server.txt",
    "schema-bootstrap-server-stdout.txt",
    "main-server.txt",
    "main-server-stdout.txt",
    "post-check-server.txt",
    "post-check-server-stdout.txt",
)
patterns = (
    ("go_panic", re.compile(r"(?:^|\s)panic:\s", re.IGNORECASE)),
    ("go_runtime_fatal", re.compile(r"fatal error:\s", re.IGNORECASE)),
    ("tidb_fatal_level", re.compile(r"(?:\[(?:FATAL|DPANIC)\]|[\"']level[\"']\s*:\s*[\"'](?:fatal|dpanic)[\"'])", re.IGNORECASE)),
    ("checker_start_failure", re.compile(r"failed to start online transaction checker", re.IGNORECASE)),
    ("checker_frontier_failure", re.compile(r"online transaction checker frontier stopped with an error", re.IGNORECASE)),
    ("checker_close_failure", re.compile(r"online transaction checker did not close cleanly", re.IGNORECASE)),
)
files = []
matches = []
reasons = []
for name in expected_names:
    path = attempt / name
    try:
        text = path.read_text(errors="replace")
    except OSError as error:
        reasons.append(f"missing server log {name}: {error}")
        continue
    lines = text.splitlines()
    files.append({"path": name, "bytes": path.stat().st_size, "lines": len(lines)})
    for line_number, line in enumerate(lines, 1):
        for category, pattern in patterns:
            if pattern.search(line):
                matches.append({
                    "path": name,
                    "line": line_number,
                    "category": category,
                    "text": line[:1000],
                })
                break
if matches:
    reasons.append(f"server logs contain {len(matches)} fatal/checker failure marker(s)")
record = {
    "format_version": 1,
    "mode": mode,
    "passed": not reasons,
    "reason": "; ".join(reasons),
    "files": files,
    "failure_matches": matches,
}
output.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
if reasons:
    raise SystemExit("server-log validation failed: " + "; ".join(reasons))
PY
}

validate_prepare_output() {
  local command_status=$1
  local input=$2
  local output=$3
  python3 - "$command_status" "$input" "$output" <<'PY'
import json
import pathlib
import sys

status = int(sys.argv[1])
text = pathlib.Path(sys.argv[2]).read_text(errors="replace")
finished = sum(line.strip() == "Finished" for line in text.splitlines())
markers = [
    marker
    for marker in (
        "check prepare failed",
        "a fatal occurred when preparing data",
        "failed to init work loader",
        "panic:",
        "fatal error",
        "execute failed",
    )
    if marker in text.lower()
]
passed = status == 0 and finished == 1 and not markers
result = {
    "passed": passed,
    "exit_status": status,
    "finished_marker_count": finished,
    "failure_markers": markers,
}
pathlib.Path(sys.argv[3]).write_text(
    json.dumps(result, indent=2, sort_keys=True) + "\n"
)
if not passed:
    raise SystemExit("go-tpc prepare validation failed")
PY
}

validate_and_compare_preparation() {
  local mode=$1
  local manifest_path=$2
  local digest_path=$3
  local bootstrap_digest_path=$4
  local output_path=$5
  local reference_path="$ACTIVE_SUITE_ROOT/preparation-reference.json"
  python3 - \
    "$mode" "$manifest_path" "$digest_path" "$bootstrap_digest_path" \
    "$output_path" "$reference_path" "$PREPARE_SEED" "$PREPARE_LOAD_TIME" <<'PY'
import hashlib
import json
import os
import pathlib
import sys

mode, manifest_name, digest_name, bootstrap_name, output_name, reference_name, seed, load_time = sys.argv[1:]
manifest_path = pathlib.Path(manifest_name)
digest_path = pathlib.Path(digest_name)
bootstrap_path = pathlib.Path(bootstrap_name)
output_path = pathlib.Path(output_name)
reference_path = pathlib.Path(reference_name)
reasons = []

def load(path, description):
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        reasons.append(f"{description} is unavailable or malformed: {error}")
        return {}

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def sha256_text(value):
    return isinstance(value, str) and len(value) == 64 and all(character in "0123456789abcdef" for character in value)

manifest = load(manifest_path, "preparation manifest")
digest = load(digest_path, "post-prepare digest")
bootstrap = load(bootstrap_path, "schema bootstrap digest")
transactions = manifest.get("transactions")
if manifest.get("format_version") != 1:
    reasons.append("unexpected preparation manifest format")
if manifest.get("seed") != int(seed):
    reasons.append("preparation seed mismatch")
if manifest.get("load_time") != load_time:
    reasons.append("preparation load time mismatch")
if manifest.get("worker_count") != 1:
    reasons.append("preparation was not single-worker")
if not isinstance(transactions, list) or not transactions:
    reasons.append("preparation transaction list is empty")
    transactions = []
if manifest.get("transaction_count") != len(transactions):
    reasons.append("preparation transaction count does not match records")
if [record.get("sequence") for record in transactions] != list(range(1, len(transactions) + 1)):
    reasons.append("preparation transaction sequence is not dense and ordered")
logical_writes = sum(record.get("logical_write_count", 0) for record in transactions if isinstance(record, dict))
if logical_writes <= 0 or manifest.get("logical_write_count") != logical_writes:
    reasons.append("preparation logical write count does not balance")
for record in transactions:
    if not isinstance(record, dict) or record.get("logical_write_count", 0) <= 0 or record.get("sql_bytes", 0) <= 0 or not sha256_text(record.get("sql_sha256")):
        reasons.append("preparation transaction record is malformed")
        break
if not sha256_text(manifest.get("ordered_manifest_sha256")):
    reasons.append("ordered preparation manifest hash is malformed")

expected_tables = {"customer", "district", "history", "item", "new_order", "order_line", "orders", "stock", "warehouse"}
for name, record, expect_empty in (("post-prepare", digest, False), ("schema-bootstrap", bootstrap, True)):
    if record.get("format_version") != 2:
        reasons.append(f"{name} digest format is not 2")
    for field in ("database_digest_sha256", "schema_digest_sha256", "data_digest_sha256"):
        if not sha256_text(record.get(field)):
            reasons.append(f"{name} {field} is malformed")
    counts = record.get("table_row_counts")
    if not isinstance(counts, dict) or set(counts) != expected_tables:
        reasons.append(f"{name} table row-count coverage is incomplete")
        counts = {}
    total = sum(value for value in counts.values() if isinstance(value, int))
    if record.get("total_rows") != total:
        reasons.append(f"{name} total row count does not balance")
    if expect_empty and total != 0:
        reasons.append("schema bootstrap contains user rows")
    if not expect_empty and total <= 0:
        reasons.append("post-prepare database has no rows")
if digest.get("schema_digest_sha256") != bootstrap.get("schema_digest_sha256"):
    reasons.append("schema changed between DDL bootstrap and deterministic data preparation")

observed = {
    "format_version": 1,
    "prepare_seed": int(seed),
    "prepare_load_time": load_time,
    "worker_count": manifest.get("worker_count"),
    "manifest_file_sha256": sha(manifest_path) if manifest_path.is_file() else None,
    "ordered_manifest_sha256": manifest.get("ordered_manifest_sha256"),
    "transaction_count": manifest.get("transaction_count"),
    "logical_write_count": manifest.get("logical_write_count"),
    "schema_digest_sha256": digest.get("schema_digest_sha256"),
    "data_digest_sha256": digest.get("data_digest_sha256"),
    "database_digest_sha256": digest.get("database_digest_sha256"),
    "total_rows": digest.get("total_rows"),
    "table_row_counts": digest.get("table_row_counts"),
}
reference = None
if not reasons:
    if reference_path.exists():
        reference = load(reference_path, "preparation reference")
    elif mode == "baseline-original":
        reference = observed
        reference_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = reference_path.with_name(reference_path.name + f".tmp-{os.getpid()}")
        temporary.write_text(json.dumps(reference, indent=2, sort_keys=True) + "\n")
        try:
            os.link(temporary, reference_path)
        except FileExistsError:
            pass
        finally:
            temporary.unlink(missing_ok=True)
        reference = load(reference_path, "preparation reference")
    else:
        reasons.append("baseline preparation reference is missing")
if reference is not None and observed != reference:
    reasons.append("preparation manifest/schema/data digest differs from baseline reference")
result = {
    "passed": not reasons,
    "reason": "; ".join(reasons),
    "mode": mode,
    "reference_path": str(reference_path.resolve()),
    "observed": observed,
    "reference": reference,
}
output_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
if reasons:
    raise SystemExit("preparation validation failed: " + "; ".join(reasons))
PY
}

wait_for_online_checker_drain() {
  local runtime_mode=$1
  local output_path=$2
  local raw_output=$3
  local cutoff_path=${4:-}
  if [[ -z "$runtime_mode" ]]; then
    python3 - "$output_path" "$cutoff_path" <<'PY'
import json, pathlib, sys, time
output, cutoff = sys.argv[1:]
cutoff_record = json.loads(pathlib.Path(cutoff).read_text()) if cutoff else {}
now = time.monotonic_ns()
record = {
    "format_version": 2, "passed": True, "drained": True,
    "mode": "baseline-original", "checker": "disabled", "clock": "CLOCK_MONOTONIC",
    "workload_cutoff_monotonic_ns": cutoff_record.get("workload_cutoff_monotonic_ns"),
    "drain_started_monotonic_ns": now, "initial_scrape_monotonic_ns": now,
    "drain_complete_monotonic_ns": now, "live_prefix_verdict_monotonic_ns": None,
    "cutoff_observation_delay_ns": cutoff_record.get("observation_delay_ns"),
    "scrapes": 0, "scrape_attempts": 0, "scrape_failures": 0,
    "initial_adapter_metrics": {}, "initial_native_metrics": {}, "initial_verdict": None,
    "adapter_metrics": {}, "native_metrics": {}, "verdict": None,
    "cutoff_residual_transactions": None, "cutoff_residual_events": None,
    "drained_transactions": None, "drained_events": None,
    "drain_duration_seconds": 0.0, "live_scrape_success": True, "reason": "",
}
pathlib.Path(output).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
PY
    : >"$raw_output"
    return
  fi
  python3 - \
    "$runtime_mode" "http://$SQL_HOST:$STATUS_PORT/metrics" \
    "$output_path" "$raw_output" "$CHECKER_DRAIN_TIMEOUT" "$cutoff_path" <<'PY'
import json
import pathlib
import re
import sys
import time
import urllib.request

mode, url, output_name, raw_name, timeout_text, cutoff_name = sys.argv[1:]
output_path = pathlib.Path(output_name)
raw_path = pathlib.Path(raw_name)

def duration_seconds(value):
    units = {"ms": .001, "s": 1., "m": 60., "h": 3600.}
    parts = re.findall(r"([0-9]+(?:\.[0-9]+)?)(ms|h|m|s)", value)
    if not parts or "".join(number + unit for number, unit in parts) != value:
        raise ValueError(f"invalid duration {value!r}")
    return sum(float(number) * units[unit] for number, unit in parts)

def residual_transactions(native):
    accepted = native.get("accepted_publications")
    target = native.get({
        "capture-only": "recycled_transactions",
        "order-only": "ordered_transactions",
        "full": "checked_transactions",
    }[mode])
    return None if accepted is None or target is None else accepted - target

def residual_events(native):
    if mode != "full":
        return None
    accepted, checked = native.get("accepted_events"), native.get("checked_events")
    return None if accepted is None or checked is None else accepted - checked

drain_started_ns = time.monotonic_ns()
deadline = time.monotonic() + duration_seconds(timeout_text)
cutoff = json.loads(pathlib.Path(cutoff_name).read_text()) if cutoff_name else None
initial_adapter = dict(cutoff.get("adapter_metrics", {})) if cutoff else None
initial_native = dict(cutoff.get("native_metrics", {})) if cutoff else None
initial_verdict = dict(cutoff.get("verdict", {})) if cutoff and cutoff.get("verdict") else None
initial_scrape_ns = cutoff.get("observation_monotonic_ns") if cutoff else None
scrapes = scrape_failures = 0
last_text = ""
last_reasons = ["metrics have not been scraped"]
last_adapter, last_native, last_verdict = {}, {}, {}
complete_ns = None
sample_pattern = re.compile(r"^(tidb_online_checker_(?:adapter|native|run_info))\{([^}]*)\}\s+([-+0-9.eE]+)$")
verdict_pattern = re.compile(r"^tidb_online_checker_verdict_(available|pass|violation)\s+([-+0-9.eE]+)$")
label_pattern = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)="((?:\\.|[^"\\])*)"')

def parse(text):
    adapter, native, run_info, verdict = {}, {}, [], {}
    live_scrape_success = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped == "tidb_online_checker_live_scrape_success 1":
            live_scrape_success = True
            continue
        match = verdict_pattern.match(stripped)
        if match:
            name, value = match.groups()
            verdict[name] = float(value)
            continue
        match = sample_pattern.match(stripped)
        if not match:
            continue
        family, label_text, value = match.groups()
        labels = {name: bytes(value, "utf-8").decode("unicode_escape")
                  for name, value in label_pattern.findall(label_text)}
        value = float(value)
        if family.endswith("adapter") and "metric" in labels:
            adapter[labels["metric"]] = value
        elif family.endswith("native") and "metric" in labels:
            native[labels["metric"]] = value
        elif family.endswith("run_info"):
            run_info.append((labels, value))
    if verdict:
        native.update({f"verdict_{name}": value for name, value in verdict.items()})
    return adapter, native, run_info, verdict, live_scrape_success

def evaluate(adapter, native, run_info, verdict, live_success):
    reasons = []
    if not live_success:
        reasons.append("online-checker live scrape did not succeed")
    if not any(labels.get("mode") == mode and
               labels.get("ordering_protocol") == "serialized-global-lock" and value == 1
               for labels, value in run_info):
        reasons.append("online-checker run identity metric is missing")
    for field in ("active_attempts", "active_finalizers", "active_fragments",
                  "fragment_buffer_active_bytes", "fragment_retained_active_bytes"):
        if adapter.get(field) != 0:
            reasons.append(f"adapter {field}={adapter.get(field)!r}, want 0")
    for field in ("active_attempts", "queue_reserved_bytes", "queue_reserved_depth",
                  "queue_bytes", "queue_depth", "reorder_bytes"):
        if native.get(field) != 0:
            reasons.append(f"native {field}={native.get(field)!r}, want 0")
    if adapter.get("fatal_calls") != 0 or adapter.get("fragment_capture_errors") != 0:
        reasons.append("adapter fatal/capture error metric is nonzero or missing")
    if native.get("fatal") != 0:
        reasons.append("native fatal is nonzero or missing")
    started = adapter.get("fragments_started")
    if (started is None or started < 0 or
        started != adapter.get("fragments_accepted", -1) + adapter.get("fragments_discarded", -1)):
        reasons.append("fragment lifecycle counters do not balance")
    registered, published, aborted = (adapter.get(name) for name in
        ("attempts_registered", "attempts_published", "attempts_aborted"))
    if registered is None or registered <= 0 or published is None or aborted is None or registered != published + aborted:
        reasons.append("attempt lifecycle did not advance and balance")
    accepted = adapter.get("accepted_publications")
    native_accepted = native.get("accepted_publications")
    if (accepted is None or accepted <= 0 or accepted != published or
        adapter.get("publication_sequence") != accepted or
        adapter.get("successful_write_commits", -1) + adapter.get("read_only_no_write_completions", -1) != accepted or
        native_accepted != accepted):
        reasons.append("publication lifecycle did not advance and balance")
    if native.get("captured_transactions", 0) <= 0 or native.get("captured_events", 0) <= 0:
        reasons.append("native capture lifecycle did not advance")
    if mode == "capture-only" and native.get("recycled_transactions") != native_accepted:
        reasons.append("capture recycling lifecycle did not balance")
    if mode in ("order-only", "full") and native.get("ordered_transactions") != native_accepted:
        reasons.append("ordering lifecycle did not balance")
    if mode == "full":
        if native.get("checked_transactions") != native_accepted:
            reasons.append("checking transaction lifecycle did not balance")
        if native.get("checked_events") != native.get("accepted_events"):
            reasons.append("checking event lifecycle did not balance")
        if verdict != {"available": 1., "pass": 1., "violation": 0.}:
            reasons.append(f"live checker verdict is not pass: {verdict!r}")
    if cutoff:
        if adapter.get("accepted_publications") != initial_adapter.get("accepted_publications"):
            reasons.append("accepted publications changed after workload cutoff")
        if native.get("accepted_events") != initial_native.get("accepted_events"):
            reasons.append("accepted events changed after workload cutoff")
    return reasons

while time.monotonic() < deadline:
    scrapes += 1
    try:
        with urllib.request.urlopen(url, timeout=5.) as response:
            last_text = response.read().decode("utf-8", errors="replace")
        last_adapter, last_native, run_info, last_verdict, live_success = parse(last_text)
        observed_ns = time.monotonic_ns()
        if initial_adapter is None:
            initial_adapter, initial_native = dict(last_adapter), dict(last_native)
            initial_verdict = dict(last_verdict) if last_verdict else None
            initial_scrape_ns = observed_ns
        last_reasons = evaluate(last_adapter, last_native, run_info, last_verdict, live_success)
        if not last_reasons:
            complete_ns = observed_ns
            break
    except Exception as error:
        scrape_failures += 1
        last_reasons = [f"metrics scrape failed: {error}"]
    time.sleep(.05)
raw_path.write_text(last_text)
initial_adapter, initial_native = initial_adapter or {}, initial_native or {}
initial_tx, final_tx = residual_transactions(initial_native), residual_transactions(last_native)
initial_events, final_events = residual_events(initial_native), residual_events(last_native)
result = {
    "format_version": 2, "passed": not last_reasons, "drained": not last_reasons,
    "mode": mode, "clock": "CLOCK_MONOTONIC",
    "workload_cutoff_monotonic_ns": cutoff.get("workload_cutoff_monotonic_ns") if cutoff else None,
    "drain_started_monotonic_ns": drain_started_ns,
    "initial_scrape_monotonic_ns": initial_scrape_ns,
    "drain_complete_monotonic_ns": complete_ns,
    "live_prefix_verdict_monotonic_ns": complete_ns if mode == "full" and not last_reasons else None,
    "cutoff_observation_delay_ns": cutoff.get("observation_delay_ns") if cutoff else None,
    "scrapes": scrapes, "scrape_attempts": scrapes, "scrape_failures": scrape_failures,
    "reason": "; ".join(last_reasons),
    "initial_adapter_metrics": initial_adapter, "initial_native_metrics": initial_native,
    "initial_verdict": initial_verdict, "adapter_metrics": last_adapter,
    "native_metrics": last_native, "verdict": last_verdict or None,
    "cutoff_residual_transactions": initial_tx, "cutoff_residual_events": initial_events,
    "drained_transactions": None if initial_tx is None or final_tx is None else initial_tx - final_tx,
    "drained_events": None if initial_events is None or final_events is None else initial_events - final_events,
    "drain_duration_seconds": None if complete_ns is None else (complete_ns - drain_started_ns) / 1e9,
    "live_scrape_success": not last_reasons,
}
output_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
if last_reasons:
    raise SystemExit("online-checker live drain failed: " + "; ".join(last_reasons))
PY
}
write_or_verify_experiment() {
  local suite_root=$1
  local suite_trials=$2
  local suite_warmup=$3
  local suite_run=$4
  local suite_warehouses=$5
  local suite_threads=$6
  local suite_prepare_threads=$7
  local suite_interval=$8
  local suite_stabilize=$9
  local suite_smoke=${10}
  local expected
  (( suite_prepare_threads == 1 )) || die "suite preparation worker count must be 1"
  mkdir -p "$suite_root"
  expected=$(mktemp /tmp/tpcc-global-lock-experiment.XXXXXX)
  python3 - \
    "$suite_root" \
    "$suite_trials" \
    "$suite_warmup" \
    "$suite_run" \
    "$suite_warehouses" \
    "$suite_threads" \
    "$suite_prepare_threads" \
    "$suite_interval" \
    "$suite_stabilize" \
    "$suite_smoke" \
    "$MODE_CSV" \
    "$TIDB_CPUSET" \
    "$CLIENT_CPUSET" \
    "$SQL_HOST" \
    "$SQL_PORT" \
    "$STATUS_PORT" \
    "$DSN_PARAMS" \
    "$MONITOR_INTERVAL_SECONDS" \
    "$CHECKER_DRAIN_TIMEOUT" \
    "$QUEUE_CAPACITY_BYTES" \
    "$CAPTURE_CAPACITY_BYTES" \
    "$RETAINED_STATE_CAPACITY_BYTES" \
    "$QUEUE_TIMEOUT" \
    "$CAPTURE_TIMEOUT" \
    "$CHUNK_SEGMENT_BYTES" \
    "$MAX_CACHED_CHUNK_SEGMENTS" \
    "$ARTIFACT_INDEX" \
    "$TRANSACTION_WEIGHTS" \
    "$PREPARE_SEED" \
    "$WORKLOAD_SEED" \
    "$PREPARE_LOAD_TIME" \
    "$DUPLICATE_READ_DIAGNOSTICS" >"$expected" <<'PY'
import json
import pathlib
import sys

(
    suite_root, trials, warmup, run_time, warehouses, threads, prepare_threads,
    interval, stabilize, smoke, modes_csv, tidb_cpus, client_cpus, host, sql_port,
    status_port, dsn, monitor_interval, checker_drain_timeout, queue_bytes, capture_bytes,
    retained_bytes, queue_timeout, capture_timeout, segment_bytes,
    cached_segments, artifact_index, weights, prepare_seed, workload_seed,
    prepare_load_time, duplicate_read_diagnostics,
) = sys.argv[1:]
artifacts = json.loads(pathlib.Path(artifact_index).read_text())
transaction_weights = [int(value) for value in weights.split(",")]
if (
    len(transaction_weights) != 5
    or any(value < 0 for value in transaction_weights)
    or sum(transaction_weights) != 100
    or transaction_weights[0] <= 0
):
    raise SystemExit(f"invalid TPC-C transaction weights: {transaction_weights}")
if transaction_weights != [45, 43, 4, 4, 4] and smoke != "true":
    raise SystemExit("non-default TPC-C weights are supported only for smoke diagnostics")
if duplicate_read_diagnostics not in {"0", "1"}:
    raise SystemExit("duplicate-read diagnostics must be 0 or 1")
selected_modes = modes_csv.split(",")
allowed_mode_matrices = [
    [
        "baseline-original", "serialized-capture-only",
        "serialized-order-only", "serialized-full",
    ],
    ["baseline-original", "serialized-full"],
]
if selected_modes not in allowed_mode_matrices:
    raise SystemExit(f"unsupported mode matrix: {selected_modes}")

manifest = {
    "format_version": 2,
    "benchmark": "TPC-C",
    "workload_label": "TPC-C full mix",
    "transaction_weights": transaction_weights,
    "suite_root": str(pathlib.Path(suite_root).resolve()),
    "smoke": smoke == "true",
    "trials": int(trials),
    "modes": selected_modes,
    "mode_order": selected_modes,
    "workload": {
        "warmup_time": warmup,
        "run_time": run_time,
        "warehouses": int(warehouses),
        "threads": int(threads),
        "prepare_threads": int(prepare_threads),
        "output_interval": interval,
        "stabilize_seconds": int(stabilize),
        "prepare_seed": int(prepare_seed),
        "workload_seed": int(workload_seed),
        "prepare_load_time": prepare_load_time,
        "go_tpc_ignore_error": False,
        "go_tpc_silence_errors": True,
        "go_tpc_error_policy": {
            "policy": "continue-only-tidb-write-conflict",
            "mysql_error_numbers": [9007],
            "generic_ignore_error": False,
        },
        "deterministic_weight_deck": True,
    },
    "preparation": {
        "partition_type": {"code": 1, "name": "HASH"},
        "partitions": 1,
        "use_foreign_keys": False,
        "initial_state": "fresh-empty-store-per-mode",
        "schema_bootstrap": "go-tpc ddl-only before checker activation",
        "data_load": "go-tpc skip-ddl after checker activation in checked modes",
        "worker_count": 1,
        "ordered_manifest": True,
        "exact_schema_and_row_digest": True,
        "reference_path": str(pathlib.Path(suite_root, "preparation-reference.json").resolve()),
        "outside_measured_workload_timing": True,
    },
    "transaction_configuration": {
        "mode": "optimistic",
        "async_commit": False,
        "one_phase_commit": False,
        "low_resolution_tso": False,
        "dsn_params": dsn,
    },
    "checker_configuration": {
        "ordering_protocol": "serialized-global-lock",
        "duplicate_read_diagnostics": duplicate_read_diagnostics == "1",
        "queue_capacity_bytes": int(queue_bytes),
        "capture_capacity_bytes": int(capture_bytes),
        "retained_state_capacity_bytes": int(retained_bytes),
        "reorder_capacity_bytes": 0,
        "reorder_capacity_environment": "unset",
        "frontier_interval_environment": "unset",
        "frontier_pump": False,
        "metric_scope": "complete data preparation + warm-up + measured workload",
        "queue_timeout": queue_timeout,
        "capture_timeout": capture_timeout,
        "chunk_segment_bytes": int(segment_bytes),
        "max_cached_chunk_segments": int(cached_segments),
        "pre_measurement_live_drain_gate": True,
        "operational_drain_timeout": checker_drain_timeout,
        "live_sample_interval_seconds": 1,
    },
    "placement": {
        "tidb_cpus": tidb_cpus,
        "client_cpus": client_cpus,
        "sql_host": host,
        "sql_port": int(sql_port),
        "status_port": int(status_port),
        "monitor_interval_seconds": int(monitor_interval),
    },
    "storage_configuration": {
        "store": "unistore",
        "fresh_store_per_mode": True,
        "coprocessor_cache_capacity_mb": 0,
        "tidb_store_batch_size": 0,
        "tidb_allow_batch_cop": False,
        "tidb_index_lookup_pushdown_policy": "force",
    },
    "build": {
        "type": artifacts["build_type"],
        "tidb_build_command": artifacts["tidb_build_command"],
        "artifact_index": str(pathlib.Path(artifact_index).resolve()),
        "native_library_sha256": artifacts["artifacts"]["native_library"]["sha256"],
        "pristine_tidb_commit": artifacts["inputs"]["pristine_tidb"]["commit"],
        "pristine_tidb_server_sha256": artifacts["artifacts"]["pristine_tidb_server"]["sha256"],
        "instrumented_tidb_commit": artifacts["inputs"]["instrumented_tidb"]["commit"],
        "instrumented_tidb_server_sha256": artifacts["artifacts"]["instrumented_tidb_server"]["sha256"],
        "go_tpc_sha256": artifacts["artifacts"]["go_tpc"]["sha256"],
        "go_tpc_patch_sha256": artifacts["artifacts"]["go_tpc_patch"]["sha256"],
    },
}
print(json.dumps(manifest, indent=2, sort_keys=True))
PY

  if [[ -e "$suite_root/experiment.json" ]]; then
    if [[ "$RESUME" != true ]]; then
      rm -f "$expected"
      die "experiment already exists at $suite_root; pass --resume"
    fi
    if ! cmp -s "$expected" "$suite_root/experiment.json"; then
      diff -u "$suite_root/experiment.json" "$expected" >&2 || true
      rm -f "$expected"
      die "resume manifest does not exactly match $suite_root/experiment.json"
    fi
    rm -f "$expected"
  else
    if [[ "$RESUME" = true ]] &&
      [[ -n "$(find "$suite_root" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
      if [[ "$suite_root" != "$RESULT_ROOT" ]]; then
        rm -f "$expected"
        die "cannot resume suite without experiment.json: $suite_root"
      fi
      python3 - "$suite_root" "$ARTIFACT_INDEX" <<'PY'
import json
import pathlib
import sys
root, artifact_index = map(pathlib.Path, sys.argv[1:])
allowed = {"artifacts", "build-artifacts.json"}
entries = {path.name for path in root.iterdir()}
unexpected = sorted(entries - allowed)
if unexpected:
    raise SystemExit("cannot adopt interrupted setup with non-setup artifacts: " + ", ".join(unexpected))
if not artifact_index.is_file():
    raise SystemExit("interrupted setup is missing the verified artifact index")
artifacts = json.loads(artifact_index.read_text())
artifact_root = (root / "artifacts").resolve()
for artifact in artifacts.get("artifacts", {}).values():
    pathlib.Path(artifact["path"]).resolve().relative_to(artifact_root)
PY
      echo "Adopting verified build artifacts before creating $suite_root/experiment.json"
    fi
    mv "$expected" "$suite_root/experiment.json"
    chmod 0444 "$suite_root/experiment.json"
  fi
}

write_smoke_experiment() {
  local suite_root=$1
  write_or_verify_experiment \
    "$suite_root" \
    1 \
    "$SMOKE_WARMUP_TIME" \
    "$SMOKE_RUN_TIME" \
    "$SMOKE_WAREHOUSES" \
    "$SMOKE_THREADS" \
    "$SMOKE_PREPARE_THREADS" \
    "$SMOKE_OUTPUT_INTERVAL" \
    "$SMOKE_STABILIZE_SECONDS" \
    true
}

write_requested_experiment() {
  local suite_root=$1
  write_or_verify_experiment \
    "$suite_root" \
    "$TRIALS" \
    "$WARMUP_TIME" \
    "$RUN_TIME" \
    "$WAREHOUSES" \
    "$THREADS" \
    "$PREPARE_THREADS" \
    "$OUTPUT_INTERVAL" \
    "$STABILIZE_SECONDS" \
    false
}

record_phase() {
  local output=$1
  local phase=$2
  local state=$3
  local status=$4
  local timestamp
  timestamp=$(monotonic_ns)
  python3 - "$output" "$phase" "$state" "$status" "$timestamp" <<'PY'
import json
import pathlib
import sys

path, phase, state, status, timestamp = sys.argv[1:]
record = {
    "phase": phase,
    "state": state,
    "monotonic_ns": int(timestamp),
    "exit_status": None if status == "" else int(status),
}
with pathlib.Path(path).open("a") as stream:
    stream.write(json.dumps(record, sort_keys=True) + "\n")
PY
}

validate_go_tpc_run() {
  local expected_duration=$1
  local output_interval=$2
  local command_status=$3
  local input=$4
  local output=$5
  local transaction_weights=$6
  local start_ns=${7:-}
  local exit_ns=${8:-}
  python3 - \
    "$expected_duration" \
    "$output_interval" \
    "$command_status" \
    "$input" \
    "$output" \
    "$transaction_weights" \
    "$start_ns" \
    "$exit_ns" <<'PY'
import json
import math
import pathlib
import re
import sys

(
    expected_text, interval_text, status_text, input_path, output_path,
    weights_text, start_ns, exit_ns,
) = sys.argv[1:]
operations = ("NEW_ORDER", "PAYMENT", "ORDER_STATUS", "DELIVERY", "STOCK_LEVEL")
transaction_weights = [int(value) for value in weights_text.split(",")]

def duration_seconds(text):
    units = {"h": 3600.0, "m": 60.0, "s": 1.0, "ms": 0.001}
    pattern = re.compile(r"([0-9]+(?:\.[0-9]+)?)(ms|h|m|s)")
    position = 0
    total = 0.0
    for match in pattern.finditer(text):
        if match.start() != position:
            raise ValueError(f"unsupported duration {text!r}")
        total += float(match.group(1)) * units[match.group(2)]
        position = match.end()
    if position != len(text) or total <= 0:
        raise ValueError(f"unsupported duration {text!r}")
    return total

expected = duration_seconds(expected_text)
interval = duration_seconds(interval_text)
command_status = int(status_text)
text = pathlib.Path(input_path).read_text(errors="replace")
lower = text.lower()
policy_records = []
finished = sum(line.strip() == "Finished" for line in text.splitlines())
current_intervals = {operation: 0 for operation in operations}
summaries = {operation: [] for operation in operations}
error_summaries = {operation: [] for operation in operations}
final_records = []
malformed = 0
for line in text.splitlines():
    stripped = line.strip()
    if not stripped.startswith(("[", "{")):
        continue
    try:
        decoded = json.loads(stripped)
    except json.JSONDecodeError:
        malformed += 1
        continue
    records = decoded if isinstance(decoded, list) else [decoded]
    for record in records:
        if not isinstance(record, dict):
            continue
        prefix = str(record.get("Prefix", "")).strip()
        operation = str(record.get("Operation", ""))
        if prefix == "[Current]" and operation in current_intervals:
            current_intervals[operation] += 1
        elif prefix == "[Summary]" and operation in summaries:
            summaries[operation].append(record)
        if record.get("record") == "go-tpc-run-error-policy":
            policy_records.append(record)
        elif prefix == "[Summary]" and operation.endswith("_ERR"):
            base = operation[:-4]
            if base in error_summaries:
                error_summaries[base].append(record)
        if "tpmC" in record:
            final_records.append(record)

failure_markers = [
    marker for marker in ("panic:", "fatal error", "execute run failed", "failed to execute workload")
    if marker in lower
]
reasons = []
if command_status != 0:
    reasons.append(f"go-tpc exit status {command_status}")
if finished != 1:
    reasons.append(f"expected one Finished marker, found {finished}")
if malformed:
    reasons.append(f"found {malformed} malformed JSON candidate lines")
if failure_markers:
    reasons.append(f"failure markers: {failure_markers}")
if len(final_records) != 1:
    reasons.append(f"expected one tpmC record, found {len(final_records)}")

policy_record = {}
continued_write_conflicts = None
if len(policy_records) != 1:
    reasons.append(
        f"expected one strict write-conflict policy record, found {len(policy_records)}"
    )
else:
    policy_record = policy_records[0]
    continued_write_conflicts = policy_record.get("continued_count")
    rejected_run_errors = policy_record.get("rejected_count")
    if (
        policy_record.get("format_version") != 1
        or policy_record.get("policy") != "continue-only-tidb-write-conflict"
        or policy_record.get("mysql_error_number") != 9007
    ):
        reasons.append("strict write-conflict policy identity is malformed")
    if (
        isinstance(continued_write_conflicts, bool)
        or not isinstance(continued_write_conflicts, int)
        or continued_write_conflicts < 0
    ):
        reasons.append("continued write-conflict count is malformed")
        continued_write_conflicts = None
    if rejected_run_errors != 0:
        reasons.append(
            f"strict write-conflict policy rejected {rejected_run_errors!r} run errors"
        )

transaction_results = {}
summary_seconds = []
for operation, weight in zip(operations, transaction_weights):
    if weight == 0:
        if summaries[operation]:
            reasons.append(f"zero-weight {operation} unexpectedly has a summary")
        if error_summaries[operation]:
            reasons.append(f"zero-weight {operation} unexpectedly has an error summary")
        transaction_results[operation.lower()] = {
            "count": 0,
            "error_count": 0,
            "p99_ms": None,
            "takes_seconds": None,
        }
        continue
    if len(summaries[operation]) != 1:
        reasons.append(f"expected one {operation} summary, found {len(summaries[operation])}")
        continue
    summary = summaries[operation][0]
    try:
        count = int(summary["Count"])
        duration = float(summary["Takes(s)"])
        p99_ms = float(summary["99th(ms)"])
    except (KeyError, TypeError, ValueError) as error:
        reasons.append(f"{operation} summary is malformed: {error}")
        continue
    if count <= 0:
        reasons.append(f"{operation} executed no successful transactions")
    errors = 0
    if len(error_summaries[operation]) > 1:
        reasons.append(f"found duplicate {operation}_ERR summaries")
    elif error_summaries[operation]:
        try:
            errors = int(error_summaries[operation][0]["Count"])
        except (KeyError, TypeError, ValueError) as error:
            reasons.append(f"{operation}_ERR summary is malformed: {error}")
    if errors < 0:
        reasons.append(f"{operation} reported a negative failed transaction count")
    tolerance = max(2.0, expected * 0.01)
    if abs(duration - expected) > tolerance:
        reasons.append(f"{operation} summary duration {duration} differs from expected {expected}")
    summary_seconds.append(duration)
    transaction_results[operation.lower()] = {
        "count": count,
        "error_count": errors,
        "p99_ms": p99_ms,
        "takes_seconds": duration,
    }

reported_error_count = sum(
    value["error_count"] for value in transaction_results.values()
)
if (
    continued_write_conflicts is None
    or reported_error_count != continued_write_conflicts
):
    reasons.append("structured transaction errors do not conserve classified Error 9007 conflicts")

minimum_intervals = max(1, math.floor(expected / interval) - 1)
if current_intervals["NEW_ORDER"] < minimum_intervals:
    reasons.append(
        f"only {current_intervals['NEW_ORDER']} NEW_ORDER intervals; expected at least {minimum_intervals}"
    )
actual_seconds = None
if start_ns and exit_ns:
    actual_seconds = (int(exit_ns) - int(start_ns)) / 1_000_000_000
elif summary_seconds:
    actual_seconds = summary_seconds[0]
final = final_records[0] if len(final_records) == 1 else {}
result = {
    "passed": not reasons,
    "reason": "; ".join(reasons),
    "exit_status": command_status,
    "expected_seconds": expected,
    "actual_seconds": actual_seconds,
    "go_tpc_summary_seconds": summary_seconds[0] if summary_seconds else None,
    "current_interval_counts": current_intervals,
    "minimum_interval_count": minimum_intervals,
    "finished_marker_count": finished,
    "malformed_json_record_count": malformed,
    "failure_markers": failure_markers,
    "transaction_weights": transaction_weights,
    "transactions": transaction_results,
    "new_order_count": transaction_results.get("new_order", {}).get("count"),
    "new_order_error_count": transaction_results.get("new_order", {}).get("error_count"),
    "tpm_c": float(final["tpmC"]) if "tpmC" in final else None,
    "continued_write_conflict_count": continued_write_conflicts,
    "go_tpc_error_policy": policy_record,
    "go_tpc_ignore_error": False,
    "go_tpc_silence_errors": True,
}
pathlib.Path(output_path).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
if reasons:
    raise SystemExit("go-tpc validation failed: " + "; ".join(reasons))
PY
}

validate_post_check() {
  local warehouses=$1
  local command_status=$2
  local input=$3
  local output=$4
  python3 - "$warehouses" "$command_status" "$input" "$output" <<'PY'
import json
import pathlib
import re
import sys

warehouses = int(sys.argv[1])
status = int(sys.argv[2])
text = pathlib.Path(sys.argv[3]).read_text(errors="replace")
lower = text.lower()
failure_markers = [
    marker
    for marker in (
        "execute check failed",
        "failed to init work loader",
        "panic:",
        "fatal error",
    )
    if marker in lower
]
finished = sum(line.strip() == "Finished" for line in text.splitlines())
pattern = re.compile(r"^begin to check warehouse (\d+) at condition (3\.3\.2\.(\d+))$")
observed = []
for line in text.splitlines():
    match = pattern.match(line.strip())
    if match:
        observed.append((int(match.group(1)), match.group(2)))
condition_ids = (*range(1, 11), 12)
expected = {
    (warehouse, f"3.3.2.{condition}")
    for warehouse in range(1, warehouses + 1)
    for condition in condition_ids
}
observed_set = set(observed)
reasons = []
if status != 0:
    reasons.append(f"go-tpc check exit status {status}")
if failure_markers:
    reasons.append(f"failure markers: {failure_markers}")
if finished != 1:
    reasons.append(f"expected one Finished marker, found {finished}")
if observed_set != expected or len(observed) != len(expected):
    reasons.append(
        "coverage mismatch: "
        f"missing={sorted(expected - observed_set)}, "
        f"unexpected={sorted(observed_set - expected)}, "
        f"records={len(observed)}, expected={len(expected)}"
    )
result = {
    "outcome": "pass" if not reasons else "fail",
    "passed": not reasons,
    "reason": "; ".join(reasons),
    "exit_status": status,
    "warehouses": warehouses,
    "conditions_per_warehouse": len(condition_ids),
    "condition_ids": list(condition_ids),
    "checks_completed": len(observed),
    "finished_marker_count": finished,
    "failure_markers": failure_markers,
}
pathlib.Path(sys.argv[4]).write_text(
    json.dumps(result, indent=2, sort_keys=True) + "\n"
)
if reasons:
    raise SystemExit("TPC-C post-check validation failed: " + "; ".join(reasons))
PY
}

validate_checker_result() {
  local runtime_mode=$1
  local result_path=$2
  local output=$3
  python3 - "$runtime_mode" "$result_path" "$output" <<'PY'
import json
import pathlib
import sys

mode, result_path, output_path = sys.argv[1:]
reasons = []
try:
    result = json.loads(pathlib.Path(result_path).read_text())
except (OSError, json.JSONDecodeError) as error:
    result = {}
    reasons.append(f"checker result is unavailable or malformed: {error}")

adapter = result.get("adapter_metrics")
native = result.get("native_metrics")
preclose = result.get("native_metrics_before_close")
if not isinstance(adapter, dict):
    adapter = {}
    reasons.append("adapter_metrics is missing")
if not isinstance(native, dict):
    native = {}
    reasons.append("native_metrics is missing")
if not isinstance(preclose, dict):
    preclose = {}
    reasons.append("native_metrics_before_close is missing")

def require(condition, message):
    if not condition:
        reasons.append(message)

def integer(record, field):
    value = record.get(field)
    if not isinstance(value, int) or isinstance(value, bool):
        reasons.append(f"missing integer {field}")
        return None
    return value

def equal(record, left, right, message):
    left_value = integer(record, left)
    right_value = integer(record, right)
    if left_value is not None and right_value is not None:
        require(left_value == right_value, message)

expected_outcome = {
    "capture-only": "capture-only-complete",
    "order-only": "order-only-complete",
    "full": "pass",
}[mode]
require(result.get("format_version") == 1, "unexpected checker format_version")
require(result.get("mode") == mode, "checker mode mismatch")
require(
    result.get("ordering_protocol") == "serialized-global-lock",
    "checker ordering protocol mismatch",
)
require(result.get("outcome") == expected_outcome, "unexpected checker outcome")
require(result.get("close_status") == 0, "native close status is nonzero")
require(result.get("runtime_error") is None, "checker runtime_error is present")
require(not result.get("close_error"), "checker close_error is present")
require(native.get("closed") is True, "native runtime did not close")
require(native.get("fatal") is False, "native checker reported fatal")
require(native.get("ordering_protocol") == 1, "native ordering protocol is not serialized")
require(native.get("reorder_capacity_bytes") == 0, "effective reorder capacity is not zero")

registered = integer(adapter, "attempts_registered")
published = integer(adapter, "attempts_published")
aborted = integer(adapter, "attempts_aborted")
accepted = integer(adapter, "accepted_publications")
publication_sequence = integer(adapter, "publication_sequence")
successful_writes = integer(adapter, "successful_write_commits")
read_only = integer(adapter, "read_only_no_write_completions")
if registered is not None:
    require(registered > 0, "attempt lifecycle did not advance")
if None not in (registered, published, aborted):
    require(registered == published + aborted, "attempt lifecycle counters do not balance")
if None not in (successful_writes, read_only, accepted):
    require(
        successful_writes + read_only == accepted,
        "completion/publication counters do not balance",
    )
if accepted is not None:
    require(accepted > 0, "publication lifecycle did not advance")
if None not in (accepted, published):
    require(accepted == published, "accepted/native publication counters differ")
if None not in (accepted, publication_sequence):
    require(accepted == publication_sequence, "publication sequence is not dense")

for field in (
    "active_attempts",
    "active_finalizers",
    "active_fragments",
    "fragment_buffer_active_bytes",
    "fragment_retained_active_bytes",
):
    require(adapter.get(field) == 0, f"adapter ownership field {field} did not drain")
fragments_started = integer(adapter, "fragments_started")
fragments_accepted = integer(adapter, "fragments_accepted")
fragments_discarded = integer(adapter, "fragments_discarded")
fragments_attached = integer(adapter, "fragments_storage_attached")
if None not in (fragments_started, fragments_accepted, fragments_discarded):
    require(fragments_started > 0, "fragment lifecycle did not advance")
    require(
        fragments_started == fragments_accepted + fragments_discarded,
        "fragment lifecycle counters do not balance",
    )
if None not in (fragments_attached, fragments_started):
    require(fragments_attached <= fragments_started, "storage-attached fragments exceed started fragments")
for late, total in (
    ("fragments_late_discarded", "fragments_discarded"),
    ("fragment_late_discarded_events", "fragment_discarded_events"),
    ("fragment_late_discarded_bytes", "fragment_discarded_payload_bytes"),
):
    late_value = integer(adapter, late)
    total_value = integer(adapter, total)
    if late_value is not None and total_value is not None:
        require(late_value <= total_value, f"{late} exceeds {total}")
require(adapter.get("fragment_capture_errors") == 0, "fragment capture errors are nonzero")
for field in (
    "finalization_acquisitions",
    "finalization_wait_nanos",
    "max_finalization_wait_nanos",
    "finalization_hold_nanos",
    "max_finalization_hold_nanos",
):
    integer(adapter, field)
for field in ("fatal_calls", "publish_errors", "abort_errors"):
    require(adapter.get(field) == 0, f"adapter {field} is nonzero")

native_accepted = integer(native, "accepted_publications")
accepted_events = integer(native, "accepted_events")
ordered = integer(native, "ordered_transactions")
checked = integer(native, "checked_transactions")
checked_events = integer(native, "checked_events")
if None not in (accepted, native_accepted):
    require(accepted == native_accepted, "adapter/native accepted publications differ")
for field in (
    "active_attempts",
    "committed_but_unpublished",
    "capture_active_bytes",
    "queue_reserved_bytes",
    "queue_reserved_depth",
    "queue_bytes",
    "queue_depth",
    "reorder_bytes",
):
    require(native.get(field) == 0, f"native ownership field {field} did not drain")
for field in (
    "queue_peak_depth",
    "queue_peak_bytes",
    "queue_backpressure_wait_count",
    "queue_backpressure_ns",
    "total_queue_lag_ns",
    "max_queue_lag_ns",
):
    integer(native, field)

if mode == "capture-only":
    recycled = integer(native, "recycled_transactions")
    if None not in (native_accepted, recycled):
        require(native_accepted == recycled, "capture-only publications were not recycled")
else:
    if None not in (native_accepted, ordered):
        require(native_accepted == ordered, "not all accepted publications were ordered")
if mode == "full":
    if None not in (native_accepted, checked):
        require(native_accepted == checked, "not all accepted publications were checked")
    if None not in (accepted_events, checked_events):
        require(accepted_events == checked_events, "not all accepted events were checked")
    require(native.get("violating_transactions") == 0, "checker found a violation")
    require(
        isinstance(result.get("verdict"), dict)
        and result["verdict"].get("status") == "pass",
        "full checker verdict is not pass",
    )

require(preclose.get("reorder_capacity_bytes") == 0, "pre-close reorder capacity is not zero")
for field in (
    "queue_depth",
    "queue_peak_depth",
    "queue_peak_bytes",
    "queue_backpressure_wait_count",
    "queue_backpressure_ns",
    "total_queue_lag_ns",
    "max_queue_lag_ns",
    "captured_transactions",
    "accepted_publications",
    "ordered_transactions",
    "checked_transactions",
):
    integer(preclose, field)

validation = {
    "passed": not reasons,
    "reason": "; ".join(reasons),
    "mode": mode,
    "outcome": result.get("outcome"),
    "ordering_protocol": result.get("ordering_protocol"),
    "effective_reorder_capacity_bytes": native.get("reorder_capacity_bytes"),
    "lifecycle_balanced": not reasons,
    "active_attempts": adapter.get("active_attempts"),
    "active_finalizers": adapter.get("active_finalizers"),
    "active_fragments": adapter.get("active_fragments"),
    "fragment_buffer_active_bytes": adapter.get("fragment_buffer_active_bytes"),
    "fragment_retained_active_bytes": adapter.get("fragment_retained_active_bytes"),
    "fragments_started": adapter.get("fragments_started"),
    "fragments_accepted": adapter.get("fragments_accepted"),
    "fragments_discarded": adapter.get("fragments_discarded"),
    "fragments_late_discarded": adapter.get("fragments_late_discarded"),
    "accepted_publications": native.get("accepted_publications"),
    "accepted_events": native.get("accepted_events"),
    "ordered_transactions": native.get("ordered_transactions"),
    "checked_transactions": native.get("checked_transactions"),
    "checked_events": native.get("checked_events"),
    "queue_depth_before_close": preclose.get("queue_depth"),
    "captured_minus_ordered_tail": (
        preclose.get("accepted_publications", 0) - preclose.get("ordered_transactions", 0)
        if isinstance(preclose.get("accepted_publications"), int)
        and isinstance(preclose.get("ordered_transactions"), int)
        else None
    ),
    "captured_minus_checked_tail": (
        preclose.get("accepted_publications", 0) - preclose.get("checked_transactions", 0)
        if isinstance(preclose.get("accepted_publications"), int)
        and isinstance(preclose.get("checked_transactions"), int)
        else None
    ),
    "tail_definition": (
        "accepted committed publications minus ordered/checked transactions; "
        "raw native captured counters include prepared attempts that later abort"
    ),
}
pathlib.Path(output_path).write_text(
    json.dumps(validation, indent=2, sort_keys=True) + "\n"
)
if reasons:
    raise SystemExit("checker result validation failed: " + "; ".join(reasons))
PY
}

analyzer_attempt_is_valid() {
  local suite_root=$1
  local trial=$2
  local mode=$3
  python3 - "$suite_root" "$trial" "$mode" <<'PY'
import csv
import pathlib
import sys

root = pathlib.Path(sys.argv[1]).resolve()
trial, mode = sys.argv[2:]
with (root / "tpcc-trials.csv").open(newline="") as stream:
    rows = [
        row for row in csv.DictReader(stream)
        if row.get("trial") == trial and row.get("mode") == mode
    ]
if len(rows) != 1 or rows[0].get("valid", "").lower() != "true":
    raise SystemExit(1)
row = rows[0]
expected = pathlib.Path("trials") / trial / mode / row.get("attempt", "")
if pathlib.Path(row.get("attempt_path", "")) != expected:
    raise SystemExit(1)
if not (root / expected).is_dir():
    raise SystemExit(1)
print(root / expected)
PY
}

write_attempt_manifest() {
  local output=$1
  local trial=$2
  local mode=$3
  local attempt=$4
  local runtime_mode=$5
  local server_path=$6
  local server_sha=$7
  local expected_commit=$8
  python3 - \
    "$output" "$trial" "$mode" "$attempt" "$runtime_mode" \
    "$server_path" "$server_sha" "$expected_commit" \
    "$ACTIVE_WARMUP_TIME" "$ACTIVE_RUN_TIME" "$ACTIVE_WAREHOUSES" \
    "$ACTIVE_THREADS" "$ACTIVE_PREPARE_THREADS" "$ACTIVE_OUTPUT_INTERVAL" \
    "$ACTIVE_STABILIZE_SECONDS" "$TIDB_CPUSET" "$CLIENT_CPUSET" \
    "$DSN_PARAMS" "$ACTIVE_SUITE_ROOT/experiment.json" \
    "$NATIVE_LIBRARY_SHA" "$IMMUTABLE_GO_TPC_SHA" "$TRANSACTION_WEIGHTS" \
    "$PREPARE_SEED" "$WORKLOAD_SEED" "$PREPARE_LOAD_TIME" \
    "$CHECKER_DRAIN_TIMEOUT" <<'PY'
import json
import pathlib
import sys

(
    output, trial, mode, attempt, runtime_mode, server_path, server_sha,
    expected_commit, warmup, run_time, warehouses, threads, prepare_threads,
    interval, stabilize, tidb_cpus, client_cpus, dsn, experiment, native_sha,
    go_tpc_sha, weights, prepare_seed, workload_seed, prepare_load_time,
    checker_drain_timeout,
) = sys.argv[1:]
result = {
    "format_version": 2,
    "trial": trial,
    "mode": mode,
    "attempt": attempt,
    "workload_label": "TPC-C full mix",
    "runtime_mode": runtime_mode or None,
    "ordering_protocol": None if not runtime_mode else "serialized-global-lock",
    "workload": {
        "warmup_time": warmup,
        "run_time": run_time,
        "warehouses": int(warehouses),
        "threads": int(threads),
        "prepare_threads": int(prepare_threads),
        "output_interval": interval,
        "stabilize_seconds": int(stabilize),
        "weights": [int(value) for value in weights.split(",")],
        "prepare_seed": int(prepare_seed),
        "workload_seed": int(workload_seed),
        "prepare_load_time": prepare_load_time,
        "go_tpc_ignore_error": False,
        "go_tpc_silence_errors": True,
        "go_tpc_error_policy": {
            "policy": "continue-only-tidb-write-conflict",
            "mysql_error_numbers": [9007],
            "generic_ignore_error": False,
        },
    },
    "preparation": {
        "partition_type": {"code": 1, "name": "HASH"},
        "partitions": 1,
        "use_foreign_keys": False,
        "initial_state": "fresh-empty-store",
        "schema_bootstrap": "ddl-only before checker activation",
        "data_load": "skip-ddl after checker activation",
        "ordered_manifest_path": "prepare-manifest.json",
        "post_prepare_digest_path": "post-prepare-digest.json",
        "reference_path": str(pathlib.Path(experiment).with_name("preparation-reference.json")),
        "outside_measured_timing": True,
    },
    "transaction_configuration": {
        "mode": "optimistic",
        "async_commit": False,
        "one_phase_commit": False,
        "low_resolution_tso": False,
        "dsn_params": dsn,
    },
    "cpu_affinity": {"tidb": tidb_cpus, "client": client_cpus},
    "storage_configuration": {
        "store": "unistore",
        "fresh_store": True,
        "coprocessor_cache_capacity_mb": 0,
        "tidb_store_batch_size": 0,
        "tidb_allow_batch_cop": False,
        "tidb_index_lookup_pushdown_policy": "force",
    },
    "checker_configuration": {
        "reorder_capacity_bytes": 0,
        "reorder_environment": "unset",
        "frontier_interval_environment": "unset",
        "frontier_pump": False,
        "live_pre_measurement_drain_gate": True,
        "operational_drain_timeout": checker_drain_timeout,
        "live_sample_interval_seconds": 1,
    },
    "expected_tidb_commit": expected_commit,
    "binaries": {
        "tidb_server": {"path": str(pathlib.Path(server_path).resolve()), "sha256": server_sha},
        "native_library_sha256": native_sha,
        "go_tpc_sha256": go_tpc_sha,
    },
    "experiment_manifest": str(pathlib.Path(experiment).resolve()),
}
pathlib.Path(output).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
PY
  cp "$output" "$(dirname "$output")/configuration.json"
}

run_attempt() (
  set -Eeuo pipefail

  # These variables deliberately live in the per-attempt subshell instead of
  # being function-local. Bash runs an EXIT trap after unwinding function
  # locals on an error, and cleanup still needs the recorded PIDs and paths.
  trial_name=$1
  mode=$2
  attempt_name=$3
  ATTEMPT_DIR_PATH=$4
  attempt_dir=$ATTEMPT_DIR_PATH
  runtime_mode=
  server_path=
  server_sha=
  expected_commit=
  case "$mode" in
    baseline-original)
      server_path=$PRISTINE_TIDB_SERVER
      server_sha=$PRISTINE_TIDB_SERVER_SHA
      expected_commit=$PRISTINE_TIDB_COMMIT
      ;;
    serialized-capture-only)
      runtime_mode=capture-only
      server_path=$INSTRUMENTED_TIDB_SERVER
      server_sha=$INSTRUMENTED_TIDB_SERVER_SHA
      expected_commit=$INSTRUMENTED_TIDB_COMMIT
      ;;
    serialized-order-only)
      runtime_mode=order-only
      server_path=$INSTRUMENTED_TIDB_SERVER
      server_sha=$INSTRUMENTED_TIDB_SERVER_SHA
      expected_commit=$INSTRUMENTED_TIDB_COMMIT
      ;;
    serialized-full)
      runtime_mode=full
      server_path=$INSTRUMENTED_TIDB_SERVER
      server_sha=$INSTRUMENTED_TIDB_SERVER_SHA
      expected_commit=$INSTRUMENTED_TIDB_COMMIT
      ;;
    *)
      die "unknown mode $mode"
      ;;
  esac

  mkdir -p "$attempt_dir"
  cd "$attempt_dir"
  write_attempt_manifest \
    "$attempt_dir/attempt-manifest.json" \
    "$trial_name" \
    "$mode" \
    "$attempt_name" \
    "$runtime_mode" \
    "$server_path" \
    "$server_sha" \
    "$expected_commit"

  TIDB_PID=
  SERVER_PHASE=
  PIDSTAT_PID=
  RESOURCE_MONITOR_PID=
  CHECKER_TELEMETRY_PID=
  RESULT_WATCHER_PID=
  CURRENT_PHASE=initialization
  FAILURE_REASON=
  GO_TPC_PASS=false
  ATTEMPT_SUCCESS=false
  BOOTSTRAP_SERVER_EXIT_STATUS=
  MAIN_SERVER_EXIT_STATUS=
  POST_CHECK_SERVER_EXIT_STATUS=
  WORKLOAD_START_NS=
  WORKLOAD_CLIENT_EXIT_NS=
  CHECKER_DRAIN_STARTED_NS=
  CHECKER_DRAIN_COMPLETE_NS=
  CHECKER_COMPLETION_NS=
  LIVE_PREFIX_VERDICT_NS=
  CUTOFF_OBSERVATION_DELAY_NS=
  CHECKER_RESULT_AVAILABLE_NS=
  SERVER_SHUTDOWN_START_NS=
  TIDB_SERVER_EXIT_NS=

  write_runner_status() {
    local status=$1
    python3 - \
      "$attempt_dir/runner-status.json" \
      "$status" \
      "$GO_TPC_PASS" \
      "$BOOTSTRAP_SERVER_EXIT_STATUS" \
      "$MAIN_SERVER_EXIT_STATUS" \
      "$POST_CHECK_SERVER_EXIT_STATUS" \
      "$FAILURE_REASON" \
      "$CURRENT_PHASE" <<'PY'
import json
import pathlib
import sys

path, status, go_tpc, bootstrap_status, server_status, post_status, reason, phase = sys.argv[1:]
def optional_int(value):
    return None if value == "" else int(value)
pathlib.Path(path).write_text(json.dumps({
    "format_version": 1,
    "status": status,
    "go_tpc_pass": go_tpc == "true",
    "bootstrap_server_exit_status": optional_int(bootstrap_status),
    "server_exit_status": optional_int(server_status),
    "post_check_server_exit_status": optional_int(post_status),
    "failure_reason": reason or None,
    "last_phase": phase,
}, indent=2, sort_keys=True) + "\n")
PY
  }

  write_timing() {
    local native_close=
    if [[ -s "$attempt_dir/checker-result.json" ]]; then
      native_close=$(python3 - "$attempt_dir/checker-result.json" <<'PY'
import json, pathlib, sys
try:
    value = json.loads(pathlib.Path(sys.argv[1]).read_text()).get("native_close_nanoseconds")
except (OSError, json.JSONDecodeError):
    value = None
print("" if value is None else value)
PY
)
    fi
    python3 - \
      "$attempt_dir/timing.json" \
      "$WORKLOAD_START_NS" "$WORKLOAD_CLIENT_EXIT_NS" \
      "$CHECKER_DRAIN_STARTED_NS" "$CHECKER_DRAIN_COMPLETE_NS" \
      "$CHECKER_COMPLETION_NS" "$LIVE_PREFIX_VERDICT_NS" \
      "$CUTOFF_OBSERVATION_DELAY_NS" "$CHECKER_RESULT_AVAILABLE_NS" \
      "$TIDB_SERVER_EXIT_NS" "$SERVER_SHUTDOWN_START_NS" "$native_close" <<'PY'
import json, pathlib, sys
(
    path, workload_start, client_exit, drain_start, drain_complete,
    checker_completion, live_verdict, cutoff_delay, result_available,
    server_exit, shutdown_start, native_close,
) = sys.argv[1:]
def optional_int(value):
    return None if value == "" else int(value)
values = {
    "workload_start": optional_int(workload_start),
    "workload_client_exit": optional_int(client_exit),
    "checker_drain_started": optional_int(drain_start),
    "checker_drain_complete": optional_int(drain_complete),
    "checker_completion": optional_int(checker_completion),
    "live_prefix_verdict": optional_int(live_verdict),
    "cutoff_observation_delay": optional_int(cutoff_delay),
    "checker_result_available": optional_int(result_available),
    "tidb_server_exit": optional_int(server_exit),
    "server_shutdown_start": optional_int(shutdown_start),
}
record = {"clock": "CLOCK_MONOTONIC", "native_close_nanoseconds": optional_int(native_close)}
for name, value in values.items():
    record[name + "_monotonic_ns"] = value
    record[name + "_ns"] = value
pathlib.Path(path).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
PY
  }

  stop_pidstat() {
    if [[ -n "$PIDSTAT_PID" ]]; then
      kill -TERM "$PIDSTAT_PID" 2>/dev/null || true
      wait "$PIDSTAT_PID" 2>/dev/null || true
      PIDSTAT_PID=
    fi
  }

  stop_resource_monitor() {
    if [[ -n "$RESOURCE_MONITOR_PID" ]]; then
      kill -TERM "$RESOURCE_MONITOR_PID" 2>/dev/null || true
      wait "$RESOURCE_MONITOR_PID" 2>/dev/null || true
      RESOURCE_MONITOR_PID=
    fi
  }

  stop_checker_telemetry() {
    if [[ -n "$CHECKER_TELEMETRY_PID" ]]; then
      kill -TERM "$CHECKER_TELEMETRY_PID" 2>/dev/null || true
      wait "$CHECKER_TELEMETRY_PID" 2>/dev/null || true
      CHECKER_TELEMETRY_PID=
    fi
  }

  start_checker_telemetry() {
    local expected_mode=$runtime_mode
    : >"$attempt_dir/workload-checker-samples.jsonl"
    python3 - \
      "$expected_mode" "http://$SQL_HOST:$STATUS_PORT/metrics" \
      "$attempt_dir/workload-checker-samples.jsonl" \
      "$attempt_dir/workload-checker-monitor-ready.json" \
      "$attempt_dir/workload-checker-sample-request.txt" \
      "$attempt_dir/workload-checker-sample-ack.json" \
      "$attempt_dir/workload-cutoff.prom" <<'PY' &
import json
import pathlib
import re
import signal
import sys
import time
import urllib.request

expected_mode, url, samples_name, ready_name, request_name, ack_name, cutoff_raw_name = sys.argv[1:]
samples_path = pathlib.Path(samples_name)
ready_path = pathlib.Path(ready_name)
request_path = pathlib.Path(request_name)
ack_path = pathlib.Path(ack_name)
cutoff_raw_path = pathlib.Path(cutoff_raw_name)
stop = False
last_request = None
sample_pattern = re.compile(r"^(tidb_online_checker_(?:adapter|native|run_info))\{([^}]*)\}\s+([-+0-9.eE]+)$")
verdict_pattern = re.compile(r"^tidb_online_checker_verdict_(available|pass|violation)\s+([-+0-9.eE]+)$")
label_pattern = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)="((?:\\.|[^"\\])*)"')

def request_stop(_signal, _frame):
    global stop
    stop = True
signal.signal(signal.SIGTERM, request_stop)
signal.signal(signal.SIGINT, request_stop)

def scrape():
    now = time.monotonic_ns()
    adapter, native, run_info, verdict = {}, {}, [], {}
    text = ""
    error = None
    live_success = False
    try:
        with urllib.request.urlopen(url, timeout=2.) as response:
            text = response.read().decode("utf-8", errors="replace")
        for line in text.splitlines():
            stripped = line.strip()
            if stripped == "tidb_online_checker_live_scrape_success 1":
                live_success = True
                continue
            match = verdict_pattern.match(stripped)
            if match:
                name, value = match.groups()
                verdict[name] = float(value)
                continue
            match = sample_pattern.match(stripped)
            if not match:
                continue
            family, label_text, value = match.groups()
            labels = {name: bytes(value, "utf-8").decode("unicode_escape")
                      for name, value in label_pattern.findall(label_text)}
            value = float(value)
            if family.endswith("adapter") and "metric" in labels:
                adapter[labels["metric"]] = value
            elif family.endswith("native") and "metric" in labels:
                native[labels["metric"]] = value
            elif family.endswith("run_info"):
                run_info.append((labels, value))
        if verdict:
            native.update({f"verdict_{name}": value for name, value in verdict.items()})
        if expected_mode:
            identity = next((labels for labels, value in run_info
                if value == 1 and labels.get("mode") == expected_mode and
                labels.get("ordering_protocol") == "serialized-global-lock"), None)
            success = live_success and identity is not None and set(verdict) == {"available", "pass", "violation"}
            if not success:
                error = "checked scrape lacks success, identity, or verdict metrics"
            mode = None if identity is None else identity.get("mode")
            ordering = None if identity is None else identity.get("ordering_protocol")
        else:
            families_present = any(line.startswith("tidb_online_checker_") for line in text.splitlines())
            success = not families_present
            mode = ordering = None
            if not success:
                error = "pristine baseline unexpectedly exports online-checker metrics"
    except Exception as exc:
        success = False
        mode = ordering = None
        error = f"metrics scrape failed: {exc}"
    now = time.monotonic_ns()
    record = {
        "format_version": 1, "clock": "CLOCK_MONOTONIC", "monotonic_ns": now,
        "checker_enabled": bool(expected_mode), "expected_mode": expected_mode or None,
        "scrape_success": success, "mode": mode, "ordering_protocol": ordering,
        "adapter": adapter, "native": native, "verdict": verdict or None, "error": error,
    }
    with samples_path.open("a") as stream:
        stream.write(json.dumps(record, sort_keys=True) + "\n")
        stream.flush()
    return record, text

next_periodic = time.monotonic()
while not stop:
    request_id = None
    try:
        request_id = request_path.read_text().strip()
    except OSError:
        pass
    forced = bool(request_id and request_id != last_request)
    now = time.monotonic()
    if forced or now >= next_periodic:
        record, raw = scrape()
        if record["scrape_success"] and not ready_path.exists():
            ready_path.write_text(json.dumps(record, sort_keys=True) + "\n")
        if forced:
            cutoff_raw_path.write_text(raw)
            temporary = ack_path.with_suffix(".tmp")
            temporary.write_text(json.dumps({"request_id": request_id, "sample": record}, sort_keys=True) + "\n")
            temporary.replace(ack_path)
            last_request = request_id
        next_periodic = time.monotonic() + 1.
    time.sleep(.05)
PY
    CHECKER_TELEMETRY_PID=$!
    for _ in $(seq 1 400); do
      [[ -s "$attempt_dir/workload-checker-monitor-ready.json" ]] && return
      kill -0 "$CHECKER_TELEMETRY_PID" 2>/dev/null ||
        die "checker telemetry monitor exited before its initial sample"
      sleep 0.05
    done
    die "checker telemetry monitor did not produce a valid pre-workload sample"
  }

  capture_checker_workload_cutoff() {
    local cutoff_ns=$1
    local request_tmp="$attempt_dir/workload-checker-sample-request.tmp"
    printf '%s\n' "$cutoff_ns" >"$request_tmp"
    mv "$request_tmp" "$attempt_dir/workload-checker-sample-request.txt"
    local matched=false
    for _ in $(seq 1 200); do
      if python3 - "$attempt_dir/workload-checker-sample-ack.json" "$cutoff_ns" <<'PY'
import json, pathlib, sys
try:
    value = json.loads(pathlib.Path(sys.argv[1]).read_text())
except (OSError, json.JSONDecodeError):
    raise SystemExit(1)
raise SystemExit(0 if value.get("request_id") == sys.argv[2] else 1)
PY
      then
        matched=true
        break
      fi
      kill -0 "$CHECKER_TELEMETRY_PID" 2>/dev/null ||
        die "checker telemetry monitor exited before the cutoff sample"
      sleep 0.05
    done
    [[ "$matched" = true ]] || die "checker cutoff sample was not observed"
    python3 - \
      "$runtime_mode" "$cutoff_ns" \
      "$attempt_dir/workload-checker-sample-ack.json" \
      "$attempt_dir/workload-cutoff.json" <<'PY'
import json, pathlib, sys
mode, cutoff_text, ack_name, output_name = sys.argv[1:]
cutoff_ns = int(cutoff_text)
sample = json.loads(pathlib.Path(ack_name).read_text())["sample"]
adapter, native = sample.get("adapter", {}), sample.get("native", {})
target_name = {"capture-only": "recycled_transactions", "order-only": "ordered_transactions", "full": "checked_transactions"}.get(mode)
accepted, target = native.get("accepted_publications"), native.get(target_name) if target_name else None
residual_tx = None if accepted is None or target is None else accepted - target
accepted_events, checked_events = native.get("accepted_events"), native.get("checked_events")
residual_events = None
if mode == "full" and accepted_events is not None and checked_events is not None:
    residual_events = accepted_events - checked_events
record = {
    "format_version": 1, "clock": "CLOCK_MONOTONIC",
    "checker_enabled": bool(mode), "mode": mode or "baseline-original",
    "workload_cutoff_monotonic_ns": cutoff_ns,
    "observation_monotonic_ns": sample.get("monotonic_ns"),
    "observation_delay_ns": sample.get("monotonic_ns") - cutoff_ns,
    "scrape_success": sample.get("scrape_success"),
    "run_mode": sample.get("mode"), "ordering_protocol": sample.get("ordering_protocol"),
    "adapter_metrics": adapter, "native_metrics": native, "verdict": sample.get("verdict"),
    "residual_transactions": residual_tx, "residual_events": residual_events,
}
pathlib.Path(output_name).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
if not record["scrape_success"] or record["observation_delay_ns"] < 0:
    raise SystemExit("invalid checker cutoff sample")
PY
  }

  stop_result_watcher() {
    if [[ -n "$RESULT_WATCHER_PID" ]]; then
      kill -TERM "$RESULT_WATCHER_PID" 2>/dev/null || true
      wait "$RESULT_WATCHER_PID" 2>/dev/null || true
      RESULT_WATCHER_PID=
    fi
  }

  shutdown_unexpected_server() {
    if [[ -z "$TIDB_PID" ]]; then
      return
    fi
    if kill -0 "$TIDB_PID" 2>/dev/null; then
      kill -TERM "$TIDB_PID" 2>/dev/null || true
    fi
    set +e
    wait "$TIDB_PID"
    local status=$?
    set -e
    if [[ "$SERVER_PHASE" = bootstrap && -z "$BOOTSTRAP_SERVER_EXIT_STATUS" ]]; then
      BOOTSTRAP_SERVER_EXIT_STATUS=$status
    elif [[ "$SERVER_PHASE" = main && -z "$MAIN_SERVER_EXIT_STATUS" ]]; then
      MAIN_SERVER_EXIT_STATUS=$status
      [[ -n "$SERVER_SHUTDOWN_START_NS" ]] ||
        SERVER_SHUTDOWN_START_NS=$(monotonic_ns)
      TIDB_SERVER_EXIT_NS=$(monotonic_ns)
    elif [[ "$SERVER_PHASE" = post-check && -z "$POST_CHECK_SERVER_EXIT_STATUS" ]]; then
      POST_CHECK_SERVER_EXIT_STATUS=$status
    fi
    TIDB_PID=
  }

  attempt_cleanup() {
    local status=$?
    # The caller has a dynamically scoped local named attempt_dir. Bash unwinds
    # that local before this EXIT trap runs, so restore the subshell-owned path.
    attempt_dir=$ATTEMPT_DIR_PATH
    trap - EXIT ERR INT TERM
    stop_pidstat
    stop_checker_telemetry
    shutdown_unexpected_server
    stop_resource_monitor
    stop_result_watcher
    write_timing
    if [[ "$ATTEMPT_SUCCESS" = true && "$status" -eq 0 ]]; then
      write_runner_status complete
    else
      [[ -n "$FAILURE_REASON" ]] ||
        FAILURE_REASON="attempt exited with status $status during $CURRENT_PHASE"
      write_runner_status failed
    fi
    exit "$status"
  }

  attempt_error() {
    local status=$1
    local line=$2
    local command=$3
    FAILURE_REASON="phase $CURRENT_PHASE failed at line $line with status $status: $command"
    return "$status"
  }

  trap 'attempt_error "$?" "$LINENO" "$BASH_COMMAND"' ERR
  trap attempt_cleanup EXIT
  trap 'FAILURE_REASON="interrupted"; exit 130' INT
  trap 'FAILURE_REASON="terminated"; exit 143' TERM

  record_process_snapshot() {
    local output=$1
    {
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

  start_resource_monitor() {
    python3 - \
      "$TIDB_PID" \
      "$attempt_dir/tidb-resource.json" \
      "$attempt_dir/tidb-resource-samples.txt" <<'PY' &
import json
import os
import pathlib
import signal
import sys
import time

pid = int(sys.argv[1])
output = pathlib.Path(sys.argv[2])
samples_path = pathlib.Path(sys.argv[3])
stop = False

def request_stop(_signal, _frame):
    global stop
    stop = True

signal.signal(signal.SIGTERM, request_stop)
signal.signal(signal.SIGINT, request_stop)
ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
first_cpu = None
last_cpu = None
maximum_hwm = 0
maximum_rss = 0
samples = 0
started = time.monotonic_ns()
with samples_path.open("w") as sample_file:
    sample_file.write("monotonic_ns vmrss_kib vmhwm_kib process_cpu_ticks\n")
    while not stop:
        try:
            status_text = pathlib.Path(f"/proc/{pid}/status").read_text()
            stat_fields = pathlib.Path(f"/proc/{pid}/stat").read_text().split()
        except OSError:
            break
        rss = hwm = 0
        for line in status_text.splitlines():
            if line.startswith("VmHWM:"):
                hwm = int(line.split()[1])
            elif line.startswith("VmRSS:"):
                rss = int(line.split()[1])
        cpu = int(stat_fields[13]) + int(stat_fields[14])
        first_cpu = cpu if first_cpu is None else first_cpu
        last_cpu = cpu
        maximum_hwm = max(maximum_hwm, hwm)
        now = time.monotonic_ns()
        maximum_rss = max(maximum_rss, rss)
        sample_file.write(f"{now} {rss} {hwm} {cpu}\n")
        sample_file.flush()
        samples += 1
        time.sleep(0.1)
ended = time.monotonic_ns()
record = {
    "clock": "CLOCK_MONOTONIC",
    "started_ns": started,
    "ended_ns": ended,
    "samples": samples,
    "maximum_rss_kib": maximum_rss,
    "maximum_hwm_kib": maximum_hwm,
    "process_cpu_seconds": (
        None
        if first_cpu is None or last_cpu is None
        else (last_cpu - first_cpu) / ticks
    ),
}
output.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
PY
    RESOURCE_MONITOR_PID=$!
  }

  start_result_watcher() {
    python3 - \
      "$attempt_dir/checker-result.json" \
      "$attempt_dir/checker-result-available-ns.txt" \
      "$attempt_dir/checker-result-watcher-error.txt" <<'PY' &
import json
import pathlib
import signal
import sys
import time

result_path = pathlib.Path(sys.argv[1])
output_path = pathlib.Path(sys.argv[2])
error_path = pathlib.Path(sys.argv[3])
stop = False

def request_stop(_signal, _frame):
    global stop
    stop = True

signal.signal(signal.SIGTERM, request_stop)
signal.signal(signal.SIGINT, request_stop)
while not stop:
    try:
        value = json.loads(result_path.read_text())
        if value.get("format_version") == 1 and value.get("outcome") != "in-progress":
            observed = time.monotonic_ns()
            temporary = output_path.with_suffix(".tmp")
            temporary.write_text(str(observed) + "\n")
            temporary.replace(output_path)
            raise SystemExit(0)
    except (OSError, json.JSONDecodeError):
        pass
    time.sleep(0.005)
error_path.write_text("watcher stopped before a final checker result was observed\n")
PY
    RESULT_WATCHER_PID=$!
  }

  start_server() {
    local phase=$1
    local log_prefix=$2
    ensure_ports_free
    mkdir -p "$attempt_dir/$log_prefix-tmp"
    taskset -c "$TIDB_CPUSET" "$server_path" \
      --store unistore \
      --config "$attempt_dir/tidb.toml" \
      --path "$attempt_dir/store" \
      --temp-dir "$attempt_dir/$log_prefix-tmp" \
      --host "$SQL_HOST" -P "$SQL_PORT" \
      --status-host "$SQL_HOST" --status "$STATUS_PORT" \
      --log-file "$attempt_dir/$log_prefix-server.txt" \
      >"$attempt_dir/$log_prefix-server-stdout.txt" 2>&1 &
    TIDB_PID=$!
    SERVER_PHASE=$phase
    wait_for_server_pid \
      "$TIDB_PID" "$expected_commit" "$attempt_dir/$log_prefix-status.json" ||
      die "$phase TiDB server did not become ready"
  }


  stop_bootstrap_server() {
    kill -TERM "$TIDB_PID"
    set +e
    wait "$TIDB_PID"
    BOOTSTRAP_SERVER_EXIT_STATUS=$?
    set -e
    TIDB_PID=
    (( BOOTSTRAP_SERVER_EXIT_STATUS == 0 ))
  }

  stop_main_server() {
    if [[ -n "$runtime_mode" && -z "$CHECKER_RESULT_AVAILABLE_NS" ]]; then
      start_result_watcher
    fi
    SERVER_SHUTDOWN_START_NS=$(monotonic_ns)
    kill -TERM "$TIDB_PID"
    set +e
    wait "$TIDB_PID"
    MAIN_SERVER_EXIT_STATUS=$?
    set -e
    TIDB_SERVER_EXIT_NS=$(monotonic_ns)
    TIDB_PID=
    stop_resource_monitor
    if [[ -n "$runtime_mode" && -z "$CHECKER_RESULT_AVAILABLE_NS" ]]; then
      for _ in $(seq 1 200); do
        [[ -s "$attempt_dir/checker-result-available-ns.txt" ]] && break
        sleep 0.01
      done
      if [[ -s "$attempt_dir/checker-result-available-ns.txt" ]]; then
        CHECKER_RESULT_AVAILABLE_NS=$(tr -d '[:space:]' <"$attempt_dir/checker-result-available-ns.txt")
      elif python3 - "$attempt_dir/checker-result.json" <<'PY'
import json, pathlib, sys
value = json.loads(pathlib.Path(sys.argv[1]).read_text())
if value.get("format_version") != 1 or value.get("outcome") == "in-progress":
    raise SystemExit(1)
PY
      then
        CHECKER_RESULT_AVAILABLE_NS=$(monotonic_ns)
      fi
      stop_result_watcher
    fi
    write_timing
    (( MAIN_SERVER_EXIT_STATUS == 0 ))
  }

  stop_post_check_server() {
    kill -TERM "$TIDB_PID"
    set +e
    wait "$TIDB_PID"
    POST_CHECK_SERVER_EXIT_STATUS=$?
    set -e
    TIDB_PID=
    (( POST_CHECK_SERVER_EXIT_STATUS == 0 ))
  }

  CURRENT_PHASE=create-empty-store
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  mkdir "$attempt_dir/store"
  [[ -z "$(find "$attempt_dir/store" -mindepth 1 -print -quit)" ]] ||
    die "new per-mode UniStore directory is not empty"
  printf "%s\n" \
    "[tikv-client.copr-cache]" \
    "capacity-mb = 0" >"$attempt_dir/tidb.toml"
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  # Schema creation is an explicit DDL-only bootstrap outside point-KV
  # preparation. The checked server is activated only after this server stops.
  CURRENT_PHASE=schema-bootstrap
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  clear_online_checker_environment
  start_server bootstrap schema-bootstrap
  local command_status
  set +e
  /usr/bin/time -v -o "$attempt_dir/schema-bootstrap-time.txt" \
    taskset -c "$CLIENT_CPUSET" "$IMMUTABLE_GO_TPC" \
      --ignore-error=false --silence=false tpcc prepare \
      -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
      -T 1 --warehouses "$ACTIVE_WAREHOUSES" \
      --partition-type "$TPCC_PARTITION_TYPE" --parts "$TPCC_PARTS" --use-fk="$TPCC_USE_FK" \
      --ddl-only --no-check --retry-count 0 \
      --conn-params "$DSN_PARAMS" --output json \
      >"$attempt_dir/schema-bootstrap.txt" 2>&1
  command_status=$?
  set -e
  validate_prepare_output \
    "$command_status" "$attempt_dir/schema-bootstrap.txt" \
    "$attempt_dir/schema-bootstrap-validation.json"
  taskset -c "$CLIENT_CPUSET" "$TIDB_SQL" \
    -action assert-empty \
    -address "$SQL_HOST:$SQL_PORT" \
    -database "$DATABASE" \
    -params "$DSN_PARAMS" \
    >"$attempt_dir/initial-empty-counts.json" \
    2>"$attempt_dir/initial-empty-counts-error.txt"
  taskset -c "$CLIENT_CPUSET" "$TIDB_SQL" \
    -action tpcc-digest -timeout 30m \
    -address "$SQL_HOST:$SQL_PORT" \
    -database "$DATABASE" \
    -params "$DSN_PARAMS" \
    >"$attempt_dir/schema-bootstrap-digest.json" \
    2>"$attempt_dir/schema-bootstrap-digest-error.txt"
  stop_bootstrap_server
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  clear_online_checker_environment
  if [[ -n "$runtime_mode" ]]; then
    export TIDB_ONLINE_CHECKER_MODE=$runtime_mode
    export TIDB_ONLINE_CHECKER_ORDERING_PROTOCOL=serialized-global-lock
    export TIDB_ONLINE_CHECKER_LIBRARY=$NATIVE_LIBRARY
    export TIDB_ONLINE_CHECKER_RESULT_PATH=$attempt_dir/checker-result.json
    export TIDB_ONLINE_CHECKER_PRODUCER_ID=1
    export TIDB_ONLINE_CHECKER_REQUIRED_PRODUCERS=1
    export TIDB_ONLINE_CHECKER_CLUSTER_EPOCH
    TIDB_ONLINE_CHECKER_CLUSTER_EPOCH=$(date +%s%N)
    export TIDB_ONLINE_CHECKER_QUEUE_BYTES=$QUEUE_CAPACITY_BYTES
    export TIDB_ONLINE_CHECKER_CAPTURE_BYTES=$CAPTURE_CAPACITY_BYTES
    export TIDB_ONLINE_CHECKER_RETAINED_STATE_BYTES=$RETAINED_STATE_CAPACITY_BYTES
    export TIDB_ONLINE_CHECKER_QUEUE_TIMEOUT=$QUEUE_TIMEOUT
    export TIDB_ONLINE_CHECKER_CAPTURE_TIMEOUT=$CAPTURE_TIMEOUT
    export TIDB_ONLINE_CHECKER_CHUNK_SEGMENT_BYTES=$CHUNK_SEGMENT_BYTES
    export TIDB_ONLINE_CHECKER_MAX_CACHED_CHUNK_SEGMENTS=$MAX_CACHED_CHUNK_SEGMENTS
    if [[ "$DUPLICATE_READ_DIAGNOSTICS" = 1 ]]; then
      export TIDB_ONLINE_CHECKER_DUPLICATE_READ_DIAGNOSTICS=1
    fi
    # Do not set REORDER_BYTES or FRONTIER_INTERVAL: literal zero is rejected,
    # and serialized-global-lock must have no frontier/reorder configuration.
    python3 - "$attempt_dir/checker-environment.json" <<'PY'
import json
import os
import pathlib
import sys
names = [
    "TIDB_ONLINE_CHECKER_MODE",
    "TIDB_ONLINE_CHECKER_ORDERING_PROTOCOL",
    "TIDB_ONLINE_CHECKER_LIBRARY",
    "TIDB_ONLINE_CHECKER_RESULT_PATH",
    "TIDB_ONLINE_CHECKER_PRODUCER_ID",
    "TIDB_ONLINE_CHECKER_REQUIRED_PRODUCERS",
    "TIDB_ONLINE_CHECKER_CLUSTER_EPOCH",
    "TIDB_ONLINE_CHECKER_QUEUE_BYTES",
    "TIDB_ONLINE_CHECKER_CAPTURE_BYTES",
    "TIDB_ONLINE_CHECKER_RETAINED_STATE_BYTES",
    "TIDB_ONLINE_CHECKER_QUEUE_TIMEOUT",
    "TIDB_ONLINE_CHECKER_CAPTURE_TIMEOUT",
    "TIDB_ONLINE_CHECKER_CHUNK_SEGMENT_BYTES",
    "TIDB_ONLINE_CHECKER_MAX_CACHED_CHUNK_SEGMENTS",
    "TIDB_ONLINE_CHECKER_DUPLICATE_READ_DIAGNOSTICS",
]
record = {name: os.environ.get(name) for name in names}
record["TIDB_ONLINE_CHECKER_REORDER_BYTES"] = os.environ.get(
    "TIDB_ONLINE_CHECKER_REORDER_BYTES"
)
record["TIDB_ONLINE_CHECKER_FRONTIER_INTERVAL"] = os.environ.get(
    "TIDB_ONLINE_CHECKER_FRONTIER_INTERVAL"
)
pathlib.Path(sys.argv[1]).write_text(
    json.dumps(record, indent=2, sort_keys=True) + "\n"
)
PY
  else
    printf '%s\n' '{"checker":"disabled","mode":"baseline-original"}' \
      >"$attempt_dir/checker-environment.json"
  fi

  CURRENT_PHASE=start-main-server
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  start_server main main
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=verify-main-server
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  taskset -c "$CLIENT_CPUSET" "$TIDB_SQL" \
    -action ping \
    -address "$SQL_HOST:$SQL_PORT" \
    -database "$DATABASE" \
    -params "$DSN_PARAMS" \
    >"$attempt_dir/main-server-ping.txt" 2>&1
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=prepare
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  set +e
  /usr/bin/time -v -o "$attempt_dir/prepare-time.txt" \
    taskset -c "$CLIENT_CPUSET" "$IMMUTABLE_GO_TPC" \
      --ignore-error=false --silence=false tpcc prepare \
      -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
      -T 1 --warehouses "$ACTIVE_WAREHOUSES" \
      --partition-type "$TPCC_PARTITION_TYPE" --parts "$TPCC_PARTS" --use-fk="$TPCC_USE_FK" \
      --skip-ddl --seed "$PREPARE_SEED" --load-time "$PREPARE_LOAD_TIME" \
      --manifest "$attempt_dir/prepare-manifest.json" \
      --retry-count 0 --no-check \
      --conn-params "$DSN_PARAMS" --output json \
      >"$attempt_dir/prepare.txt" 2>&1
  command_status=$?
  set -e
  validate_prepare_output \
    "$command_status" "$attempt_dir/prepare.txt" \
    "$attempt_dir/prepare-validation.json"
  printf "{\"exit_status\":%d}\n" "$command_status" \
    >"$attempt_dir/prepare-status.json"

  taskset -c "$CLIENT_CPUSET" "$TIDB_SQL" \
    -action tpcc-digest -timeout 30m \
    -address "$SQL_HOST:$SQL_PORT" \
    -database "$DATABASE" \
    -params "$DSN_PARAMS" \
    >"$attempt_dir/post-prepare-digest.json" \
    2>"$attempt_dir/post-prepare-digest-error.txt"
  validate_and_compare_preparation \
    "$mode" "$attempt_dir/prepare-manifest.json" \
    "$attempt_dir/post-prepare-digest.json" \
    "$attempt_dir/schema-bootstrap-digest.json" \
    "$attempt_dir/prepare-reference-validation.json"
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=capability-gate
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  taskset -c "$CLIENT_CPUSET" "$TIDB_SQL" \
    -action tpcc-gate \
    -address "$SQL_HOST:$SQL_PORT" \
    -database "$DATABASE" \
    -params "$DSN_PARAMS" \
    >"$attempt_dir/capability-gate.json" \
    2>"$attempt_dir/capability-gate-error.txt"
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=pre-measurement-drain
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  wait_for_online_checker_drain \
    "$runtime_mode" \
    "$attempt_dir/preparation-drain.json" \
    "$attempt_dir/preparation-drain.prom"
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=stabilize
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  sleep "$ACTIVE_STABILIZE_SECONDS"
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=warmup
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  set +e
  /usr/bin/time -v -o "$attempt_dir/warmup-time.txt" \
    taskset -c "$CLIENT_CPUSET" "$IMMUTABLE_GO_TPC" \
      --ignore-error=false --silence=true --continue-on-write-conflict=true tpcc run \
      -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
      -T "$ACTIVE_THREADS" --warehouses "$ACTIVE_WAREHOUSES" \
      --partition-type "$TPCC_PARTITION_TYPE" --parts "$TPCC_PARTS" \
      --seed "$WORKLOAD_SEED" \
      --weight "$TRANSACTION_WEIGHTS" \
      --time "$ACTIVE_WARMUP_TIME" \
      --interval "$ACTIVE_OUTPUT_INTERVAL" \
      --conn-params "$DSN_PARAMS" --output json \
      >"$attempt_dir/warmup.txt" 2>&1
  command_status=$?
  set -e
  validate_go_tpc_run \
    "$ACTIVE_WARMUP_TIME" \
    "$ACTIVE_OUTPUT_INTERVAL" \
    "$command_status" \
    "$attempt_dir/warmup.txt" \
    "$attempt_dir/warmup-validation.json" \
    "$TRANSACTION_WEIGHTS"
  printf '{"exit_status":%d}\n' "$command_status" \
    >"$attempt_dir/warmup-status.json"
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=pre-workload-drain
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  wait_for_online_checker_drain \
    "$runtime_mode" \
    "$attempt_dir/warmup-drain.json" \
    "$attempt_dir/warmup-drain.prom"
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=workload
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  record_process_snapshot "$attempt_dir/tidb-proc-before-workload.txt"
  start_resource_monitor
  pidstat -h -u -r -d -p "$TIDB_PID" "$MONITOR_INTERVAL_SECONDS" \
    >"$attempt_dir/tidb-pidstat.txt" 2>&1 &
  PIDSTAT_PID=$!
  start_checker_telemetry
  WORKLOAD_START_NS=$(monotonic_ns)
  set +e
  /usr/bin/time -v -o "$attempt_dir/workload-time.txt" \
    taskset -c "$CLIENT_CPUSET" "$IMMUTABLE_GO_TPC" \
      --ignore-error=false --silence=true --continue-on-write-conflict=true tpcc run \
      -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
      -T "$ACTIVE_THREADS" --warehouses "$ACTIVE_WAREHOUSES" \
      --partition-type "$TPCC_PARTITION_TYPE" --parts "$TPCC_PARTS" \
      --seed "$WORKLOAD_SEED" \
      --weight "$TRANSACTION_WEIGHTS" \
      --time "$ACTIVE_RUN_TIME" \
      --interval "$ACTIVE_OUTPUT_INTERVAL" \
      --conn-params "$DSN_PARAMS" --output json \
      >"$attempt_dir/workload.txt" 2>&1
  command_status=$?
  set -e
  WORKLOAD_CLIENT_EXIT_NS=$(monotonic_ns)
  capture_checker_workload_cutoff "$WORKLOAD_CLIENT_EXIT_NS"
  stop_checker_telemetry
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=post-workload-drain
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  wait_for_online_checker_drain \
    "$runtime_mode" "$attempt_dir/workload-drain.json" \
    "$attempt_dir/workload-drain.prom" "$attempt_dir/workload-cutoff.json"
  mapfile -t DRAIN_TIMESTAMPS < <(python3 - "$attempt_dir/workload-drain.json" <<'PY'
import json, pathlib, sys
value = json.loads(pathlib.Path(sys.argv[1]).read_text())
for name in ("drain_started_monotonic_ns", "drain_complete_monotonic_ns",
             "live_prefix_verdict_monotonic_ns", "cutoff_observation_delay_ns"):
    item = value.get(name)
    print("" if item is None else item)
PY
)
  CHECKER_DRAIN_STARTED_NS=${DRAIN_TIMESTAMPS[0]}
  CHECKER_DRAIN_COMPLETE_NS=${DRAIN_TIMESTAMPS[1]}
  LIVE_PREFIX_VERDICT_NS=${DRAIN_TIMESTAMPS[2]}
  CUTOFF_OBSERVATION_DELAY_NS=${DRAIN_TIMESTAMPS[3]}
  if [[ -n "$runtime_mode" ]]; then
    CHECKER_COMPLETION_NS=$CHECKER_DRAIN_COMPLETE_NS
    CHECKER_RESULT_AVAILABLE_NS=$CHECKER_COMPLETION_NS
  fi
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0
  stop_pidstat
  printf '%s\n' "$command_status" >"$attempt_dir/workload-exit-status.txt"
  printf '{"exit_status":%d}\n' "$command_status" >"$attempt_dir/workload-status.json"
  record_process_snapshot "$attempt_dir/tidb-proc-after-workload.txt"
  cp "$attempt_dir/tidb-proc-after-workload.txt" "$attempt_dir/tidb-proc-after.txt"

  CURRENT_PHASE=workload-validation
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  validate_go_tpc_run \
    "$ACTIVE_RUN_TIME" "$ACTIVE_OUTPUT_INTERVAL" "$command_status" \
    "$attempt_dir/workload.txt" "$attempt_dir/workload-validation.json" \
    "$TRANSACTION_WEIGHTS" "$WORKLOAD_START_NS" "$WORKLOAD_CLIENT_EXIT_NS"
  GO_TPC_PASS=true
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=checker-close
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  stop_main_server
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  if [[ -z "$runtime_mode" ]]; then
    [[ ! -e "$attempt_dir/checker-result.json" ]] ||
      die "pristine baseline unexpectedly produced a checker result"
    printf '%s\n' \
      '{"passed":true,"mode":"baseline-original","checker":"disabled"}' \
      >"$attempt_dir/checker-result-validation.json"
  else
    [[ -n "$CHECKER_RESULT_AVAILABLE_NS" ]] ||
      die "final checker result was not observed"
    validate_checker_result \
      "$runtime_mode" \
      "$attempt_dir/checker-result.json" \
      "$attempt_dir/checker-result-validation.json"
  fi
  write_timing

  # The post-check starts only after every online-checker timing endpoint was
  # recorded. It is deliberately excluded from end-to-verdict measurements.
  CURRENT_PHASE=post-check
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  clear_online_checker_environment
  start_server post-check post-check
  set +e
  /usr/bin/time -v -o "$attempt_dir/post-check-time.txt" \
    taskset -c "$CLIENT_CPUSET" "$IMMUTABLE_GO_TPC" \
      --ignore-error=false --silence=false tpcc check \
      -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
      -T 1 --warehouses "$ACTIVE_WAREHOUSES" \
      --partition-type "$TPCC_PARTITION_TYPE" --parts "$TPCC_PARTS" \
      --conn-params "$DSN_PARAMS" --output json \
      >"$attempt_dir/post-check.txt" 2>&1
  command_status=$?
  set -e
  validate_post_check \
    "$ACTIVE_WAREHOUSES" \
    "$command_status" \
    "$attempt_dir/post-check.txt" \
    "$attempt_dir/post-check-validation.json"
  stop_post_check_server
  validate_server_logs "$mode" "$attempt_dir" "$attempt_dir/server-log-validation.json"
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  lscpu >"$attempt_dir/lscpu.txt"
  uname -a >"$attempt_dir/uname.txt"
  df -B1 "$attempt_dir" >"$attempt_dir/filesystem.txt"
  python3 - "$attempt_dir/completed.json" <<'PY'
import datetime
import json
import pathlib
import sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({
    "format_version": 1,
    "status": "complete",
    "completed_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
}, indent=2, sort_keys=True) + "\n")
PY
  ATTEMPT_SUCCESS=true
)

refresh_analysis() {
  local suite_root=$1
  local completed_matrix=$2
  local analyzer_status

  echo "Refreshing analysis after $completed_matrix mode matrix"
  set +e
  python3 "$IMMUTABLE_ANALYZER" "$suite_root"
  analyzer_status=$?
  set -e
  if (( analyzer_status != 0 )); then
    echo \
      "WARNING: analyzer exited with status $analyzer_status after $completed_matrix" \
      >&2
  fi
}

run_suite() {
  local suite_root=$1
  local suite_trials=$2
  local suite_warmup=$3
  local suite_run=$4
  local suite_warehouses=$5
  local suite_threads=$6
  local suite_prepare_threads=$7
  local suite_interval=$8
  local suite_stabilize=$9

  ACTIVE_SUITE_ROOT=$suite_root
  ACTIVE_WARMUP_TIME=$suite_warmup
  ACTIVE_RUN_TIME=$suite_run
  ACTIVE_WAREHOUSES=$suite_warehouses
  ACTIVE_THREADS=$suite_threads
  ACTIVE_PREPARE_THREADS=$suite_prepare_threads
  ACTIVE_OUTPUT_INTERVAL=$suite_interval
  ACTIVE_STABILIZE_SECONDS=$suite_stabilize
  export ACTIVE_SUITE_ROOT ACTIVE_WARMUP_TIME ACTIVE_RUN_TIME
  export ACTIVE_WAREHOUSES ACTIVE_THREADS ACTIVE_PREPARE_THREADS
  export ACTIVE_OUTPUT_INTERVAL ACTIVE_STABILIZE_SECONDS

  mkdir -p "$suite_root/trials"
  local trial_number trial_name trial_dir mode mode_dir
  local attempt_name attempt_dir status
  if [[ "$RESUME" = true ]]; then
    echo "Refreshing analyzer validity before resume selection: $suite_root"
    python3 "$IMMUTABLE_ANALYZER" "$suite_root" >/dev/null ||
      die "cannot refresh analyzer evidence before resume: $suite_root"
  fi
  for ((trial_number = 1; trial_number <= suite_trials; ++trial_number)); do
    printf -v trial_name 'trial-%03d' "$trial_number"
    trial_dir="$suite_root/trials/$trial_name"
    mkdir -p "$trial_dir"
    for mode in "${MODES[@]}"; do
      mode_dir="$trial_dir/$mode"
      mkdir -p "$mode_dir"
      if [[ "$RESUME" = true ]] &&
        analyzer_attempt_is_valid "$suite_root" "$trial_name" "$mode" >/dev/null; then
        echo "Skipping valid completed $trial_name/$mode"
        continue
      fi
      attempt_name=$(next_numbered_directory "$mode_dir" "attempt")
      attempt_dir="$mode_dir/$attempt_name"
      echo "Running $trial_name/$mode/$attempt_name"
      set +e
      run_attempt "$trial_name" "$mode" "$attempt_name" "$attempt_dir"
      status=$?
      set -e
      if (( status != 0 )); then
        echo "WARNING: $trial_name/$mode/$attempt_name failed with status $status" >&2
      fi
    done
    refresh_analysis "$suite_root" "$trial_name"
  done
}

suite_is_valid() {
  local suite_root=$1
  local expected_trials=$2
  python3 "$IMMUTABLE_ANALYZER" "$suite_root" >/dev/null || return 1
  python3 - "$suite_root" "$expected_trials" <<'PY'
import csv
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
expected_trials = int(sys.argv[2])
experiment_path = root / "experiment.json"
if not experiment_path.is_file():
    raise SystemExit("experiment.json is missing")
experiment = json.loads(experiment_path.read_text())
expected_modes = experiment.get("modes")
allowed_mode_matrices = [
    [
        "baseline-original", "serialized-capture-only",
        "serialized-order-only", "serialized-full",
    ],
    ["baseline-original", "serialized-full"],
]
if expected_modes not in allowed_mode_matrices:
    raise SystemExit(f"unsupported experiment mode matrix: {expected_modes}")
if experiment.get("mode_order") != expected_modes:
    raise SystemExit("experiment mode_order does not match modes")
summary_path = root / "tpcc-summary.json"
if summary_path.is_file():
    try:
        summary = json.loads(summary_path.read_text())
    except json.JSONDecodeError:
        summary = {}
    if summary.get("all_valid") is False:
        raise SystemExit(1)
    if summary.get("modes") != expected_modes:
        raise SystemExit("summary modes do not match experiment modes")
    if summary.get("expected_trials") != expected_trials:
        raise SystemExit("summary trial count does not match")
    if summary.get("valid_run_count") != expected_trials * len(expected_modes):
        raise SystemExit("summary valid run count does not match mode matrix")
else:
    raise SystemExit("tpcc-summary.json was not generated")

performance_path = root / "tpcc-performance.csv"
if not performance_path.is_file():
    raise SystemExit("tpcc-performance.csv was not generated")
performance_rows = list(csv.DictReader(performance_path.open(newline="")))
if [row.get("mode") for row in performance_rows] != expected_modes:
    raise SystemExit("tpcc-performance.csv modes do not match experiment modes")
for row in performance_rows:
    try:
        valid_trials = int(row.get("valid_trial_count", ""))
        invalid_trials = int(row.get("invalid_trial_count", ""))
    except ValueError:
        raise SystemExit("tpcc-performance.csv trial counts are invalid")
    if valid_trials != expected_trials or invalid_trials != 0:
        raise SystemExit(
            "tpcc-performance.csv does not contain a complete valid matrix"
        )

csv_path = root / "tpcc-trials.csv"
if not csv_path.is_file():
    raise SystemExit("tpcc-trials.csv was not generated")
rows = list(csv.DictReader(csv_path.open(newline="")))
if len(rows) != expected_trials * len(expected_modes):
    raise SystemExit("tpcc-trials.csv row count does not match mode matrix")
selected = {}
for row in rows:
    trial = row.get("trial", "")
    mode = row.get("mode", "")
    valid = row.get("valid", "").strip().lower() in {"1", "true", "yes", "valid"}
    if trial and mode:
        if (trial, mode) in selected:
            raise SystemExit(f"duplicate analyzer row: {trial}/{mode}")
        selected[(trial, mode)] = valid
for number in range(1, expected_trials + 1):
    trial = f"trial-{number:03d}"
    for mode in expected_modes:
        if selected.get((trial, mode)) is not True:
            raise SystemExit(f"invalid or missing analyzer row: {trial}/{mode}")
PY
}

write_valid_marker() {
  local suite_root=$1
  local output=$2
  python3 - "$suite_root/experiment.json" "$output" <<'PY'
import hashlib
import json
import pathlib
import sys

manifest = pathlib.Path(sys.argv[1])
pathlib.Path(sys.argv[2]).write_text(json.dumps({
    "format_version": 1,
    "status": "pass",
    "experiment_sha256": hashlib.sha256(manifest.read_bytes()).hexdigest(),
}, indent=2, sort_keys=True) + "\n")
PY
}

build_artifacts
load_artifact_paths

if [[ "$SMOKE" = true ]]; then
  SMOKE_WARMUP_TIME=${SMOKE_WARMUP_TIME:-5s}
  SMOKE_RUN_TIME=${SMOKE_RUN_TIME:-10s}
  SMOKE_OUTPUT_INTERVAL=${SMOKE_OUTPUT_INTERVAL:-1s}
  SMOKE_STABILIZE_SECONDS=${SMOKE_STABILIZE_SECONDS:-1}
  SMOKE_THREADS=${SMOKE_THREADS:-2}
  SMOKE_PREPARE_THREADS=${SMOKE_PREPARE_THREADS:-1}
  write_smoke_experiment "$RESULT_ROOT"
  run_suite \
    "$RESULT_ROOT" \
    1 \
    "$SMOKE_WARMUP_TIME" \
    "$SMOKE_RUN_TIME" \
    "$SMOKE_WAREHOUSES" \
    "$SMOKE_THREADS" \
    "$SMOKE_PREPARE_THREADS" \
    "$SMOKE_OUTPUT_INTERVAL" \
    "$SMOKE_STABILIZE_SECONDS"
  if suite_is_valid "$RESULT_ROOT" 1; then
    write_valid_marker "$RESULT_ROOT" "$RESULT_ROOT/smoke-passed.json"
    echo "${#MODES[@]}-mode smoke suite passed: $RESULT_ROOT"
    exit 0
  fi
  die "${#MODES[@]}-mode smoke suite failed; inspect $RESULT_ROOT/tpcc-trials.csv"
fi

write_requested_experiment "$RESULT_ROOT"

SMOKE_ROOT="$RESULT_ROOT/smoke"
SMOKE_WARMUP_TIME=${SMOKE_WARMUP_TIME:-5s}
SMOKE_RUN_TIME=${SMOKE_RUN_TIME:-10s}
SMOKE_OUTPUT_INTERVAL=${SMOKE_OUTPUT_INTERVAL:-1s}
SMOKE_STABILIZE_SECONDS=${SMOKE_STABILIZE_SECONDS:-1}
SMOKE_THREADS=${SMOKE_THREADS:-2}
SMOKE_PREPARE_THREADS=${SMOKE_PREPARE_THREADS:-1}
write_smoke_experiment "$SMOKE_ROOT"

if [[ "$RESUME" = true && -s "$SMOKE_ROOT/smoke-passed.json" ]] &&
  suite_is_valid "$SMOKE_ROOT" 1; then
  echo "Skipping exact, valid ${#MODES[@]}-mode smoke suite: $SMOKE_ROOT"
else
  run_suite \
    "$SMOKE_ROOT" \
    1 \
    "$SMOKE_WARMUP_TIME" \
    "$SMOKE_RUN_TIME" \
    "$SMOKE_WAREHOUSES" \
    "$SMOKE_THREADS" \
    "$SMOKE_PREPARE_THREADS" \
    "$SMOKE_OUTPUT_INTERVAL" \
    "$SMOKE_STABILIZE_SECONDS"
  if ! suite_is_valid "$SMOKE_ROOT" 1; then
    die "${#MODES[@]}-mode smoke suite failed; long trials were not started"
  fi
  write_valid_marker "$SMOKE_ROOT" "$SMOKE_ROOT/smoke-passed.json"
fi

run_suite \
  "$RESULT_ROOT" \
  "$TRIALS" \
  "$WARMUP_TIME" \
  "$RUN_TIME" \
  "$WAREHOUSES" \
  "$THREADS" \
  "$PREPARE_THREADS" \
  "$OUTPUT_INTERVAL" \
  "$STABILIZE_SECONDS"

echo "Trial CSV:   $RESULT_ROOT/tpcc-trials.csv"
echo "Performance CSV: $RESULT_ROOT/tpcc-performance.csv"
echo "Summary CSV: $RESULT_ROOT/tpcc-summary.csv"
echo "Summary JSON: $RESULT_ROOT/tpcc-summary.json"
echo "Report:      $RESULT_ROOT/tpcc-report.md"

if suite_is_valid "$RESULT_ROOT" "$TRIALS"; then
  exit 0
fi
die "one or more trial/mode runs are invalid; all evidence was retained"
