#!/usr/bin/env bash

set -Eeuo pipefail

readonly PRISTINE_TIDB_COMMIT="b94006d3abffd428634ecc5c499f4bcb7b4d06d1"
readonly WORKLOAD_LABEL="TPC-C NewOrder-only"
readonly -a MODES=(
  "baseline-original"
  "serialized-capture-only"
  "serialized-order-only"
  "serialized-full"
)

usage() {
  cat <<'EOF'
Usage:
  run_tpcc_global_lock_comparison.sh \
    --trials 3 \
    --warmup-time 60s \
    --run-time 600s \
    --warehouses 8 \
    --threads 16 \
    --result-root /absolute/path/to/results

Required:
  --result-root PATH       Absolute, append-only result root.

Workload:
  --trials N               Paired four-mode trials (default: 3).
  --warmup-time DURATION   Unmeasured go-tpc warm-up (default: 60s).
  --run-time DURATION      Measured go-tpc duration (default: 600s).
  --warehouses N           TPC-C warehouses (default: 8).
  --threads N              Measured/warm-up client threads (default: 16).
  --prepare-threads N      Data-load threads (default: warehouses).
  --output-interval D      go-tpc reporting interval (default: 10s).
  --stabilize-seconds N    Pause between load and warm-up (default: 30).

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
  --smoke                  Run only a fast one-trial, four-mode smoke suite.
  --resume                 Require an exact manifest match; skip valid modes
                           and append a new attempt for invalid/incomplete ones.
  -h, --help               Show this help.

A normal invocation automatically runs and gates on a short four-mode suite
under RESULT_ROOT/smoke before starting the requested trials. Runs are always
sequential. The pristine TiDB tree is never switched or reset.
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

TRIALS=3
WARMUP_TIME=60s
RUN_TIME=600s
WAREHOUSES=8
THREADS=16
PREPARE_THREADS=
OUTPUT_INTERVAL=10s
STABILIZE_SECONDS=30
MONITOR_INTERVAL_SECONDS=1
BUILD_JOBS=2

SQL_HOST=127.0.0.1
SQL_PORT=4400
STATUS_PORT=14080
DATABASE=tpcc
TIDB_CPUSET=
CLIENT_CPUSET=

SMOKE=false
RESUME=false

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
    --smoke)
      SMOKE=true
      shift
      ;;
    --resume)
      RESUME=true
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

[[ -n "$RESULT_ROOT" ]] || die "--result-root is required"
[[ "$RESULT_ROOT" = /* ]] || die "--result-root must be absolute"
CHECKER_ROOT=$(realpath "$CHECKER_ROOT")
TIDB_ROOT=$(realpath "$TIDB_ROOT")
ANALYZER=$(realpath "$ANALYZER")
RESULT_ROOT=$(realpath -m "$RESULT_ROOT")
BASELINE_WORKTREE=$(realpath -m "$BASELINE_WORKTREE")
[[ "$BASELINE_WORKTREE" = /tmp/* && "$BASELINE_WORKTREE" != /tmp ]] ||
  die "--baseline-worktree must resolve to a specific directory under /tmp"

if [[ -z "$GO_TPC" ]]; then
  GO_TPC=$(command -v go-tpc 2>/dev/null || true)
  if [[ -z "$GO_TPC" && -x /home/dyf/go/bin/go-tpc ]]; then
    GO_TPC=/home/dyf/go/bin/go-tpc
  fi
fi
[[ -n "$GO_TPC" ]] || die "go-tpc was not found; pass --go-tpc"
GO_TPC=$(realpath "$GO_TPC")

[[ -n "$PREPARE_THREADS" ]] || PREPARE_THREADS=$WAREHOUSES

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

for item in \
  "trials:$TRIALS" \
  "warehouses:$WAREHOUSES" \
  "threads:$THREADS" \
  "prepare threads:$PREPARE_THREADS" \
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
validate_duration "queue timeout" "$QUEUE_TIMEOUT"
validate_duration "capture timeout" "$CAPTURE_TIMEOUT"

for command in \
  bash cmake cp curl df git go lscpu make ninja pidstat python3 \
  realpath sha256sum ss taskset uname /usr/bin/time
do
  command -v "$command" >/dev/null || die "required command not found: $command"
done
[[ -x "$GO_TPC" ]] || die "go-tpc is not executable: $GO_TPC"
[[ -f "$ANALYZER" ]] || die "analyzer not found: $ANALYZER"
[[ -f "$CHECKER_ROOT/online/CMakeLists.txt" ]] ||
  die "checker online CMake project not found under $CHECKER_ROOT"
[[ -f "$CHECKER_ROOT/online/tools/tidb_sql.go" ]] ||
  die "TiDB SQL helper source not found"
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

readonly DSN_PARAMS="tidb_enable_async_commit=0&tidb_enable_1pc=0&tidb_txn_mode=optimistic&tidb_low_resolution_tso=0"
readonly TRUNCATE_FILES="$CHECKER_ROOT/online/scripts/sql/truncate_customer.sql,$CHECKER_ROOT/online/scripts/sql/truncate_district.sql,$CHECKER_ROOT/online/scripts/sql/truncate_history.sql,$CHECKER_ROOT/online/scripts/sql/truncate_item.sql,$CHECKER_ROOT/online/scripts/sql/truncate_new_order.sql,$CHECKER_ROOT/online/scripts/sql/truncate_order_line.sql,$CHECKER_ROOT/online/scripts/sql/truncate_orders.sql,$CHECKER_ROOT/online/scripts/sql/truncate_stock.sql,$CHECKER_ROOT/online/scripts/sql/truncate_warehouse.sql"

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
    TIDB_ONLINE_CHECKER_MAX_CACHED_CHUNK_SEGMENTS
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

tree_fingerprint() {
  python3 - "$1" <<'PY'
import hashlib
import os
import pathlib
import stat
import sys

root = pathlib.Path(sys.argv[1])
digest = hashlib.sha256()
count = 0
total = 0
for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
    relative = path.relative_to(root).as_posix()
    metadata = path.lstat()
    digest.update(relative.encode())
    digest.update(b"\0")
    digest.update(str(stat.S_IMODE(metadata.st_mode)).encode())
    digest.update(b"\0")
    if path.is_symlink():
        digest.update(b"L")
        digest.update(os.readlink(path).encode())
    elif path.is_file():
        digest.update(b"F")
        count += 1
        total += metadata.st_size
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    elif path.is_dir():
        digest.update(b"D")
    digest.update(b"\0")
print(f"{digest.hexdigest()} {count} {total}")
PY
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
    "$GO_TPC" <<'PY'
import hashlib
import json
import pathlib
import subprocess
import sys

index_path, checker, tidb, runner, analyzer, go_tpc = sys.argv[1:]
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

def diff_sha(repo):
    return hashlib.sha256(
        git(repo, "diff", "--binary", "--no-ext-diff", "HEAD", binary=True)
    ).hexdigest()

errors = []
inputs = index["inputs"]
for name, path in (("runner", runner), ("analyzer", analyzer), ("go_tpc", go_tpc)):
    got = sha(path)
    if got != inputs[name]["sha256"]:
        errors.append(f"{name} changed: {got} != {inputs[name]['sha256']}")
for name, repo in (("checker", checker), ("instrumented_tidb", tidb)):
    expected = inputs[name]
    head = git(repo, "rev-parse", "HEAD").strip()
    changed = diff_sha(repo)
    if head != expected["commit"] or changed != expected["diff_sha256"]:
        errors.append(
            f"{name} source changed: commit={head}, diff_sha256={changed}"
        )
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

  local checker_commit checker_diff tidb_commit tidb_diff
  checker_commit=$(git -C "$CHECKER_ROOT" rev-parse HEAD)
  checker_diff=$(git_diff_sha256 "$CHECKER_ROOT")
  tidb_commit=$(git -C "$TIDB_ROOT" rev-parse HEAD)
  tidb_diff=$(git_diff_sha256 "$TIDB_ROOT")

  {
    echo "checker_commit=$checker_commit"
    echo "checker_diff_sha256=$checker_diff"
    echo "instrumented_tidb_commit=$tidb_commit"
    echo "instrumented_tidb_diff_sha256=$tidb_diff"
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
    go build -trimpath -o "$build_dir/tidb-sql" \
      "$CHECKER_ROOT/online/tools/tidb_sql.go"
  ) >"$build_dir/tidb-sql-build.txt" 2>&1

  cp "$GO_TPC" "$build_dir/go-tpc"
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

  python3 - \
    "$build_dir" \
    "$checker_commit" \
    "$checker_diff" \
    "$tidb_commit" \
    "$tidb_diff" \
    "$PRISTINE_TIDB_COMMIT" \
    "$SCRIPT_PATH" \
    "$ANALYZER" \
    "$GO_TPC" >"$build_dir/manifest.json" <<'PY'
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
    pristine_commit,
    runner_source,
    analyzer_source,
    go_tpc_source,
) = sys.argv[1:]
build = pathlib.Path(build_dir).resolve()

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
    "runner": "run_tpcc_global_lock_comparison.sh",
    "analyzer": "analyze_tpcc_global_lock.py",
}
manifest = {
    "format_version": 1,
    "build_type": "Release",
    "tidb_build_command": "make server TARGET=<absolute-output>",
    "inputs": {
        "checker": {
            "commit": checker_commit,
            "diff_sha256": checker_diff,
        },
        "instrumented_tidb": {
            "commit": tidb_commit,
            "diff_sha256": tidb_diff,
        },
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
        "go_tpc": {
            "path": str(pathlib.Path(go_tpc_source).resolve()),
            "sha256": sha(go_tpc_source),
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
    for marker in ("panic:", "fatal error", "execute failed")
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

validate_truncate_output() {
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
operations = set()
for line in text.splitlines():
    stripped = line.strip()
    if not stripped.startswith("["):
        continue
    try:
        records = json.loads(stripped)
    except json.JSONDecodeError:
        continue
    if isinstance(records, list):
        operations.update(
            str(record.get("Operation"))
            for record in records
            if isinstance(record, dict) and str(record.get("Prefix", "")).strip() == "[Summary]"
        )
expected = {
    "TRUNCATE_CUSTOMER",
    "TRUNCATE_DISTRICT",
    "TRUNCATE_HISTORY",
    "TRUNCATE_ITEM",
    "TRUNCATE_NEW_ORDER",
    "TRUNCATE_ORDER_LINE",
    "TRUNCATE_ORDERS",
    "TRUNCATE_STOCK",
    "TRUNCATE_WAREHOUSE",
}
passed = status == 0 and finished == 1 and operations == expected
result = {
    "passed": passed,
    "exit_status": status,
    "finished_marker_count": finished,
    "summary_operations": sorted(operations),
    "expected_operations": sorted(expected),
}
pathlib.Path(sys.argv[3]).write_text(
    json.dumps(result, indent=2, sort_keys=True) + "\n"
)
if not passed:
    raise SystemExit("TPC-C schema truncation validation failed")
PY
}

SNAPSHOT_INDEX="$RESULT_ROOT/empty-schema-snapshot.json"

verify_existing_snapshot() {
  python3 - "$SNAPSHOT_INDEX" <<'PY'
import json
import pathlib
import sys

index = json.loads(pathlib.Path(sys.argv[1]).read_text())
store = pathlib.Path(index["store_path"])
if not store.is_dir():
    raise SystemExit(f"snapshot store is missing: {store}")
PY
  local expected actual
  expected=$(python3 - "$SNAPSHOT_INDEX" <<'PY'
import json
import pathlib
import sys
print(json.loads(pathlib.Path(sys.argv[1]).read_text())["tree_sha256"])
PY
)
  read -r actual _ < <(tree_fingerprint "$SNAPSHOT_STORE")
  [[ "$actual" = "$expected" ]] || die "empty-schema snapshot fingerprint changed"
}

create_empty_schema_snapshot() {
  if [[ -s "$SNAPSHOT_INDEX" ]]; then
    [[ "$RESUME" = true ]] ||
      die "snapshot index already exists without --resume"
    SNAPSHOT_STORE=$(python3 - "$SNAPSHOT_INDEX" <<'PY'
import json
import pathlib
import sys
print(json.loads(pathlib.Path(sys.argv[1]).read_text())["store_path"])
PY
)
    verify_existing_snapshot
    return
  fi

  mkdir -p "$RESULT_ROOT/snapshots"
  local snapshot_name snapshot_dir snapshot_pid snapshot_status
  snapshot_name=$(next_numbered_directory "$RESULT_ROOT/snapshots" "snapshot-attempt")
  snapshot_dir="$RESULT_ROOT/snapshots/$snapshot_name"
  mkdir -p "$snapshot_dir/store" "$snapshot_dir/tmp"
  snapshot_pid=
  snapshot_status=

  snapshot_cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [[ -n "$snapshot_pid" ]] && kill -0 "$snapshot_pid" 2>/dev/null; then
      kill -TERM "$snapshot_pid" 2>/dev/null || true
      set +e
      wait "$snapshot_pid"
      snapshot_status=$?
      set -e
      printf '%s\n' "$snapshot_status" >"$snapshot_dir/server-exit-status.txt"
    fi
    exit "$status"
  }
  trap snapshot_cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM

  ensure_ports_free
  clear_online_checker_environment
  echo "Creating stopped empty-schema UniStore snapshot"
  taskset -c "$TIDB_CPUSET" "$PRISTINE_TIDB_SERVER" \
    --store unistore \
    --path "$snapshot_dir/store" \
    --temp-dir "$snapshot_dir/tmp" \
    --host "$SQL_HOST" -P "$SQL_PORT" \
    --status-host "$SQL_HOST" --status "$STATUS_PORT" \
    --log-file "$snapshot_dir/server.txt" \
    >"$snapshot_dir/server-stdout.txt" 2>&1 &
  snapshot_pid=$!
  wait_for_server_pid \
    "$snapshot_pid" "$PRISTINE_TIDB_COMMIT" "$snapshot_dir/status.json" ||
    die "snapshot TiDB server did not become ready"

  local status
  set +e
  /usr/bin/time -v -o "$snapshot_dir/prepare-time.txt" \
    taskset -c "$CLIENT_CPUSET" "$IMMUTABLE_GO_TPC" tpcc prepare \
      -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
      -T 1 --warehouses 1 --no-check \
      --conn-params "$DSN_PARAMS" --output json \
      >"$snapshot_dir/prepare.txt" 2>&1
  status=$?
  set -e
  validate_prepare_output \
    "$status" "$snapshot_dir/prepare.txt" "$snapshot_dir/prepare-validation.json"

  set +e
  /usr/bin/time -v -o "$snapshot_dir/truncate-time.txt" \
    taskset -c "$CLIENT_CPUSET" "$IMMUTABLE_GO_TPC" rawsql run \
      -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
      -T 1 --count 9 --query-files "$TRUNCATE_FILES" \
      --conn-params "$DSN_PARAMS" --output json \
      >"$snapshot_dir/truncate.txt" 2>&1
  status=$?
  set -e
  validate_truncate_output \
    "$status" "$snapshot_dir/truncate.txt" "$snapshot_dir/truncate-validation.json"

  taskset -c "$CLIENT_CPUSET" "$TIDB_SQL" \
    -action assert-empty \
    -address "$SQL_HOST:$SQL_PORT" \
    -database "$DATABASE" \
    -params "$DSN_PARAMS" \
    >"$snapshot_dir/empty-counts.json" \
    2>"$snapshot_dir/empty-counts-error.txt"

  kill -TERM "$snapshot_pid"
  set +e
  wait "$snapshot_pid"
  snapshot_status=$?
  set -e
  snapshot_pid=
  printf '%s\n' "$snapshot_status" >"$snapshot_dir/server-exit-status.txt"
  (( snapshot_status == 0 )) ||
    die "snapshot TiDB server exited with status $snapshot_status"

  local tree_hash file_count byte_count
  read -r tree_hash file_count byte_count < <(tree_fingerprint "$snapshot_dir/store")
  python3 - \
    "$snapshot_dir" \
    "$tree_hash" \
    "$file_count" \
    "$byte_count" \
    "$PRISTINE_TIDB_COMMIT" \
    "$PRISTINE_TIDB_SERVER_SHA" \
    "$DSN_PARAMS" \
    "$SQL_HOST" \
    "$SQL_PORT" \
    "$STATUS_PORT" >"$snapshot_dir/manifest.json" <<'PY'
import json
import pathlib
import sys

(
    snapshot_dir,
    tree_hash,
    file_count,
    byte_count,
    commit,
    server_sha,
    dsn,
    host,
    sql_port,
    status_port,
) = sys.argv[1:]
root = pathlib.Path(snapshot_dir).resolve()
print(json.dumps({
    "format_version": 1,
    "state": "stopped-empty-schema",
    "store": "unistore",
    "store_path": str(root / "store"),
    "tree_sha256": tree_hash,
    "file_count": int(file_count),
    "byte_count": int(byte_count),
    "created_with_tidb_commit": commit,
    "created_with_tidb_server_sha256": server_sha,
    "database": "tpcc",
    "tables": [
        "customer", "district", "history", "item", "new_order",
        "order_line", "orders", "stock", "warehouse",
    ],
    "all_table_counts": 0,
    "storage_configuration": {
        "store": "unistore",
        "host": host,
        "sql_port": int(sql_port),
        "status_port": int(status_port),
    },
    "dsn_params": dsn,
}, indent=2, sort_keys=True))
PY
  cp "$snapshot_dir/manifest.json" "$SNAPSHOT_INDEX"
  chmod 0444 "$snapshot_dir/manifest.json" "$SNAPSHOT_INDEX"
  SNAPSHOT_STORE="$snapshot_dir/store"
  trap - EXIT INT TERM
}

snapshot_sha() {
  python3 - "$SNAPSHOT_INDEX" <<'PY'
import json
import pathlib
import sys
print(json.loads(pathlib.Path(sys.argv[1]).read_text())["tree_sha256"])
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
    "$TIDB_CPUSET" \
    "$CLIENT_CPUSET" \
    "$SQL_HOST" \
    "$SQL_PORT" \
    "$STATUS_PORT" \
    "$DSN_PARAMS" \
    "$MONITOR_INTERVAL_SECONDS" \
    "$QUEUE_CAPACITY_BYTES" \
    "$CAPTURE_CAPACITY_BYTES" \
    "$RETAINED_STATE_CAPACITY_BYTES" \
    "$QUEUE_TIMEOUT" \
    "$CAPTURE_TIMEOUT" \
    "$CHUNK_SEGMENT_BYTES" \
    "$MAX_CACHED_CHUNK_SEGMENTS" \
    "$ARTIFACT_INDEX" \
    "$SNAPSHOT_INDEX" >"$expected" <<'PY'
import json
import pathlib
import sys

(
    suite_root,
    trials,
    warmup,
    run_time,
    warehouses,
    threads,
    prepare_threads,
    interval,
    stabilize,
    smoke,
    tidb_cpus,
    client_cpus,
    host,
    sql_port,
    status_port,
    dsn,
    monitor_interval,
    queue_bytes,
    capture_bytes,
    retained_bytes,
    queue_timeout,
    capture_timeout,
    segment_bytes,
    cached_segments,
    artifact_index,
    snapshot_index,
) = sys.argv[1:]
artifacts = json.loads(pathlib.Path(artifact_index).read_text())
snapshot = json.loads(pathlib.Path(snapshot_index).read_text())
manifest = {
    "format_version": 1,
    "benchmark": "TPC-C",
    "workload_label": "TPC-C NewOrder-only",
    "transaction_weights": [100, 0, 0, 0, 0],
    "suite_root": str(pathlib.Path(suite_root).resolve()),
    "smoke": smoke == "true",
    "trials": int(trials),
    "modes": [
        "baseline-original",
        "serialized-capture-only",
        "serialized-order-only",
        "serialized-full",
    ],
    "mode_order": [
        "baseline-original",
        "serialized-capture-only",
        "serialized-order-only",
        "serialized-full",
    ],
    "workload": {
        "warmup_time": warmup,
        "run_time": run_time,
        "warehouses": int(warehouses),
        "threads": int(threads),
        "prepare_threads": int(prepare_threads),
        "output_interval": interval,
        "stabilize_seconds": int(stabilize),
        "go_tpc_ignore_error": True,
        "go_tpc_silence_errors": True,
        "note": (
            "Every mode clones the same stopped empty-schema snapshot. "
            "go-tpc has no seed option, so it provides the same workload "
            "specification and cardinality, not byte-identical random inputs."
        ),
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
        "queue_capacity_bytes": int(queue_bytes),
        "capture_capacity_bytes": int(capture_bytes),
        "retained_state_capacity_bytes": int(retained_bytes),
        "reorder_capacity_bytes": 0,
        "reorder_capacity_environment": "unset",
        "frontier_interval_environment": "unset",
        "frontier_pump": False,
        "metric_scope": "epoch-wide: prepare + warm-up + measured workload",
        "queue_timeout": queue_timeout,
        "capture_timeout": capture_timeout,
        "chunk_segment_bytes": int(segment_bytes),
        "max_cached_chunk_segments": int(cached_segments),
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
        "snapshot_path": snapshot["store_path"],
        "snapshot_tree_sha256": snapshot["tree_sha256"],
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
      python3 - "$suite_root" "$ARTIFACT_INDEX" "$SNAPSHOT_INDEX" <<'PY'
import json
import pathlib
import sys

root, artifact_index, snapshot_index = map(pathlib.Path, sys.argv[1:])
allowed = {"artifacts", "build-artifacts.json", "snapshots", "empty-schema-snapshot.json"}
entries = {path.name for path in root.iterdir()}
unexpected = sorted(entries - allowed)
if unexpected:
    raise SystemExit(
        "cannot adopt interrupted setup with non-setup artifacts: "
        + ", ".join(unexpected)
    )
if not artifact_index.is_file() or not snapshot_index.is_file():
    raise SystemExit("interrupted setup is missing a verified setup index")
artifacts = json.loads(artifact_index.read_text())
snapshot = json.loads(snapshot_index.read_text())
artifact_root = (root / "artifacts").resolve()
snapshot_root = (root / "snapshots").resolve()
for artifact in artifacts.get("artifacts", {}).values():
    pathlib.Path(artifact["path"]).resolve().relative_to(artifact_root)
pathlib.Path(snapshot["store_path"]).resolve().relative_to(snapshot_root)
if snapshot.get("state") != "stopped-empty-schema":
    raise SystemExit("interrupted setup snapshot is not stopped-empty-schema")
PY
      echo "Adopting verified setup-only artifacts before creating $suite_root/experiment.json"
    fi
    mv "$expected" "$suite_root/experiment.json"
    chmod 0444 "$suite_root/experiment.json"
  fi
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
  local start_ns=${6:-}
  local exit_ns=${7:-}
  python3 - \
    "$expected_duration" \
    "$output_interval" \
    "$command_status" \
    "$input" \
    "$output" \
    "$start_ns" \
    "$exit_ns" <<'PY'
import json
import math
import pathlib
import re
import sys

expected_text, interval_text, status_text, input_path, output_path, start_ns, exit_ns = sys.argv[1:]

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
finished = sum(line.strip() == "Finished" for line in text.splitlines())
current_intervals = 0
summary = None
error_summary = None
final = None
malformed = 0
for line in text.splitlines():
    stripped = line.strip()
    if not stripped.startswith(("[{", "{")):
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
        operation = record.get("Operation")
        if prefix == "[Current]" and operation == "NEW_ORDER":
            current_intervals += 1
        elif prefix == "[Summary]" and operation == "NEW_ORDER":
            summary = record
        elif prefix == "[Summary]" and operation == "NEW_ORDER_ERR":
            error_summary = record
        elif "tpmC" in record:
            final = record

reasons = []
if command_status != 0:
    reasons.append(f"go-tpc exit status {command_status}")
if finished != 1:
    reasons.append(f"expected one Finished marker, found {finished}")
if malformed:
    reasons.append(f"found {malformed} malformed JSON candidate lines")
if summary is None or final is None:
    reasons.append("go-tpc final summary is incomplete")

summary_seconds = None
if summary is not None:
    try:
        summary_seconds = float(summary["Takes(s)"])
    except (KeyError, TypeError, ValueError):
        reasons.append("NEW_ORDER summary duration is missing")
if summary_seconds is not None:
    tolerance = max(2.0, expected * 0.01)
    if abs(summary_seconds - expected) > tolerance:
        reasons.append(
            f"summary duration {summary_seconds} differs from expected {expected}"
        )
minimum_intervals = max(1, math.floor(expected / interval) - 1)
if current_intervals < minimum_intervals:
    reasons.append(
        f"only {current_intervals} intervals; expected at least {minimum_intervals}"
    )

actual_seconds = summary_seconds
if start_ns and exit_ns:
    actual_seconds = (int(exit_ns) - int(start_ns)) / 1_000_000_000
result = {
    "passed": not reasons,
    "reason": "; ".join(reasons),
    "exit_status": command_status,
    "expected_seconds": expected,
    "actual_seconds": actual_seconds,
    "go_tpc_summary_seconds": summary_seconds,
    "current_interval_count": current_intervals,
    "minimum_interval_count": minimum_intervals,
    "finished_marker_count": finished,
    "malformed_json_record_count": malformed,
    "new_order_count": (
        int(summary["Count"]) if summary is not None and "Count" in summary else None
    ),
    "new_order_error_count": (
        int(error_summary["Count"])
        if error_summary is not None and "Count" in error_summary
        else 0
    ),
    "tpm_c": float(final["tpmC"]) if final is not None else None,
    "go_tpc_ignore_error": True,
    "go_tpc_silence_errors": True,
}
pathlib.Path(output_path).write_text(
    json.dumps(result, indent=2, sort_keys=True) + "\n"
)
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
    for marker in ("execute check failed", "panic:", "fatal error")
    if marker in lower
]
finished = sum(line.strip() == "Finished" for line in text.splitlines())
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
    "conditions_per_warehouse": 12,
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
if None not in (registered, published, aborted):
    require(registered == published + aborted, "attempt lifecycle counters do not balance")
if None not in (successful_writes, read_only, accepted):
    require(
        successful_writes + read_only == accepted,
        "completion/publication counters do not balance",
    )
if None not in (accepted, published):
    require(accepted == published, "accepted/native publication counters differ")
if None not in (accepted, publication_sequence):
    require(accepted == publication_sequence, "publication sequence is not dense")

require(adapter.get("active_finalizers") == 0, "active finalizers remain at close")
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
    "active_finalizers": adapter.get("active_finalizers"),
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
    "$output" \
    "$trial" \
    "$mode" \
    "$attempt" \
    "$runtime_mode" \
    "$server_path" \
    "$server_sha" \
    "$expected_commit" \
    "$ACTIVE_WARMUP_TIME" \
    "$ACTIVE_RUN_TIME" \
    "$ACTIVE_WAREHOUSES" \
    "$ACTIVE_THREADS" \
    "$ACTIVE_PREPARE_THREADS" \
    "$ACTIVE_OUTPUT_INTERVAL" \
    "$ACTIVE_STABILIZE_SECONDS" \
    "$TIDB_CPUSET" \
    "$CLIENT_CPUSET" \
    "$DSN_PARAMS" \
    "$ACTIVE_SUITE_ROOT/experiment.json" \
    "$(snapshot_sha)" \
    "$NATIVE_LIBRARY_SHA" \
    "$IMMUTABLE_GO_TPC_SHA" <<'PY'
import json
import pathlib
import sys

(
    output,
    trial,
    mode,
    attempt,
    runtime_mode,
    server_path,
    server_sha,
    expected_commit,
    warmup,
    run_time,
    warehouses,
    threads,
    prepare_threads,
    interval,
    stabilize,
    tidb_cpus,
    client_cpus,
    dsn,
    experiment,
    snapshot_sha,
    native_sha,
    go_tpc_sha,
) = sys.argv[1:]
result = {
    "format_version": 1,
    "trial": trial,
    "mode": mode,
    "attempt": attempt,
    "workload_label": "TPC-C NewOrder-only",
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
        "weights": [100, 0, 0, 0, 0],
    },
    "transaction_configuration": {
        "mode": "optimistic",
        "async_commit": False,
        "one_phase_commit": False,
        "low_resolution_tso": False,
        "dsn_params": dsn,
    },
    "cpu_affinity": {
        "tidb": tidb_cpus,
        "client": client_cpus,
    },
    "storage_configuration": {
        "store": "unistore",
        "source_snapshot_tree_sha256": snapshot_sha,
    },
    "checker_configuration": {
        "reorder_capacity_bytes": 0,
        "reorder_environment": "unset",
        "frontier_interval_environment": "unset",
        "frontier_pump": False,
    },
    "expected_tidb_commit": expected_commit,
    "binaries": {
        "tidb_server": {
            "path": str(pathlib.Path(server_path).resolve()),
            "sha256": server_sha,
        },
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
  attempt_dir=$4
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
  RESULT_WATCHER_PID=
  CURRENT_PHASE=initialization
  FAILURE_REASON=
  GO_TPC_PASS=false
  ATTEMPT_SUCCESS=false
  MAIN_SERVER_EXIT_STATUS=
  POST_CHECK_SERVER_EXIT_STATUS=
  WORKLOAD_START_NS=
  WORKLOAD_CLIENT_EXIT_NS=
  CHECKER_RESULT_AVAILABLE_NS=
  SERVER_SHUTDOWN_START_NS=
  TIDB_SERVER_EXIT_NS=

  write_runner_status() {
    local status=$1
    python3 - \
      "$attempt_dir/runner-status.json" \
      "$status" \
      "$GO_TPC_PASS" \
      "$MAIN_SERVER_EXIT_STATUS" \
      "$POST_CHECK_SERVER_EXIT_STATUS" \
      "$FAILURE_REASON" \
      "$CURRENT_PHASE" <<'PY'
import json
import pathlib
import sys

path, status, go_tpc, server_status, post_status, reason, phase = sys.argv[1:]
def optional_int(value):
    return None if value == "" else int(value)
pathlib.Path(path).write_text(json.dumps({
    "format_version": 1,
    "status": status,
    "go_tpc_pass": go_tpc == "true",
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
import json
import pathlib
import sys
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
      "$WORKLOAD_START_NS" \
      "$WORKLOAD_CLIENT_EXIT_NS" \
      "$CHECKER_RESULT_AVAILABLE_NS" \
      "$TIDB_SERVER_EXIT_NS" \
      "$SERVER_SHUTDOWN_START_NS" \
      "$native_close" <<'PY'
import json
import pathlib
import sys

path, workload_start, client_exit, result_available, server_exit, shutdown_start, native_close = sys.argv[1:]
def optional_int(value):
    return None if value == "" else int(value)
record = {
    "clock": "CLOCK_MONOTONIC",
    "workload_start_monotonic_ns": optional_int(workload_start),
    "workload_client_exit_monotonic_ns": optional_int(client_exit),
    "checker_result_available_monotonic_ns": optional_int(result_available),
    "tidb_server_exit_monotonic_ns": optional_int(server_exit),
    "server_shutdown_start_monotonic_ns": optional_int(shutdown_start),
    "workload_start_ns": optional_int(workload_start),
    "workload_client_exit_ns": optional_int(client_exit),
    "checker_result_available_ns": optional_int(result_available),
    "tidb_server_exit_ns": optional_int(server_exit),
    "server_shutdown_start_ns": optional_int(shutdown_start),
    "native_close_nanoseconds": optional_int(native_close),
}
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
    if [[ "$SERVER_PHASE" = main && -z "$MAIN_SERVER_EXIT_STATUS" ]]; then
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
    trap - EXIT ERR INT TERM
    stop_pidstat
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
samples = 0
started = time.monotonic_ns()
with samples_path.open("w") as sample_file:
    sample_file.write("monotonic_ns vmhwm_kib process_cpu_ticks\n")
    while not stop:
        try:
            status_text = pathlib.Path(f"/proc/{pid}/status").read_text()
            stat_fields = pathlib.Path(f"/proc/{pid}/stat").read_text().split()
        except OSError:
            break
        hwm = 0
        for line in status_text.splitlines():
            if line.startswith("VmHWM:"):
                hwm = int(line.split()[1])
                break
        cpu = int(stat_fields[13]) + int(stat_fields[14])
        first_cpu = cpu if first_cpu is None else first_cpu
        last_cpu = cpu
        maximum_hwm = max(maximum_hwm, hwm)
        now = time.monotonic_ns()
        sample_file.write(f"{now} {hwm} {cpu}\n")
        sample_file.flush()
        samples += 1
        time.sleep(0.1)
ended = time.monotonic_ns()
record = {
    "clock": "CLOCK_MONOTONIC",
    "started_ns": started,
    "ended_ns": ended,
    "samples": samples,
    "maximum_rss_kib": maximum_hwm,
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

  stop_main_server() {
    if [[ -n "$runtime_mode" ]]; then
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
    if [[ -n "$runtime_mode" ]]; then
      for _ in $(seq 1 200); do
        [[ -s "$attempt_dir/checker-result-available-ns.txt" ]] && break
        sleep 0.01
      done
      if [[ -s "$attempt_dir/checker-result-available-ns.txt" ]]; then
        CHECKER_RESULT_AVAILABLE_NS=$(
          tr -d '[:space:]' <"$attempt_dir/checker-result-available-ns.txt"
        )
      else
        # A final parse at this point distinguishes a missed observation from a
        # genuinely absent verdict while keeping the timestamp monotonic.
        if python3 - "$attempt_dir/checker-result.json" <<'PY'
import json
import pathlib
import sys
value = json.loads(pathlib.Path(sys.argv[1]).read_text())
if value.get("format_version") != 1 or value.get("outcome") == "in-progress":
    raise SystemExit(1)
PY
        then
          CHECKER_RESULT_AVAILABLE_NS=$(monotonic_ns)
        fi
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

  CURRENT_PHASE=clone-snapshot
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  cp -a --reflink=auto "$SNAPSHOT_STORE" "$attempt_dir/store"
  local cloned_hash
  read -r cloned_hash _ < <(tree_fingerprint "$attempt_dir/store")
  [[ "$cloned_hash" = "$(snapshot_sha)" ]] ||
    die "cloned store does not match the stopped empty-schema snapshot"
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

  CURRENT_PHASE=verify-empty-start
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  if [[ -z "$runtime_mode" ]]; then
    taskset -c "$CLIENT_CPUSET" "$TIDB_SQL" \
      -action assert-empty \
      -address "$SQL_HOST:$SQL_PORT" \
      -database "$DATABASE" \
      -params "$DSN_PARAMS" \
      >"$attempt_dir/initial-empty-counts.json" \
      2>"$attempt_dir/initial-empty-counts-error.txt"
  else
    # COUNT(*) uses a coprocessor reader that cannot expose exact read evidence
    # in checked mode. The stopped clone was fingerprinted before startup.
    taskset -c "$CLIENT_CPUSET" "$TIDB_SQL" \
      -action ping \
      -address "$SQL_HOST:$SQL_PORT" \
      -database "$DATABASE" \
      -params "$DSN_PARAMS" \
      >"$attempt_dir/initial-empty-counts-error.txt" 2>&1
    cp "$SNAPSHOT_INDEX" "$attempt_dir/initial-empty-counts.json"
  fi
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  local command_status
  CURRENT_PHASE=prepare
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  set +e
  /usr/bin/time -v -o "$attempt_dir/prepare-time.txt" \
    taskset -c "$CLIENT_CPUSET" "$IMMUTABLE_GO_TPC" tpcc prepare \
      -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
      -T "$ACTIVE_PREPARE_THREADS" --warehouses "$ACTIVE_WAREHOUSES" \
      --no-check --conn-params "$DSN_PARAMS" --output json \
      >"$attempt_dir/prepare.txt" 2>&1
  command_status=$?
  set -e
  validate_prepare_output \
    "$command_status" \
    "$attempt_dir/prepare.txt" \
    "$attempt_dir/prepare-validation.json"
  printf '{"exit_status":%d}\n' "$command_status" \
    >"$attempt_dir/prepare-status.json"
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=capability-gate
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  taskset -c "$CLIENT_CPUSET" "$TIDB_SQL" \
    -action new-order-gate \
    -address "$SQL_HOST:$SQL_PORT" \
    -database "$DATABASE" \
    -params "$DSN_PARAMS" \
    >"$attempt_dir/capability-gate.json" \
    2>"$attempt_dir/capability-gate-error.txt"
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=stabilize
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  sleep "$ACTIVE_STABILIZE_SECONDS"
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=warmup
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  set +e
  /usr/bin/time -v -o "$attempt_dir/warmup-time.txt" \
    taskset -c "$CLIENT_CPUSET" "$IMMUTABLE_GO_TPC" tpcc run \
      -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
      -T "$ACTIVE_THREADS" --warehouses "$ACTIVE_WAREHOUSES" \
      --ignore-error --silence \
      --weight 100,0,0,0,0 \
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
    "$attempt_dir/warmup-validation.json"
  printf '{"exit_status":%d}\n' "$command_status" \
    >"$attempt_dir/warmup-status.json"
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" complete 0

  CURRENT_PHASE=workload
  record_phase "$attempt_dir/phases.jsonl" "$CURRENT_PHASE" start ""
  record_process_snapshot "$attempt_dir/tidb-proc-before-workload.txt"
  start_resource_monitor
  pidstat -h -u -r -d -p "$TIDB_PID" "$MONITOR_INTERVAL_SECONDS" \
    >"$attempt_dir/tidb-pidstat.txt" 2>&1 &
  PIDSTAT_PID=$!
  WORKLOAD_START_NS=$(monotonic_ns)
  set +e
  /usr/bin/time -v -o "$attempt_dir/workload-time.txt" \
    taskset -c "$CLIENT_CPUSET" "$IMMUTABLE_GO_TPC" tpcc run \
      -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
      -T "$ACTIVE_THREADS" --warehouses "$ACTIVE_WAREHOUSES" \
      --ignore-error --silence \
      --weight 100,0,0,0,0 \
      --time "$ACTIVE_RUN_TIME" \
      --interval "$ACTIVE_OUTPUT_INTERVAL" \
      --conn-params "$DSN_PARAMS" --output json \
      >"$attempt_dir/workload.txt" 2>&1
  command_status=$?
  set -e
  WORKLOAD_CLIENT_EXIT_NS=$(monotonic_ns)
  printf '%s\n' "$command_status" >"$attempt_dir/workload-exit-status.txt"
  printf '{"exit_status":%d}\n' "$command_status" \
    >"$attempt_dir/workload-status.json"
  stop_pidstat
  record_process_snapshot "$attempt_dir/tidb-proc-after-workload.txt"
  cp "$attempt_dir/tidb-proc-after-workload.txt" \
    "$attempt_dir/tidb-proc-after.txt"
  validate_go_tpc_run \
    "$ACTIVE_RUN_TIME" \
    "$ACTIVE_OUTPUT_INTERVAL" \
    "$command_status" \
    "$attempt_dir/workload.txt" \
    "$attempt_dir/workload-validation.json" \
    "$WORKLOAD_START_NS" \
    "$WORKLOAD_CLIENT_EXIT_NS"
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
    taskset -c "$CLIENT_CPUSET" "$IMMUTABLE_GO_TPC" tpcc check \
      -H "$SQL_HOST" -P "$SQL_PORT" -D "$DATABASE" \
      -T 1 --warehouses "$ACTIVE_WAREHOUSES" --check-all \
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
  done

  set +e
  python3 "$IMMUTABLE_ANALYZER" "$suite_root"
  ANALYZER_STATUS=$?
  set -e
  if (( ANALYZER_STATUS != 0 )); then
    echo "WARNING: analyzer exited with status $ANALYZER_STATUS" >&2
  fi
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
expected_modes = {
    "baseline-original",
    "serialized-capture-only",
    "serialized-order-only",
    "serialized-full",
}
summary_path = root / "tpcc-summary.json"
if summary_path.is_file():
    try:
        summary = json.loads(summary_path.read_text())
    except json.JSONDecodeError:
        summary = {}
    if summary.get("all_valid") is False:
        raise SystemExit(1)

csv_path = root / "tpcc-trials.csv"
if not csv_path.is_file():
    raise SystemExit("tpcc-trials.csv was not generated")
rows = list(csv.DictReader(csv_path.open(newline="")))
selected = {}
for row in rows:
    trial = row.get("trial", "")
    mode = row.get("mode", "")
    valid = row.get("valid", "").strip().lower() in {"1", "true", "yes", "valid"}
    if trial and mode:
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
create_empty_schema_snapshot

if [[ "$SMOKE" = true ]]; then
  SMOKE_WARMUP_TIME=${SMOKE_WARMUP_TIME:-2s}
  SMOKE_RUN_TIME=${SMOKE_RUN_TIME:-5s}
  SMOKE_OUTPUT_INTERVAL=${SMOKE_OUTPUT_INTERVAL:-1s}
  SMOKE_STABILIZE_SECONDS=${SMOKE_STABILIZE_SECONDS:-1}
  SMOKE_THREADS=${SMOKE_THREADS:-2}
  SMOKE_PREPARE_THREADS=${SMOKE_PREPARE_THREADS:-1}
  write_or_verify_experiment \
    "$RESULT_ROOT" \
    1 \
    "$SMOKE_WARMUP_TIME" \
    "$SMOKE_RUN_TIME" \
    1 \
    "$SMOKE_THREADS" \
    "$SMOKE_PREPARE_THREADS" \
    "$SMOKE_OUTPUT_INTERVAL" \
    "$SMOKE_STABILIZE_SECONDS" \
    true
  run_suite \
    "$RESULT_ROOT" \
    1 \
    "$SMOKE_WARMUP_TIME" \
    "$SMOKE_RUN_TIME" \
    1 \
    "$SMOKE_THREADS" \
    "$SMOKE_PREPARE_THREADS" \
    "$SMOKE_OUTPUT_INTERVAL" \
    "$SMOKE_STABILIZE_SECONDS"
  if suite_is_valid "$RESULT_ROOT" 1; then
    write_valid_marker "$RESULT_ROOT" "$RESULT_ROOT/smoke-passed.json"
    echo "Four-mode smoke suite passed: $RESULT_ROOT"
    exit 0
  fi
  die "four-mode smoke suite failed; inspect $RESULT_ROOT/tpcc-trials.csv"
fi

write_or_verify_experiment \
  "$RESULT_ROOT" \
  "$TRIALS" \
  "$WARMUP_TIME" \
  "$RUN_TIME" \
  "$WAREHOUSES" \
  "$THREADS" \
  "$PREPARE_THREADS" \
  "$OUTPUT_INTERVAL" \
  "$STABILIZE_SECONDS" \
  false

SMOKE_ROOT="$RESULT_ROOT/smoke"
SMOKE_WARMUP_TIME=${SMOKE_WARMUP_TIME:-2s}
SMOKE_RUN_TIME=${SMOKE_RUN_TIME:-5s}
SMOKE_OUTPUT_INTERVAL=${SMOKE_OUTPUT_INTERVAL:-1s}
SMOKE_STABILIZE_SECONDS=${SMOKE_STABILIZE_SECONDS:-1}
SMOKE_THREADS=${SMOKE_THREADS:-2}
SMOKE_PREPARE_THREADS=${SMOKE_PREPARE_THREADS:-1}
write_or_verify_experiment \
  "$SMOKE_ROOT" \
  1 \
  "$SMOKE_WARMUP_TIME" \
  "$SMOKE_RUN_TIME" \
  1 \
  "$SMOKE_THREADS" \
  "$SMOKE_PREPARE_THREADS" \
  "$SMOKE_OUTPUT_INTERVAL" \
  "$SMOKE_STABILIZE_SECONDS" \
  true

if [[ "$RESUME" = true && -s "$SMOKE_ROOT/smoke-passed.json" ]] &&
  suite_is_valid "$SMOKE_ROOT" 1; then
  echo "Skipping exact, valid four-mode smoke suite: $SMOKE_ROOT"
else
  run_suite \
    "$SMOKE_ROOT" \
    1 \
    "$SMOKE_WARMUP_TIME" \
    "$SMOKE_RUN_TIME" \
    1 \
    "$SMOKE_THREADS" \
    "$SMOKE_PREPARE_THREADS" \
    "$SMOKE_OUTPUT_INTERVAL" \
    "$SMOKE_STABILIZE_SECONDS"
  if ! suite_is_valid "$SMOKE_ROOT" 1; then
    die "four-mode smoke suite failed; long trials were not started"
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
echo "Summary CSV: $RESULT_ROOT/tpcc-summary.csv"
echo "Summary JSON: $RESULT_ROOT/tpcc-summary.json"
echo "Report:      $RESULT_ROOT/tpcc-report.md"

if suite_is_valid "$RESULT_ROOT" "$TRIALS"; then
  exit 0
fi
die "one or more trial/mode runs are invalid; all evidence was retained"
