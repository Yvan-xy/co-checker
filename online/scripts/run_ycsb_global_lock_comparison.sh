#!/usr/bin/env bash

set -Eeuo pipefail

readonly PRISTINE_TIDB_COMMIT="b94006d3abffd428634ecc5c499f4bcb7b4d06d1"
readonly GO_YCSB_COMMIT="f030f9942393a8febbf4365c2d582711723159f5"
readonly PROFILE_ID="co-checker-ycsb-core-v1"
readonly -a HEADLINE_MODES=(
  baseline-original
  serialized-capture-only
  serialized-order-only
  serialized-full
)
readonly LOAD_SEED=7732542672685676034
readonly WARMUP_SEED=6300581044758642952
readonly MEASURED_SEED=1251957853004150076

usage() {
  cat <<'EOF'
Usage: run_ycsb_global_lock_comparison.sh [options]

Required paths:
  --result-root PATH       Absolute append-only result root.
  --tidb PATH              Instrumented TiDB source worktree.
  --go-ycsb-source PATH    Clean go-ycsb v1.0.3 source at the pinned commit.
  --tidb-cpus LIST         taskset CPU list for TiDB.
  --client-cpus LIST       taskset CPU list for go-ycsb and control helpers.

Optional paths:
  --checker PATH           co-checker source worktree (default: script root).
  --analyzer PATH          YCSB analyzer.
  --baseline-worktree PATH Detached pristine TiDB worktree under /tmp.

Experiment:
  --trials N               Paired trials (default: 5; measured minimum: 5).
  --workloads A,B,...      Ordered workload set (default: A,B,C,D,E,F).
  --modes MODE,...         Mode subset (diagnostic-only; default: all four).
  --calibration PATH       Frozen versioned measured calibration JSON.
  --smoke                  One functional trial with smoke-only counts.
  --diagnostic-only        Permit a nonpublishable measured subset/count.
  --resume                 Verify the immutable manifest and append attempts.

Smoke-only overrides:
  --record-count N         Initial rows (default: 10000).
  --threads N              Measured/warm-up clients (default: 4).
  --warmup-operations N    Fixed warm-up operations (default: 2000).
  --operations N           Fixed measured operations (default: 10000).

Operational:
  --sql-host HOST          SQL/status host (default: 127.0.0.1).
  --sql-port PORT          SQL port (default: 4400).
  --status-port PORT       status port (default: 14080).
  --stabilize-seconds N    Pause before warm-up (default: 2 smoke, 30 measured).
  --digest-page-size N     Bounded digest page size (default: 1000).
  --digest-timeout DURATION Whole canonical-digest deadline (default: 10m).
  --server-log-level LEVEL TiDB log level (default: error).
  --retain-details         Keep stores/raw attempt evidence after success.
  --build-jobs N           Native build parallelism (default: 2).
  --checker-drain-timeout DURATION (default: 30m).
  --validate-only          Validate arguments/calibration without creating files.
  -h, --help               Show this help.

Normal measured runs require the exact A-F/four-mode matrix, at least five
trials, and a calibration JSON. They automatically gate on a fresh A-F
four-mode smoke under RESULT_ROOT/smoke before measured attempts begin.
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

require_value() {
  (( $# >= 2 )) || die "$1 requires a value"
}

SCRIPT_PATH=$(realpath "$0")
CHECKER_ROOT=$(realpath "$(dirname "$SCRIPT_PATH")/../..")
TIDB_ROOT=
GO_YCSB_SOURCE=
RESULT_ROOT=
ANALYZER=
BASELINE_WORKTREE=
CALIBRATION=
TIDB_CPUSET=
CLIENT_CPUSET=
SQL_HOST=127.0.0.1
SQL_PORT=4400
STATUS_PORT=14080
DATABASE=ycsb
TRIALS=5
WORKLOAD_TEXT=A,B,C,D,E,F
MODE_TEXT=baseline-original,serialized-capture-only,serialized-order-only,serialized-full
SMOKE=false
DIAGNOSTIC_ONLY=false
RESUME=false
VALIDATE_ONLY=false
CLI_TRIALS=false
CLI_RECORD_COUNT=false
CLI_THREADS=false
CLI_WARMUP_OPERATIONS=false
CLI_OPERATIONS=false
RECORD_COUNT=10000
FIELD_COUNT=10
FIELD_LENGTH=100
THREADS=4
WARMUP_OPERATIONS=2000
OPERATIONS=10000
STABILIZE_SECONDS=
DIGEST_PAGE_SIZE=1000
DIGEST_TIMEOUT=10m
SERVER_LOG_LEVEL=error
FINAL_RESULTS_ONLY=true
BUILD_JOBS=2
CHECKER_DRAIN_TIMEOUT=30m
readonly TIDB_RETRY_LIMIT=100

QUEUE_CAPACITY_BYTES=${QUEUE_CAPACITY_BYTES:-2147483648}
CAPTURE_CAPACITY_BYTES=${CAPTURE_CAPACITY_BYTES:-34359738368}
RETAINED_STATE_CAPACITY_BYTES=${RETAINED_STATE_CAPACITY_BYTES:-34359738368}
QUEUE_TIMEOUT=${QUEUE_TIMEOUT:-5m}
CAPTURE_TIMEOUT=${CAPTURE_TIMEOUT:-5m}
CHUNK_SEGMENT_BYTES=${CHUNK_SEGMENT_BYTES:-4096}
MAX_CACHED_CHUNK_SEGMENTS=${MAX_CACHED_CHUNK_SEGMENTS:-256}

while (( $# > 0 )); do
  case "$1" in
    --checker) require_value "$@"; CHECKER_ROOT=$2; shift 2 ;;
    --tidb) require_value "$@"; TIDB_ROOT=$2; shift 2 ;;
    --go-ycsb-source) require_value "$@"; GO_YCSB_SOURCE=$2; shift 2 ;;
    --result-root) require_value "$@"; RESULT_ROOT=$2; shift 2 ;;
    --analyzer) require_value "$@"; ANALYZER=$2; shift 2 ;;
    --baseline-worktree) require_value "$@"; BASELINE_WORKTREE=$2; shift 2 ;;
    --calibration) require_value "$@"; CALIBRATION=$2; shift 2 ;;
    --tidb-cpus) require_value "$@"; TIDB_CPUSET=$2; shift 2 ;;
    --client-cpus) require_value "$@"; CLIENT_CPUSET=$2; shift 2 ;;
    --sql-host) require_value "$@"; SQL_HOST=$2; shift 2 ;;
    --sql-port) require_value "$@"; SQL_PORT=$2; shift 2 ;;
    --status-port) require_value "$@"; STATUS_PORT=$2; shift 2 ;;
    --trials) require_value "$@"; TRIALS=$2; CLI_TRIALS=true; shift 2 ;;
    --workloads) require_value "$@"; WORKLOAD_TEXT=$2; shift 2 ;;
    --modes) require_value "$@"; MODE_TEXT=$2; shift 2 ;;
    --record-count) require_value "$@"; RECORD_COUNT=$2; CLI_RECORD_COUNT=true; shift 2 ;;
    --threads) require_value "$@"; THREADS=$2; CLI_THREADS=true; shift 2 ;;
    --warmup-operations) require_value "$@"; WARMUP_OPERATIONS=$2; CLI_WARMUP_OPERATIONS=true; shift 2 ;;
    --operations) require_value "$@"; OPERATIONS=$2; CLI_OPERATIONS=true; shift 2 ;;
    --stabilize-seconds) require_value "$@"; STABILIZE_SECONDS=$2; shift 2 ;;
    --digest-page-size) require_value "$@"; DIGEST_PAGE_SIZE=$2; shift 2 ;;
    --digest-timeout) require_value "$@"; DIGEST_TIMEOUT=$2; shift 2 ;;
    --server-log-level) require_value "$@"; SERVER_LOG_LEVEL=$2; shift 2 ;;
    --retain-details) FINAL_RESULTS_ONLY=false; shift ;;
    --build-jobs) require_value "$@"; BUILD_JOBS=$2; shift 2 ;;
    --checker-drain-timeout) require_value "$@"; CHECKER_DRAIN_TIMEOUT=$2; shift 2 ;;
    --smoke) SMOKE=true; shift ;;
    --diagnostic-only) DIAGNOSTIC_ONLY=true; shift ;;
    --resume) RESUME=true; shift ;;
    --validate-only) VALIDATE_ONLY=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[[ -n "$RESULT_ROOT" ]] || die "--result-root is required"
[[ "$RESULT_ROOT" = /* ]] || die "--result-root must be absolute"
[[ -n "$TIDB_ROOT" ]] || die "--tidb is required"
[[ "$TIDB_ROOT" = /* ]] || die "--tidb must be absolute"
[[ -n "$GO_YCSB_SOURCE" ]] || die "--go-ycsb-source is required"
[[ "$GO_YCSB_SOURCE" = /* ]] || die "--go-ycsb-source must be absolute"
[[ -n "$TIDB_CPUSET" ]] || die "--tidb-cpus is required"
[[ -n "$CLIENT_CPUSET" ]] || die "--client-cpus is required"
[[ "$CHECKER_ROOT" = /* ]] || die "--checker must be absolute"

RESULT_ROOT=$(realpath -m "$RESULT_ROOT")
CHECKER_ROOT=$(realpath "$CHECKER_ROOT")
TIDB_ROOT=$(realpath "$TIDB_ROOT")
GO_YCSB_SOURCE=$(realpath "$GO_YCSB_SOURCE")
ANALYZER=${ANALYZER:-$CHECKER_ROOT/online/tools/analyze_ycsb_global_lock.py}
ANALYZER=$(realpath "$ANALYZER")
BASELINE_WORKTREE=${BASELINE_WORKTREE:-/tmp/tidb-ycsb-pristine-${PRISTINE_TIDB_COMMIT:0:12}-u$(id -u)}
BASELINE_WORKTREE=$(realpath -m "$BASELINE_WORKTREE")
[[ "$BASELINE_WORKTREE" = /tmp/* && "$BASELINE_WORKTREE" != /tmp ]] ||
  die "--baseline-worktree must be a specific directory under /tmp"
if [[ -n "$CALIBRATION" ]]; then
  [[ "$CALIBRATION" = /* ]] || die "--calibration must be absolute"
  CALIBRATION=$(realpath "$CALIBRATION")
fi
validate_positive() {
  [[ "$2" =~ ^[1-9][0-9]*$ ]] || die "$1 must be a positive integer"
}

validate_nonnegative() {
  [[ "$2" =~ ^[0-9]+$ ]] || die "$1 must be a nonnegative integer"
}

validate_duration() {
  python3 - "$1" "$2" <<'PY'
import re, sys
name, value = sys.argv[1:]
parts = re.findall(r"([0-9]+(?:\.[0-9]+)?)(ms|h|m|s)", value)
if not parts or "".join(a + b for a, b in parts) != value:
    raise SystemExit(f"{name} must be a positive Go-style duration")
units = {"ms": .001, "s": 1., "m": 60., "h": 3600.}
if sum(float(a) * units[b] for a, b in parts) <= 0:
    raise SystemExit(f"{name} must be positive")
PY
}

for pair in \
  "trials:$TRIALS" "record count:$RECORD_COUNT" "threads:$THREADS" \
  "warmup operations:$WARMUP_OPERATIONS" "operations:$OPERATIONS" \
  "digest page size:$DIGEST_PAGE_SIZE" "build jobs:$BUILD_JOBS" \
  "queue capacity:$QUEUE_CAPACITY_BYTES" "capture capacity:$CAPTURE_CAPACITY_BYTES" \
  "retained capacity:$RETAINED_STATE_CAPACITY_BYTES" \
  "chunk segment bytes:$CHUNK_SEGMENT_BYTES" \
  "maximum cached chunk segments:$MAX_CACHED_CHUNK_SEGMENTS"
do
  validate_positive "${pair%%:*}" "${pair#*:}"
done
[[ "$SQL_PORT" =~ ^[1-9][0-9]*$ && "$STATUS_PORT" =~ ^[1-9][0-9]*$ ]] || die "ports must be positive integers"
(( SQL_PORT <= 65535 && STATUS_PORT <= 65535 && SQL_PORT != STATUS_PORT )) || die "ports must be distinct and at most 65535"
validate_duration "checker drain timeout" "$CHECKER_DRAIN_TIMEOUT"
validate_duration "digest timeout" "$DIGEST_TIMEOUT"
validate_duration "queue timeout" "$QUEUE_TIMEOUT"
validate_duration "capture timeout" "$CAPTURE_TIMEOUT"
case "$SERVER_LOG_LEVEL" in debug|info|warn|error|fatal) ;; *) die "unsupported server log level: $SERVER_LOG_LEVEL" ;; esac
if [[ -n "$STABILIZE_SECONDS" ]]; then validate_nonnegative "stabilize seconds" "$STABILIZE_SECONDS"; fi

IFS=',' read -r -a WORKLOADS <<<"$WORKLOAD_TEXT"
(( ${#WORKLOADS[@]} > 0 )) || die "--workloads cannot be empty"
declare -A SEEN_WORKLOAD=()
for workload in "${WORKLOADS[@]}"; do
  [[ "$workload" =~ ^[A-F]$ ]] || die "workloads must be comma-separated A-F"
  [[ -z "${SEEN_WORKLOAD[$workload]:-}" ]] || die "duplicate workload: $workload"
  SEEN_WORKLOAD[$workload]=1
done

IFS=',' read -r -a MODES <<<"$MODE_TEXT"
(( ${#MODES[@]} > 0 )) || die "--modes cannot be empty"
declare -A SEEN_MODE=()
for mode in "${MODES[@]}"; do
  case "$mode" in
    baseline-original|serialized-capture-only|serialized-order-only|serialized-full) ;;
    *) die "unsupported mode: $mode" ;;
  esac
  [[ -z "${SEEN_MODE[$mode]:-}" ]] || die "duplicate mode: $mode"
  SEEN_MODE[$mode]=1
done
if [[ "$DIAGNOSTIC_ONLY" = true ]]; then
  [[ -n "${SEEN_MODE[baseline-original]:-}" && ${#MODES[@]} -ge 2 ]] ||
    die "diagnostic mode subsets require baseline-original and at least one checked mode"
elif [[ "$MODE_TEXT" != baseline-original,serialized-capture-only,serialized-order-only,serialized-full ]]; then
  die "mode subsets require --diagnostic-only"
fi

if [[ "$SMOKE" = true ]]; then
  if [[ "$CLI_TRIALS" = true ]]; then (( TRIALS == 1 )) || die "--smoke requires --trials 1"; else TRIALS=1; fi
  [[ -z "$CALIBRATION" ]] || die "--smoke does not accept --calibration"
  STABILIZE_SECONDS=${STABILIZE_SECONDS:-2}
else
  if [[ "$CLI_RECORD_COUNT" = true || "$CLI_THREADS" = true || "$CLI_WARMUP_OPERATIONS" = true || "$CLI_OPERATIONS" = true ]]; then
    die "record/thread/operation overrides are smoke-only; measured parameters come from calibration"
  fi
  STABILIZE_SECONDS=${STABILIZE_SECONDS:-30}
  [[ -n "$CALIBRATION" ]] || die "measured runs require --calibration"
  [[ -f "$CALIBRATION" ]] || die "calibration file not found: $CALIBRATION"
  if [[ "$DIAGNOSTIC_ONLY" != true ]]; then
    (( TRIALS >= 5 )) || die "publishable measured runs require at least five trials"
    [[ "$WORKLOAD_TEXT" = A,B,C,D,E,F ]] || die "publishable measured runs require workloads A,B,C,D,E,F"
  fi
fi

for command in bash cmake cp curl df git go lscpu make ninja patch pidstat python3 realpath rg sha256sum ss tar taskset uname /usr/bin/time; do
  command -v "$command" >/dev/null || die "required command not found: $command"
done
[[ -f "$CHECKER_ROOT/online/CMakeLists.txt" ]] || die "checker source is missing online/CMakeLists.txt"
[[ -f "$ANALYZER" ]] || die "analyzer not found: $ANALYZER"
[[ -f "$CHECKER_ROOT/online/tools/tidb_sql.go" ]] || die "SQL helper source is missing"
[[ -f "$CHECKER_ROOT/online/tools/tidb_sql_test.go" ]] || die "SQL helper tests are missing"
GO_YCSB_PATCH="$CHECKER_ROOT/online/tools/go_ycsb_v1.0.3_online_checker.patch"
[[ -f "$GO_YCSB_PATCH" ]] || die "go-ycsb patch is missing"
[[ -f "$TIDB_ROOT/AGENTS.md" && -f "$TIDB_ROOT/Makefile" ]] || die "instrumented TiDB worktree is invalid"
[[ -d "$GO_YCSB_SOURCE/.git" || -f "$GO_YCSB_SOURCE/.git" ]] || die "go-ycsb source must be a Git worktree"
[[ "$(git -C "$GO_YCSB_SOURCE" rev-parse HEAD)" = "$GO_YCSB_COMMIT" ]] || die "go-ycsb source is not at pinned commit $GO_YCSB_COMMIT"
[[ -z "$(git -C "$GO_YCSB_SOURCE" status --short --untracked-files=all)" ]] || die "go-ycsb source must be clean"
git -C "$TIDB_ROOT" cat-file -e "$PRISTINE_TIDB_COMMIT^{commit}" || die "pristine TiDB commit is unavailable"
rg -q 'serialized-global-lock' "$TIDB_ROOT/pkg/store/driver/txn/captureadapter/config.go" || die "instrumented TiDB lacks serialized global-lock support"
taskset -c "$TIDB_CPUSET" true || die "TiDB CPU set is unavailable: $TIDB_CPUSET"
taskset -c "$CLIENT_CPUSET" true || die "client CPU set is unavailable: $CLIENT_CPUSET"

for source_root in "$CHECKER_ROOT" "$TIDB_ROOT" "$GO_YCSB_SOURCE"; do
  [[ "$RESULT_ROOT" != "$source_root" && "$RESULT_ROOT" != "$source_root/"* ]] || die "result root must be outside source worktrees"
done

if [[ "$VALIDATE_ONLY" = true ]]; then
  if [[ "$SMOKE" != true ]]; then
    python3 - "$CALIBRATION" "$PROFILE_ID" "$WORKLOAD_TEXT" "$TIDB_RETRY_LIMIT" <<'PY'
import json, pathlib, sys
path, profile, workload_text, retry_limit = sys.argv[1:]
value = json.loads(pathlib.Path(path).read_text())
if value.get("format_version") != 1 or value.get("profile_id") != profile:
    raise SystemExit("calibration format/profile mismatch")
if value.get("tidb_retry_limit") != int(retry_limit):
    raise SystemExit("calibration TiDB retry limit mismatch")
for name in ("record_count", "field_count", "field_length", "threads"):
    if not isinstance(value.get(name), int) or value[name] <= 0:
        raise SystemExit(f"invalid calibration {name}")
if value.get("seed_policy") != "fixed-v1":
    raise SystemExit("calibration seed policy mismatch")
expected_seeds = {"load": 7732542672685676034, "warmup": 6300581044758642952, "measured": 1251957853004150076}
if value.get("seeds") != expected_seeds:
    raise SystemExit("calibration fixed seeds mismatch")
for workload in workload_text.split(","):
    entry = value.get("workloads", {}).get(workload)
    if not isinstance(entry, dict):
        raise SystemExit(f"calibration workload {workload} is missing")
    for name in ("warmup_operations", "measured_operations"):
        if not isinstance(entry.get(name), int) or entry[name] <= 0:
            raise SystemExit(f"calibration workload {workload} has invalid {name}")
PY
  fi
  echo "configuration validation passed"
  exit 0
fi

declare -A WORKLOAD_WARMUP=()
declare -A WORKLOAD_OPERATIONS=()
CALIBRATION_SHA=
if [[ "$SMOKE" = true ]]; then
  for workload in "${WORKLOADS[@]}"; do
    WORKLOAD_WARMUP[$workload]=$WARMUP_OPERATIONS
    WORKLOAD_OPERATIONS[$workload]=$OPERATIONS
  done
else
  CALIBRATION_SHA=$(sha256sum "$CALIBRATION" | awk '{print $1}')
  mapfile -t CALIBRATION_VALUES < <(
    python3 - "$CALIBRATION" "$PROFILE_ID" "$WORKLOAD_TEXT" "$TIDB_RETRY_LIMIT" <<'PY'
import json, pathlib, sys
path, profile, workload_text, retry_limit = sys.argv[1:]
value = json.loads(pathlib.Path(path).read_text())
if value.get("format_version") != 1 or value.get("profile_id") != profile:
    raise SystemExit("calibration format/profile mismatch")
if value.get("tidb_retry_limit") != int(retry_limit):
    raise SystemExit("calibration TiDB retry limit mismatch")
for name in ("record_count", "field_count", "field_length", "threads"):
    if not isinstance(value.get(name), int) or value[name] <= 0:
        raise SystemExit(f"invalid calibration {name}")
if value.get("field_count") != 10 or value.get("field_length") != 100:
    raise SystemExit("supported profile requires field_count=10 and field_length=100")
if value.get("seed_policy") != "fixed-v1":
    raise SystemExit("calibration seed policy mismatch")
expected_seeds = {"load": 7732542672685676034, "warmup": 6300581044758642952, "measured": 1251957853004150076}
if value.get("seeds") != expected_seeds:
    raise SystemExit("calibration fixed seeds mismatch")
print(value["record_count"])
print(value["field_count"])
print(value["field_length"])
print(value["threads"])
for workload in workload_text.split(","):
    entry = value.get("workloads", {}).get(workload)
    if not isinstance(entry, dict):
        raise SystemExit(f"calibration workload {workload} is missing")
    for name in ("warmup_operations", "measured_operations"):
        if not isinstance(entry.get(name), int) or entry[name] <= 0:
            raise SystemExit(f"calibration workload {workload} has invalid {name}")
    print(f"{workload} {entry['warmup_operations']} {entry['measured_operations']}")
PY
  )
  RECORD_COUNT=${CALIBRATION_VALUES[0]}
  FIELD_COUNT=${CALIBRATION_VALUES[1]}
  FIELD_LENGTH=${CALIBRATION_VALUES[2]}
  THREADS=${CALIBRATION_VALUES[3]}
  for value in "${CALIBRATION_VALUES[@]:4}"; do
    read -r workload warmup operations <<<"$value"
    WORKLOAD_WARMUP[$workload]=$warmup
    WORKLOAD_OPERATIONS[$workload]=$operations
  done
fi

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

monotonic_ns() {
  python3 -c 'import time; print(time.monotonic_ns())'
}

next_numbered_directory() {
  local parent=$1 prefix=$2 maximum=0 path base number
  if [[ -d "$parent" ]]; then
    for path in "$parent"/"$prefix"-*; do
      [[ -d "$path" ]] || continue
      base=${path##*/}; number=${base#"$prefix"-}
      if [[ "$number" =~ ^[0-9]+$ ]] && (( 10#$number > maximum )); then
        maximum=$((10#$number))
      fi
    done
  fi
  printf '%s-%03d\n' "$prefix" "$((maximum + 1))"
}

capture_source_state() {
  local repository=$1 name=$2 output=$3 selection=$4
  local patch_path="$output/$name-tracked.patch"
  local untracked_path="$output/$name-untracked"
  local state_path="$output/$name-source-state.json"
  git -C "$repository" diff --binary --no-ext-diff HEAD >"$patch_path"
  python3 - "$repository" "$selection" "$patch_path" "$untracked_path" "$state_path" <<'PY'
import hashlib, json, os, pathlib, shutil, stat, subprocess, sys
repository, selection, patch_name, snapshot_name, state_name = sys.argv[1:]
root = pathlib.Path(repository).resolve()
patch = pathlib.Path(patch_name).resolve()
snapshot = pathlib.Path(snapshot_name).resolve()
state = pathlib.Path(state_name).resolve()

def sha(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def git(*args, binary=False):
    return subprocess.check_output(["git", "-C", str(root), *args], text=not binary)

raw = sorted(item for item in git("ls-files", "--others", "--exclude-standard", "-z", binary=True).split(b"\0") if item)
if selection == "online":
    raw = [item for item in raw if os.fsdecode(item).startswith("online/")]
elif selection != "all":
    raise SystemExit(f"unknown selection {selection!r}")
snapshot.mkdir(parents=True)
entries = []
for raw_path in raw:
    relative = os.fsdecode(raw_path)
    parts = pathlib.PurePosixPath(relative).parts
    if not parts or ".." in parts or pathlib.PurePosixPath(relative).is_absolute():
        raise SystemExit(f"unsafe untracked path {relative!r}")
    source = root.joinpath(*parts)
    destination = snapshot.joinpath(*parts)
    source_stat = source.lstat()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if stat.S_ISREG(source_stat.st_mode):
        shutil.copy2(source, destination, follow_symlinks=False)
        kind, size, digest = "file", source_stat.st_size, sha(source)
    elif stat.S_ISLNK(source_stat.st_mode):
        target = os.readlink(source)
        os.symlink(target, destination)
        content = os.fsencode(target)
        kind, size, digest = "symlink", len(content), hashlib.sha256(content).hexdigest()
    else:
        raise SystemExit(f"unsupported untracked source {relative!r}")
    entries.append({"path": relative, "type": kind, "mode": stat.S_IMODE(source_stat.st_mode), "size": size, "sha256": digest})
payload = {
    "format_version": 1,
    "commit": git("rev-parse", "HEAD").strip(),
    "diff_sha256": sha(patch),
    "untracked_selection": selection,
    "untracked_files": entries,
}
canonical = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
payload["source_state_sha256"] = hashlib.sha256(canonical).hexdigest()
payload["tracked_patch"] = {"path": str(patch), "sha256": payload["diff_sha256"]}
payload["untracked_snapshot_path"] = str(snapshot)
state.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
PY
}

materialize_source() {
  local repository=$1 state_path=$2 destination=$3
  local commit patch snapshot
  mapfile -t values < <(python3 - "$state_path" <<'PY'
import json, pathlib, sys
value = json.loads(pathlib.Path(sys.argv[1]).read_text())
print(value["commit"])
print(value["tracked_patch"]["path"])
print(value["untracked_snapshot_path"])
PY
  )
  commit=${values[0]}; patch=${values[1]}; snapshot=${values[2]}
  git -C "$repository" worktree add --detach "$destination" "$commit" >/dev/null
  if [[ "$(sha256_file "$patch")" != "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["diff_sha256"])' "$state_path")" ]]; then
    die "source patch changed while materializing $destination"
  fi
  if [[ -s "$patch" ]]; then git -C "$destination" apply --binary "$patch"; fi
  python3 - "$snapshot" "$destination" <<'PY'
import pathlib, shutil, sys
source, destination = map(pathlib.Path, sys.argv[1:])
if source.is_dir():
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        target = destination / relative
        if path.is_symlink():
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(path.readlink())
        elif path.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target, follow_symlinks=False)
PY
}

verify_source_state_unchanged() {
  local repository=$1 selection=$2 expected=$3 temporary
  temporary=$(mktemp -d -t ycsb-source-state.XXXXXX)
  capture_source_state "$repository" current "$temporary" "$selection"
  python3 - "$expected" "$temporary/current-source-state.json" <<'PY'
import json, pathlib, sys
expected, current = (json.loads(pathlib.Path(path).read_text()) for path in sys.argv[1:])
for name in ("commit", "diff_sha256", "source_state_sha256"):
    if expected.get(name) != current.get(name):
        raise SystemExit(f"source changed during artifact creation: {name}")
PY
  rm -rf -- "$temporary"
}

ensure_pristine_worktree() {
  if [[ -e "$BASELINE_WORKTREE" ]]; then
    git -C "$BASELINE_WORKTREE" rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "baseline path is not a Git worktree"
  else
    git -C "$TIDB_ROOT" worktree add --detach "$BASELINE_WORKTREE" "$PRISTINE_TIDB_COMMIT"
  fi
  [[ "$(git -C "$BASELINE_WORKTREE" rev-parse HEAD)" = "$PRISTINE_TIDB_COMMIT" ]] || die "baseline worktree commit mismatch"
  [[ -z "$(git -C "$BASELINE_WORKTREE" status --short --untracked-files=all)" ]] || die "baseline worktree is not pristine"
}

ARTIFACT_INDEX="$RESULT_ROOT/build-artifacts.json"

verify_existing_artifacts() {
  python3 - "$ARTIFACT_INDEX" "$SCRIPT_PATH" "$ANALYZER" "$GO_YCSB_PATCH" "$CHECKER_ROOT" "$TIDB_ROOT" <<'PY'
import hashlib, json, pathlib, subprocess, sys
index_name, runner, analyzer, patch, checker, tidb = sys.argv[1:]
index = json.loads(pathlib.Path(index_name).read_text())
def sha(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()
errors = []
for name, path in (("runner", runner), ("analyzer", analyzer), ("go_ycsb_patch", patch)):
    if sha(path) != index["inputs"][name]["sha256"]:
        errors.append(f"{name} changed")
for name, artifact in index["artifacts"].items():
    path = pathlib.Path(artifact["path"])
    if not path.is_file() or sha(path) != artifact["sha256"]:
        errors.append(f"artifact {name} changed or is missing")
for name, repo in (("checker", checker), ("instrumented_tidb", tidb)):
    if subprocess.check_output(["git", "-C", repo, "rev-parse", "HEAD"], text=True).strip() != index["inputs"][name]["commit"]:
        errors.append(f"{name} commit changed")
if errors:
    raise SystemExit("cannot resume with a different artifact manifest: " + "; ".join(errors))
PY
  local verification_directory
  verification_directory=$(mktemp -d -t ycsb-artifact-verify.XXXXXX)
  capture_source_state "$CHECKER_ROOT" checker "$verification_directory" online
  capture_source_state "$TIDB_ROOT" instrumented-tidb "$verification_directory" all
  python3 - "$ARTIFACT_INDEX" "$verification_directory/checker-source-state.json" "$verification_directory/instrumented-tidb-source-state.json" <<'PY'
import json, pathlib, sys
index = json.loads(pathlib.Path(sys.argv[1]).read_text())
for name, state_name in (("checker", sys.argv[2]), ("instrumented_tidb", sys.argv[3])):
    current = json.loads(pathlib.Path(state_name).read_text())
    expected = index["inputs"][name]
    for field in ("commit", "diff_sha256", "source_state_sha256"):
        if current.get(field) != expected.get(field):
            raise SystemExit(f"cannot resume: {name} source {field} changed")
PY
  rm -rf -- "$verification_directory"
}

build_artifacts() {
  if [[ -s "$ARTIFACT_INDEX" ]]; then
    [[ "$RESUME" = true ]] || die "artifact index exists; use --resume"
    verify_existing_artifacts
    return
  fi
  ensure_pristine_worktree
  mkdir -p "$RESULT_ROOT/artifacts"
  local name directory checker_commit tidb_commit
  name=$(next_numbered_directory "$RESULT_ROOT/artifacts" build-attempt)
  directory="$RESULT_ROOT/artifacts/$name"
  mkdir "$directory"
  capture_source_state "$CHECKER_ROOT" checker "$directory" online
  capture_source_state "$TIDB_ROOT" instrumented-tidb "$directory" all
  checker_commit=$(git -C "$CHECKER_ROOT" rev-parse HEAD)
  tidb_commit=$(git -C "$TIDB_ROOT" rev-parse HEAD)
  materialize_source "$CHECKER_ROOT" "$directory/checker-source-state.json" "$directory/checker-source"
  materialize_source "$TIDB_ROOT" "$directory/instrumented-tidb-source-state.json" "$directory/instrumented-tidb-source"

  {
    echo "checker_commit=$checker_commit"
    echo "instrumented_tidb_commit=$tidb_commit"
    echo "pristine_tidb_commit=$PRISTINE_TIDB_COMMIT"
    go version
    go env GOOS GOARCH GOVERSION CGO_ENABLED CC CXX
    cmake --version | head -n 1
    ninja --version
    uname -a
    lscpu
  } >"$directory/build-settings.txt"

  cmake -S "$directory/checker-source/online" -B "$directory/checker-release" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release >"$directory/checker-configure.txt" 2>&1
  cmake --build "$directory/checker-release" --target co_checker_online_c -j "$BUILD_JOBS" \
    >"$directory/checker-build.txt" 2>&1
  cp "$directory/checker-release/libco_checker_online_c.so" "$directory/libco_checker_online_c.so"

  make -C "$BASELINE_WORKTREE" server TARGET="$directory/tidb-server-pristine" \
    >"$directory/pristine-tidb-build.txt" 2>&1
  make -C "$directory/instrumented-tidb-source" server TARGET="$directory/tidb-server-instrumented" \
    >"$directory/instrumented-tidb-build.txt" 2>&1
  (
    cd "$directory/instrumented-tidb-source"
    go test "$directory/checker-source/online/tools/tidb_sql.go" "$directory/checker-source/online/tools/tidb_sql_test.go"
    go build -trimpath -o "$directory/tidb-sql" "$directory/checker-source/online/tools/tidb_sql.go"
  ) >"$directory/tidb-sql-build.txt" 2>&1

  mkdir "$directory/go-ycsb-source"
  git -C "$GO_YCSB_SOURCE" archive "$GO_YCSB_COMMIT" | tar -xf - -C "$directory/go-ycsb-source"
  patch -d "$directory/go-ycsb-source" -p1 --forward --batch <"$GO_YCSB_PATCH" \
    >"$directory/go-ycsb-patch.txt" 2>&1
  (
    cd "$directory/go-ycsb-source"
    go test ./pkg/client ./pkg/generator ./pkg/workload ./db/mysql ./cmd/go-ycsb
    go build -trimpath -o "$directory/go-ycsb-online" ./cmd/go-ycsb
  ) >"$directory/go-ycsb-build.txt" 2>&1
  go version -m "$directory/go-ycsb-online" >"$directory/go-ycsb-version.txt"

  mkdir "$directory/profiles"
  cp "$CHECKER_ROOT"/online/scripts/ycsb/workload?.properties "$directory/profiles/"
  cp "$GO_YCSB_PATCH" "$directory/go_ycsb_v1.0.3_online_checker.patch"
  cp "$SCRIPT_PATH" "$directory/run_ycsb_global_lock_comparison.sh"
  cp "$ANALYZER" "$directory/analyze_ycsb_global_lock.py"
  chmod 0555 "$directory/libco_checker_online_c.so" "$directory/tidb-server-pristine" \
    "$directory/tidb-server-instrumented" "$directory/tidb-sql" "$directory/go-ycsb-online" \
    "$directory/run_ycsb_global_lock_comparison.sh" "$directory/analyze_ycsb_global_lock.py"
  chmod 0444 "$directory/go_ycsb_v1.0.3_online_checker.patch" "$directory"/profiles/*

  verify_source_state_unchanged "$CHECKER_ROOT" online "$directory/checker-source-state.json"
  verify_source_state_unchanged "$TIDB_ROOT" all "$directory/instrumented-tidb-source-state.json"
  python3 - "$directory" "$checker_commit" "$tidb_commit" "$PRISTINE_TIDB_COMMIT" \
    "$SCRIPT_PATH" "$ANALYZER" "$GO_YCSB_PATCH" >"$directory/manifest.json" <<'PY'
import hashlib, json, pathlib, sys
directory, checker_commit, tidb_commit, pristine_commit, runner, analyzer, patch = sys.argv[1:]
root = pathlib.Path(directory).resolve()
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
    "go_ycsb": "go-ycsb-online",
    "runner": "run_ycsb_global_lock_comparison.sh",
    "analyzer": "analyze_ycsb_global_lock.py",
    "go_ycsb_patch": "go_ycsb_v1.0.3_online_checker.patch",
}
for path in sorted((root / "profiles").glob("*.properties")):
    artifact_names["profile_" + path.stem[-1].upper()] = str(path.relative_to(root))
manifest = {
    "format_version": 1,
    "build_type": "Release",
    "inputs": {
        "checker": json.loads((root / "checker-source-state.json").read_text()),
        "instrumented_tidb": json.loads((root / "instrumented-tidb-source-state.json").read_text()),
        "pristine_tidb": {"commit": pristine_commit},
        "runner": {"path": str(pathlib.Path(runner).resolve()), "sha256": sha(runner)},
        "analyzer": {"path": str(pathlib.Path(analyzer).resolve()), "sha256": sha(analyzer)},
        "go_ycsb_patch": {"path": str(pathlib.Path(patch).resolve()), "sha256": sha(patch), "base_commit": "f030f9942393a8febbf4365c2d582711723159f5"},
    },
    "artifacts": {name: {"path": str(root / filename), "sha256": sha(root / filename)} for name, filename in artifact_names.items()},
    "build_settings_path": str(root / "build-settings.txt"),
}
print(json.dumps(manifest, indent=2, sort_keys=True))
PY
  cp "$directory/manifest.json" "$ARTIFACT_INDEX"
  chmod 0444 "$directory/manifest.json" "$ARTIFACT_INDEX"
}

load_artifacts() {
  mapfile -t values < <(python3 - "$ARTIFACT_INDEX" <<'PY'
import json, pathlib, sys
value = json.loads(pathlib.Path(sys.argv[1]).read_text())
for name in ("native_library", "pristine_tidb_server", "instrumented_tidb_server", "tidb_sql", "go_ycsb", "analyzer"):
    print(value["artifacts"][name]["path"])
print(value["inputs"]["instrumented_tidb"]["commit"])
PY
  )
  NATIVE_LIBRARY=${values[0]}
  PRISTINE_TIDB_SERVER=${values[1]}
  INSTRUMENTED_TIDB_SERVER=${values[2]}
  TIDB_SQL=${values[3]}
  IMMUTABLE_GO_YCSB=${values[4]}
  IMMUTABLE_ANALYZER=${values[5]}
  INSTRUMENTED_TIDB_COMMIT=${values[6]}
  ARTIFACT_MANIFEST_SHA=$(sha256_file "$ARTIFACT_INDEX")
}

if [[ -e "$RESULT_ROOT" && ! -d "$RESULT_ROOT" ]]; then die "result root is not a directory"; fi
if [[ -d "$RESULT_ROOT" && "$RESUME" != true ]] && [[ -n "$(find "$RESULT_ROOT" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
  die "result root is not empty; use --resume for the exact experiment"
fi
mkdir -p "$RESULT_ROOT"
build_artifacts
load_artifacts

readonly DSN_PARAMS="tidb_enable_async_commit=0&tidb_enable_1pc=0&tidb_txn_mode=%27optimistic%27&tidb_low_resolution_tso=0&tidb_store_batch_size=0&tidb_allow_batch_cop=0&tidb_index_lookup_pushdown_policy=%27force%27&tidb_retry_limit=$TIDB_RETRY_LIMIT"

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

ensure_ports_free() {
  [[ -z "$(ss -H -ltn "sport = :$SQL_PORT")" ]] || die "SQL port $SQL_PORT is already listening"
  [[ -z "$(ss -H -ltn "sport = :$STATUS_PORT")" ]] || die "status port $STATUS_PORT is already listening"
}

wait_for_server() {
  local pid=$1 expected_commit=$2 output=$3 count
  for count in $(seq 1 180); do
    kill -0 "$pid" 2>/dev/null || return 1
    if curl --fail --silent "http://$SQL_HOST:$STATUS_PORT/status" >"$output"; then
      python3 - "$output" "$expected_commit" <<'PY'
import json, pathlib, sys
value = json.loads(pathlib.Path(sys.argv[1]).read_text())
if value.get("git_hash") != sys.argv[2]:
    raise SystemExit(f"unexpected TiDB commit {value.get('git_hash')!r}")
PY
      return 0
    fi
    sleep 1
  done
  return 1
}

start_server() {
  local server=$1 expected_commit=$2 directory=$3 prefix=$4
  ensure_ports_free
  mkdir -p "$directory/$prefix-tmp"
  taskset -c "$TIDB_CPUSET" "$server" \
    --store unistore \
    --config "$directory/tidb.toml" \
    --path "$directory/store" \
    --temp-dir "$directory/$prefix-tmp" \
    --host "$SQL_HOST" -P "$SQL_PORT" \
    --status-host "$SQL_HOST" --status "$STATUS_PORT" \
    --log-file "$directory/$prefix-server.txt" \
    >"$directory/$prefix-server-stdout.txt" 2>&1 &
  TIDB_PID=$!
  wait_for_server "$TIDB_PID" "$expected_commit" "$directory/$prefix-status.json" ||
    die "$prefix server did not become ready"
}

stop_server() {
  local output=$1
  kill -TERM "$TIDB_PID" 2>/dev/null || true
  set +e
  wait "$TIDB_PID"
  local status=$?
  set -e
  TIDB_PID=
  printf '{"exit_status":%d,"passed":%s}\n' "$status" "$([[ $status = 0 ]] && echo true || echo false)" >"$output"
  (( status == 0 )) || die "TiDB did not stop cleanly (status $status)"
}

write_store_config() {
  local directory=$1
  printf '%s\n' '[log]' "level = \"$SERVER_LOG_LEVEL\"" \
    '[tikv-client.copr-cache]' 'capacity-mb = 0' >"$directory/tidb.toml"
}

configure_store() {
  local directory=$1
  mkdir "$directory/store"
  [[ -z "$(find "$directory/store" -mindepth 1 -print -quit)" ]] || die "new UniStore directory is not empty"
  write_store_config "$directory"
}

delete_evidence_tree() {
  local evidence_tree=$1 expected_parent=$2
  [[ "$evidence_tree" = "$expected_parent/"* && "$evidence_tree" != "$expected_parent" ]] ||
    die "refusing to prune evidence outside its result root: $evidence_tree"
  if [[ -e "$evidence_tree" ]]; then
    find "$evidence_tree" -depth -delete
  fi
}

record_retention() {
  local suite_root=$1
  shift
  python3 - "$suite_root/retention.json" "$SERVER_LOG_LEVEL" "$@" <<'PY'
import datetime, json, pathlib, sys
path = pathlib.Path(sys.argv[1])
record = json.loads(path.read_text()) if path.exists() else {
    "format_version": 1,
    "policy": "final-results-only",
    "removed": [],
}
record["server_log_level"] = sys.argv[2]
record["removed"] = sorted(set(record["removed"]) | set(sys.argv[3:]))
record["updated_at_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
PY
}

prune_successful_attempt_store() {
  local suite_root=$1 attempt=$2
  [[ "$FINAL_RESULTS_ONLY" = true ]] || return 0
  delete_evidence_tree "$attempt/store" "$suite_root"
}

prune_successful_suite() {
  local suite_root=$1
  [[ "$FINAL_RESULTS_ONLY" = true ]] || return 0
  record_retention "$suite_root" "$suite_root/load-references" "$suite_root/trials"
  delete_evidence_tree "$suite_root/load-references" "$suite_root"
  delete_evidence_tree "$suite_root/trials" "$suite_root"
}

prune_build_artifacts() {
  local suite_root=$1
  [[ "$FINAL_RESULTS_ONLY" = true ]] || return 0
  record_retention "$suite_root" "$suite_root/artifacts"
  delete_evidence_tree "$suite_root/artifacts" "$suite_root"
}

run_sql() {
  local action=$1 output=$2 error=$3
  shift 3
  taskset -c "$CLIENT_CPUSET" "$TIDB_SQL" \
    -action "$action" \
    -address "$SQL_HOST:$SQL_PORT" \
    -database "$DATABASE" \
    -params "$DSN_PARAMS" \
    "$@" >"$output" 2>"$error"
}

run_ycsb_digest() {
  local output=$1 error=$2
  shift 2
  run_sql ycsb-digest "$output" "$error" -timeout "$DIGEST_TIMEOUT" "$@"
}

record_phase() {
  local path=$1 phase=$2 state=$3 status=${4:-}
  python3 - "$path" "$phase" "$state" "$status" <<'PY'
import datetime, json, pathlib, sys, time
path, phase, state, status = sys.argv[1:]
record = {
    "format_version": 1,
    "phase": phase,
    "state": state,
    "exit_status": None if status == "" else int(status),
    "monotonic_ns": time.monotonic_ns(),
    "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
}
with pathlib.Path(path).open("a") as stream:
    stream.write(json.dumps(record, sort_keys=True) + "\n")
PY
}

write_or_verify_experiment() {
  local suite_root=$1 suite_smoke=$2 suite_trials=$3 suite_record_count=$4 suite_threads=$5
  local suite_warmup=$6 suite_operations=$7 calibration_path=$8
  local suite_calibration_sha=
  if [[ -n "$calibration_path" ]]; then suite_calibration_sha=$CALIBRATION_SHA; fi
  mkdir -p "$suite_root"
  python3 - \
    "$suite_root/experiment.json" "$suite_smoke" "$DIAGNOSTIC_ONLY" \
    "$suite_trials" "$WORKLOAD_TEXT" "$MODE_TEXT" "$suite_record_count" "$FIELD_COUNT" "$FIELD_LENGTH" \
    "$suite_threads" "$suite_warmup" "$suite_operations" "$ARTIFACT_MANIFEST_SHA" \
    "$suite_calibration_sha" "$calibration_path" "$TIDB_CPUSET" "$CLIENT_CPUSET" \
    "$QUEUE_CAPACITY_BYTES" "$CAPTURE_CAPACITY_BYTES" "$RETAINED_STATE_CAPACITY_BYTES" \
    "$SQL_HOST" "$SQL_PORT" "$STATUS_PORT" "$DIGEST_PAGE_SIZE" "$STABILIZE_SECONDS" \
    "$DSN_PARAMS" "$TIDB_RETRY_LIMIT" "$QUEUE_TIMEOUT" "$CAPTURE_TIMEOUT" \
    "$DIGEST_TIMEOUT" "$SERVER_LOG_LEVEL" "$FINAL_RESULTS_ONLY" <<'PY'
import hashlib, json, os, pathlib, sys, tempfile
(
    output, smoke, diagnostic, trials, workload_text, mode_text, record_count, field_count,
    field_length, threads, common_warmup, common_operations, artifact_hash,
    calibration_hash, calibration_path, tidb_cpus, client_cpus, queue_bytes,
    capture_bytes, retained_bytes, host, sql_port, status_port, page_size,
    stabilize_seconds, dsn_params, tidb_retry_limit, queue_timeout, capture_timeout,
    digest_timeout, server_log_level, final_results_only,
) = sys.argv[1:]
trials = int(trials)
workloads = workload_text.split(",")
workload_config = {}
if calibration_path:
    calibrated = json.loads(pathlib.Path(calibration_path).read_text())
else:
    calibrated = None
for workload in workloads:
    if calibrated:
        entry = calibrated["workloads"][workload]
        warmup, operations = entry["warmup_operations"], entry["measured_operations"]
    else:
        warmup, operations = int(common_warmup), int(common_operations)
    workload_config[workload] = {
        "profile": f"workload{workload.lower()}.properties",
        "warmup_operations": warmup,
        "measured_operations": operations,
    }
modes = mode_text.split(",")
record = {
    "format_version": 1,
    "profile_id": "co-checker-ycsb-core-v1",
    "smoke": smoke == "true",
    "diagnostic_only": diagnostic == "true",
    "trials": trials,
    "workloads": workloads,
    "modes": modes,
    "mode_rotation": {
        str(trial): modes[(trial - 1) % len(modes):] + modes[:(trial - 1) % len(modes)]
        for trial in range(1, trials + 1)
    },
    "artifact_manifest_sha256": artifact_hash,
    "calibration": None if not calibration_hash else {"path": calibration_path, "sha256": calibration_hash},
    "seed_policy": "fixed-v1",
    "seeds": {"load": 7732542672685676034, "warmup": 6300581044758642952, "measured": 1251957853004150076},
    "configuration": {
        "record_count": int(record_count),
        "field_count": int(field_count),
        "field_length": int(field_length),
        "threads": int(threads),
        "load_threads": 1,
        "batch_size": 1,
        "stabilize_seconds": int(stabilize_seconds),
        "digest_page_size": int(page_size),
        "digest_timeout": digest_timeout,
        "server_log_level": server_log_level,
        "retention_policy": "final-results-only" if final_results_only == "true" else "retain-details",
        "load_store_policy": "fresh-empty-store-and-seeded-ycsb-load",
        "tidb_cpus": tidb_cpus,
        "client_cpus": client_cpus,
        "sql_host": host,
        "sql_port": int(sql_port),
        "status_port": int(status_port),
        "queue_capacity_bytes": int(queue_bytes),
        "capture_capacity_bytes": int(capture_bytes),
        "retained_state_capacity_bytes": int(retained_bytes),
        "queue_timeout": queue_timeout,
        "capture_timeout": capture_timeout,
        "mysql_params": dsn_params,
        "tidb_retry_limit": int(tidb_retry_limit),
        "workloads": workload_config,
    },
}
unsigned = json.dumps(record, sort_keys=True, separators=(",", ":")).encode()
record["experiment_sha256"] = hashlib.sha256(unsigned).hexdigest()
path = pathlib.Path(output)
encoded = json.dumps(record, indent=2, sort_keys=True) + "\n"
if path.exists():
    if path.read_text() != encoded:
        raise SystemExit("cannot resume with a different experiment manifest")
else:
    descriptor, temporary = tempfile.mkstemp(prefix=".experiment.", dir=path.parent)
    with os.fdopen(descriptor, "w") as stream:
        stream.write(encoded); stream.flush(); os.fsync(stream.fileno())
    os.link(temporary, path)
    os.unlink(temporary)
PY
}

validate_ycsb_result() {
  local command=$1 workload=$2 requested=$3 result=$4 output=$5
  python3 - "$command" "$workload" "$requested" "$result" "$output" <<'PY'
import json, pathlib, sys
command, workload, requested, result_name, output_name = sys.argv[1:]
requested = int(requested)
reasons = []
try:
    result = json.loads(pathlib.Path(result_name).read_text())
except (OSError, json.JSONDecodeError) as error:
    result = {}
    reasons.append(f"result unavailable or malformed: {error}")
def count(name):
    value = result.get("measurements", {}).get(name, {}).get("count", 0)
    return value if isinstance(value, int) and value >= 0 else 0
if result.get("format_version") != "go-ycsb-online-result-v1": reasons.append("format mismatch")
if result.get("command") != command: reasons.append("command mismatch")
values = [result.get(name) for name in ("requested", "attempted", "successful", "failed")]
if values != [requested, requested, requested, 0]: reasons.append(f"operation lifecycle mismatch: {values!r}")
if result.get("error_classes") != {}: reasons.append("error classes are nonempty")
raw = {name: count(name) for name in ("READ", "UPDATE", "INSERT", "SCAN", "READ_MODIFY_WRITE", "DELETE")}
if command == "load":
    top = {"INSERT": raw["INSERT"]}
    db = dict(top)
    if raw["INSERT"] != requested: reasons.append("load insert count mismatch")
elif workload == "F":
    rmw = raw["READ_MODIFY_WRITE"]
    top = {"READ": requested - rmw, "READ_MODIFY_WRITE": rmw}
    db = {"READ": raw["READ"], "UPDATE": raw["UPDATE"]}
    if raw["READ"] != requested or raw["UPDATE"] != rmw: reasons.append("workload F DB-call expansion mismatch")
else:
    top = {name: raw[name] for name in ("READ", "UPDATE", "INSERT", "SCAN", "DELETE") if raw[name]}
    db = dict(top)
    if sum(top.values()) != requested: reasons.append("top-level operation mix does not conserve requested work")
record = {
    "format_version": 1,
    "passed": not reasons,
    "reason": "; ".join(reasons),
    "command": command,
    "requested": requested,
    "actual_operation_mix": top,
    "db_call_mix": db,
}
pathlib.Path(output_name).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
if reasons: raise SystemExit("YCSB result validation failed: " + "; ".join(reasons))
PY
}

run_ycsb() {
  local command=$1 workload=$2 profile=$3 record_count=$4 operations=$5 threads=$6 seed=$7
  local result=$8 log=$9 time_output=${10}
  /usr/bin/time -v -o "$time_output" \
    taskset -c "$CLIENT_CPUSET" "$IMMUTABLE_GO_YCSB" "$command" mysql \
      -P "$profile" \
      -p "mysql.host=$SQL_HOST" \
      -p "mysql.port=$SQL_PORT" \
      -p "mysql.user=root" \
      -p "mysql.password=" \
      -p "mysql.db=$DATABASE" \
      -p "mysql.params=$DSN_PARAMS" \
      -p "mysql.create_table=false" \
      -p "mysql.strict_rows=true" \
      -p "load.analyze=false" \
      -p "strict=true" \
      -p "recordcount=$record_count" \
      -p "operationcount=$operations" \
      -p "threadcount=$threads" \
      -p "batch.size=1" \
      -p "randomseed=$seed" \
      -p "warmuptime=0" \
      -p "result.path=$result" \
      >"$log" 2>&1
}

digest_matches_reference() {
  local digest=$1 reference=$2 output=$3
  python3 - "$digest" "$reference" "$DIGEST_PAGE_SIZE" "$output" <<'PY'
import json, pathlib, sys
digest_name, reference_name, page_size, output_name = sys.argv[1:]
digest = json.loads(pathlib.Path(digest_name).read_text())
reference = json.loads(pathlib.Path(reference_name).read_text())
reasons = []
for name in ("schema_digest_sha256", "data_digest_sha256", "database_digest_sha256", "total_rows"):
    if digest.get(name) != reference.get(name): reasons.append(f"{name} differs from pristine reference")
if digest.get("configured_page_size") != int(page_size): reasons.append("digest page-size mismatch")
if not isinstance(digest.get("maximum_observed_page_size"), int) or digest["maximum_observed_page_size"] > int(page_size):
    reasons.append("digest exceeded page bound")
pathlib.Path(output_name).write_text(json.dumps({"passed": not reasons, "reason": "; ".join(reasons)}, indent=2, sort_keys=True) + "\n")
if reasons: raise SystemExit("load digest validation failed: " + "; ".join(reasons))
PY
}

capture_metrics() {
  local expected_mode=$1 output=$2
  python3 - "$expected_mode" "http://$SQL_HOST:$STATUS_PORT/metrics" "$output" <<'PY'
import json, pathlib, re, sys, time, urllib.request
expected, url, output = sys.argv[1:]
sample_pattern = re.compile(r"^(tidb_online_checker_(?:adapter|native|run_info))\{([^}]*)\}\s+([-+0-9.eE]+)$")
verdict_pattern = re.compile(r"^tidb_online_checker_verdict_(available|pass|violation)\s+([-+0-9.eE]+)$")
label_pattern = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)="((?:\\.|[^"\\])*)"')
now = time.monotonic_ns()
adapter, native, identities, verdict = {}, {}, [], {}
text, error, live_success = "", None, False
try:
    with urllib.request.urlopen(url, timeout=5.) as response:
        text = response.read().decode("utf-8", errors="replace")
    for line in text.splitlines():
        stripped = line.strip()
        if stripped == "tidb_online_checker_live_scrape_success 1":
            live_success = True; continue
        match = verdict_pattern.match(stripped)
        if match:
            verdict[match.group(1)] = float(match.group(2)); continue
        match = sample_pattern.match(stripped)
        if not match: continue
        family, label_text, raw_value = match.groups()
        labels = {name: bytes(value, "utf-8").decode("unicode_escape") for name, value in label_pattern.findall(label_text)}
        value = float(raw_value)
        if family.endswith("adapter") and "metric" in labels: adapter[labels["metric"]] = value
        elif family.endswith("native") and "metric" in labels: native[labels["metric"]] = value
        elif family.endswith("run_info"): identities.append((labels, value))
    if expected:
        identity = next((labels for labels, value in identities if value == 1 and labels.get("mode") == expected and labels.get("ordering_protocol") == "serialized-global-lock"), None)
        success = live_success and identity is not None and set(verdict) == {"available", "pass", "violation"}
        if not success: error = "checked scrape lacks live-success, identity, or verdict metrics"
    else:
        identity = None
        success = not any(line.startswith("tidb_online_checker_") for line in text.splitlines())
        if not success: error = "baseline unexpectedly exports online-checker metrics"
except Exception as exc:
    identity = None; success = False; error = f"metrics scrape failed: {exc}"
record = {
    "format_version": 1, "clock": "CLOCK_MONOTONIC", "monotonic_ns": time.monotonic_ns(),
    "checker_enabled": bool(expected), "expected_mode": expected or None,
    "scrape_success": success, "mode": None if identity is None else identity.get("mode"),
    "ordering_protocol": None if identity is None else identity.get("ordering_protocol"),
    "adapter": adapter, "native": native, "verdict": verdict or None, "error": error,
}
pathlib.Path(output).write_text(json.dumps(record, sort_keys=True) + "\n")
if not success: raise SystemExit(error)
PY
}

start_telemetry() {
  local expected_mode=$1 samples=$2 ready=$3
  : >"$samples"
  python3 - "$SCRIPT_PATH" "$expected_mode" "$SQL_HOST" "$STATUS_PORT" "$samples" "$ready" <<'PY' &
import json, pathlib, re, signal, sys, time, urllib.request
_, expected, host, port, samples_name, ready_name = sys.argv[1:]
samples, ready = pathlib.Path(samples_name), pathlib.Path(ready_name)
stop = False
sample_pattern = re.compile(r"^(tidb_online_checker_(?:adapter|native|run_info))\{([^}]*)\}\s+([-+0-9.eE]+)$")
verdict_pattern = re.compile(r"^tidb_online_checker_verdict_(available|pass|violation)\s+([-+0-9.eE]+)$")
label_pattern = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)="((?:\\.|[^"\\])*)"')
def request_stop(_signal, _frame):
    global stop
    stop = True
signal.signal(signal.SIGTERM, request_stop)
signal.signal(signal.SIGINT, request_stop)
def scrape():
    adapter, native, identities, verdict = {}, {}, [], {}
    error, text, live_success = None, "", False
    try:
        with urllib.request.urlopen(f"http://{host}:{port}/metrics", timeout=2.) as response:
            text = response.read().decode("utf-8", errors="replace")
        for line in text.splitlines():
            stripped = line.strip()
            if stripped == "tidb_online_checker_live_scrape_success 1": live_success = True; continue
            match = verdict_pattern.match(stripped)
            if match: verdict[match.group(1)] = float(match.group(2)); continue
            match = sample_pattern.match(stripped)
            if not match: continue
            family, label_text, raw = match.groups()
            labels = {name: bytes(value, "utf-8").decode("unicode_escape") for name, value in label_pattern.findall(label_text)}
            value = float(raw)
            if family.endswith("adapter") and "metric" in labels: adapter[labels["metric"]] = value
            elif family.endswith("native") and "metric" in labels: native[labels["metric"]] = value
            elif family.endswith("run_info"): identities.append((labels, value))
        if expected:
            identity = next((labels for labels, value in identities if value == 1 and labels.get("mode") == expected and labels.get("ordering_protocol") == "serialized-global-lock"), None)
            success = live_success and identity is not None and set(verdict) == {"available", "pass", "violation"}
            if not success: error = "checked scrape lacks live-success, identity, or verdict metrics"
        else:
            identity = None
            success = not any(line.startswith("tidb_online_checker_") for line in text.splitlines())
            if not success: error = "baseline unexpectedly exports online-checker metrics"
    except Exception as exc:
        identity = None; success = False; error = f"metrics scrape failed: {exc}"
    return {
        "format_version": 1, "clock": "CLOCK_MONOTONIC", "monotonic_ns": time.monotonic_ns(),
        "checker_enabled": bool(expected), "expected_mode": expected or None,
        "scrape_success": success, "mode": None if identity is None else identity.get("mode"),
        "ordering_protocol": None if identity is None else identity.get("ordering_protocol"),
        "adapter": adapter, "native": native, "verdict": verdict or None, "error": error,
    }
with samples.open("a") as stream:
    while not stop:
        stream.write(json.dumps(scrape(), sort_keys=True) + "\n"); stream.flush()
        if not ready.exists(): ready.write_text(json.dumps({"ready": True, "monotonic_ns": time.monotonic_ns()}) + "\n")
        deadline = time.monotonic() + 1.
        while not stop and time.monotonic() < deadline: time.sleep(.05)
PY
  TELEMETRY_PID=$!
  for _ in $(seq 1 100); do
    [[ -s "$ready" ]] && return
    kill -0 "$TELEMETRY_PID" 2>/dev/null || die "telemetry monitor stopped during startup"
    sleep .05
  done
  die "telemetry monitor did not become ready"
}

stop_telemetry() {
  if [[ -n "${TELEMETRY_PID:-}" ]]; then
    kill -TERM "$TELEMETRY_PID" 2>/dev/null || true
    wait "$TELEMETRY_PID" 2>/dev/null || true
    TELEMETRY_PID=
  fi
}

start_resource_monitor() {
  local pid=$1 samples=$2 output=$3
  python3 - "$pid" "$samples" "$output" <<'PY' &
import json, os, pathlib, signal, sys, time
pid, samples_name, output_name = int(sys.argv[1]), sys.argv[2], sys.argv[3]
stop = False
def request_stop(_signal, _frame):
    global stop
    stop = True
signal.signal(signal.SIGTERM, request_stop); signal.signal(signal.SIGINT, request_stop)
ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
first_cpu = last_cpu = None; max_rss = max_hwm = count = 0; started = time.monotonic_ns()
with pathlib.Path(samples_name).open("w") as stream:
    stream.write("monotonic_ns vmrss_kib vmhwm_kib process_cpu_ticks\n")
    while not stop:
        try:
            status = pathlib.Path(f"/proc/{pid}/status").read_text()
            fields = pathlib.Path(f"/proc/{pid}/stat").read_text().split()
        except OSError: break
        rss = hwm = 0
        for line in status.splitlines():
            if line.startswith("VmRSS:"): rss = int(line.split()[1])
            elif line.startswith("VmHWM:"): hwm = int(line.split()[1])
        cpu = int(fields[13]) + int(fields[14])
        if first_cpu is None: first_cpu = cpu
        last_cpu = cpu; max_rss = max(max_rss, rss); max_hwm = max(max_hwm, hwm); count += 1
        stream.write(f"{time.monotonic_ns()} {rss} {hwm} {cpu}\n"); stream.flush(); time.sleep(.1)
ended = time.monotonic_ns()
cpu_seconds = None if first_cpu is None or last_cpu is None else (last_cpu - first_cpu) / ticks
duration = (ended - started) / 1e9
record = {
    "clock": "CLOCK_MONOTONIC", "started_ns": started, "ended_ns": ended, "samples": count,
    "maximum_rss_kib": max_rss, "maximum_hwm_kib": max_hwm, "process_cpu_seconds": cpu_seconds,
    "cpu_mean_percent": None if cpu_seconds is None or duration <= 0 else cpu_seconds / duration * 100.,
}
pathlib.Path(output_name).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
PY
  RESOURCE_MONITOR_PID=$!
}

stop_resource_monitor() {
  if [[ -n "${RESOURCE_MONITOR_PID:-}" ]]; then
    kill -TERM "$RESOURCE_MONITOR_PID" 2>/dev/null || true
    wait "$RESOURCE_MONITOR_PID" 2>/dev/null || true
    RESOURCE_MONITOR_PID=
  fi
}

parse_time_resource() {
  local input=$1 output=$2
  python3 - "$input" "$output" <<'PY'
import json, pathlib, re, sys
text = pathlib.Path(sys.argv[1]).read_text()
def field(name, cast=float):
    match = re.search(rf"^\s*{re.escape(name)}:\s*(.+?)\s*$", text, re.MULTILINE)
    if not match: return None
    try: return cast(match.group(1))
    except ValueError: return None
user = field("User time (seconds)")
system = field("System time (seconds)")
record = {
    "user_cpu_seconds": user, "system_cpu_seconds": system,
    "user_system_cpu_seconds": None if user is None or system is None else user + system,
    "maximum_resident_set_size_kib": field("Maximum resident set size (kbytes)", int),
    "voluntary_context_switches": field("Voluntary context switches", int),
    "involuntary_context_switches": field("Involuntary context switches", int),
    "file_system_inputs": field("File system inputs", int),
    "file_system_outputs": field("File system outputs", int),
}
pathlib.Path(sys.argv[2]).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
PY
}

summarize_resource_interval() {
  local samples=$1 start_ns=$2 exit_ns=$3 monitor_summary=$4 output=$5
  python3 - "$samples" "$start_ns" "$exit_ns" "$monitor_summary" "$output" <<'PY'
import json, os, pathlib, sys
samples_name, start_text, exit_text, monitor_name, output_name = sys.argv[1:]
start, end = int(start_text), int(exit_text)
rows = []
for line in pathlib.Path(samples_name).read_text().splitlines()[1:]:
    fields = line.split()
    if len(fields) != 4: continue
    timestamp, rss, hwm, cpu = map(int, fields)
    if start <= timestamp <= end: rows.append((timestamp, rss, hwm, cpu))
monitor = json.loads(pathlib.Path(monitor_name).read_text())
ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
cpu = None if len(rows) < 2 else (rows[-1][3] - rows[0][3]) / ticks
duration = (end - start) / 1e9
record = {
    "clock": "CLOCK_MONOTONIC", "started_ns": start, "ended_ns": end,
    "samples": len(rows), "maximum_rss_kib": max((row[1] for row in rows), default=monitor.get("maximum_rss_kib")),
    "maximum_hwm_kib": max((row[2] for row in rows), default=monitor.get("maximum_hwm_kib")),
    "process_cpu_seconds": cpu, "cpu_mean_percent": None if cpu is None or duration <= 0 else cpu / duration * 100.,
    "monitor_lifetime_summary": monitor,
}
pathlib.Path(output_name).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
PY
}

wait_for_drain() {
  local runtime_mode=$1 output=$2 raw=$3 cutoff=${4:-}
  if [[ -z "$runtime_mode" ]]; then
    python3 - "$output" "$cutoff" <<'PY'
import json, pathlib, sys, time
cutoff = json.loads(pathlib.Path(sys.argv[2]).read_text()) if sys.argv[2] else {}
client_exit = cutoff.get("workload_client_exit_monotonic_ns", time.monotonic_ns())
pathlib.Path(sys.argv[1]).write_text(json.dumps({
    "format_version": 1, "passed": True, "drained": True, "mode": "baseline-original",
    "checker": "disabled", "workload_cutoff_monotonic_ns": client_exit,
    "drain_started_monotonic_ns": client_exit, "drain_complete_monotonic_ns": client_exit,
    "cutoff_residual_transactions": 0, "cutoff_residual_events": 0,
    "drained_transactions": 0, "drained_events": 0, "reason": "",
}, indent=2, sort_keys=True) + "\n")
PY
    : >"$raw"
    return
  fi
  python3 - "$runtime_mode" "http://$SQL_HOST:$STATUS_PORT/metrics" "$output" "$raw" "$CHECKER_DRAIN_TIMEOUT" "$cutoff" <<'PY'
import json, pathlib, re, sys, time, urllib.request
mode, url, output_name, raw_name, timeout_text, cutoff_name = sys.argv[1:]
def duration(value):
    units = {"ms": .001, "s": 1., "m": 60., "h": 3600.}
    parts = re.findall(r"([0-9]+(?:\.[0-9]+)?)(ms|h|m|s)", value)
    return sum(float(number) * units[unit] for number, unit in parts)
sample_pattern = re.compile(r"^(tidb_online_checker_(?:adapter|native|run_info))\{([^}]*)\}\s+([-+0-9.eE]+)$")
verdict_pattern = re.compile(r"^tidb_online_checker_verdict_(available|pass|violation)\s+([-+0-9.eE]+)$")
label_pattern = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)="((?:\\.|[^"\\])*)"')
def parse(text):
    adapter, native, identities, verdict = {}, {}, [], {}
    live = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped == "tidb_online_checker_live_scrape_success 1": live = True; continue
        match = verdict_pattern.match(stripped)
        if match: verdict[match.group(1)] = float(match.group(2)); continue
        match = sample_pattern.match(stripped)
        if not match: continue
        family, labels_text, raw = match.groups()
        labels = {name: bytes(value, "utf-8").decode("unicode_escape") for name, value in label_pattern.findall(labels_text)}
        value = float(raw)
        if family.endswith("adapter") and "metric" in labels: adapter[labels["metric"]] = value
        elif family.endswith("native") and "metric" in labels: native[labels["metric"]] = value
        elif family.endswith("run_info"): identities.append((labels, value))
    return adapter, native, identities, verdict, live
def residual(native):
    target = {"capture-only": "recycled_transactions", "order-only": "ordered_transactions", "full": "checked_transactions"}[mode]
    accepted, terminal = native.get("accepted_publications"), native.get(target)
    tx = None if accepted is None or terminal is None else accepted - terminal
    events = None if mode != "full" else native.get("accepted_events", 0) - native.get("checked_events", 0)
    return tx, events
def evaluate(adapter, native, identities, verdict, live):
    reasons = []
    if not live: reasons.append("live scrape failed")
    if not any(value == 1 and labels.get("mode") == mode and labels.get("ordering_protocol") == "serialized-global-lock" for labels, value in identities): reasons.append("run identity missing")
    for field in ("active_attempts", "active_finalizers", "active_fragments", "fragment_buffer_active_bytes", "fragment_retained_active_bytes"):
        if adapter.get(field) != 0: reasons.append(f"adapter {field} did not drain")
    for field in ("active_attempts", "queue_reserved_bytes", "queue_reserved_depth", "queue_bytes", "queue_depth", "reorder_bytes"):
        if native.get(field) != 0: reasons.append(f"native {field} did not drain")
    if adapter.get("fatal_calls") != 0 or adapter.get("fragment_capture_errors") != 0: reasons.append("adapter error counter is nonzero or missing")
    if native.get("fatal") != 0: reasons.append("native fatal is nonzero or missing")
    registered, published, aborted = (adapter.get(name) for name in ("attempts_registered", "attempts_published", "attempts_aborted"))
    if registered is None or published is None or aborted is None or registered != published + aborted: reasons.append("attempt lifecycle imbalance")
    accepted = adapter.get("accepted_publications")
    if accepted is None or accepted != published or accepted != native.get("accepted_publications"): reasons.append("publication lifecycle imbalance")
    if adapter.get("successful_write_commits", -1) + adapter.get("read_only_no_write_completions", -1) != accepted: reasons.append("completion lifecycle imbalance")
    if mode == "capture-only" and native.get("recycled_transactions") != accepted: reasons.append("capture recycling incomplete")
    if mode != "capture-only" and native.get("ordered_transactions") != accepted: reasons.append("ordering incomplete")
    if mode == "full":
        if native.get("checked_transactions") != accepted or native.get("checked_events") != native.get("accepted_events"): reasons.append("checking incomplete")
        if verdict != {"available": 1., "pass": 1., "violation": 0.}: reasons.append("live verdict is not pass")
    return reasons
cutoff = json.loads(pathlib.Path(cutoff_name).read_text()) if cutoff_name else None
initial_adapter = cutoff.get("adapter", {}) if cutoff else None
initial_native = cutoff.get("native", {}) if cutoff else None
started = time.monotonic_ns(); deadline = time.monotonic() + duration(timeout_text)
last_adapter = last_native = {}; last_verdict = {}; reasons = ["no scrape"]
last_text = ""; complete = None; attempts = failures = 0
while time.monotonic() < deadline:
    attempts += 1
    try:
        with urllib.request.urlopen(url, timeout=5.) as response: last_text = response.read().decode("utf-8", errors="replace")
        adapter, native, identities, verdict, live = parse(last_text)
        if initial_adapter is None: initial_adapter, initial_native = dict(adapter), dict(native)
        last_adapter, last_native, last_verdict = adapter, native, verdict
        reasons = evaluate(adapter, native, identities, verdict, live)
        if not reasons:
            if cutoff and (adapter.get("accepted_publications") != initial_adapter.get("accepted_publications") or native.get("accepted_events") != initial_native.get("accepted_events")):
                reasons.append("accepted work changed after cutoff")
            else:
                complete = time.monotonic_ns(); break
    except Exception as error:
        failures += 1; reasons = [f"scrape failed: {error}"]
    time.sleep(.05)
pathlib.Path(raw_name).write_text(last_text)
initial_adapter, initial_native = initial_adapter or {}, initial_native or {}
initial_tx, initial_events = residual(initial_native); final_tx, final_events = residual(last_native)
record = {
    "format_version": 1, "passed": not reasons, "drained": not reasons, "mode": mode,
    "workload_cutoff_monotonic_ns": cutoff.get("workload_client_exit_monotonic_ns") if cutoff else None,
    "drain_started_monotonic_ns": started, "drain_complete_monotonic_ns": complete,
    "scrape_attempts": attempts, "scrape_failures": failures, "reason": "; ".join(reasons),
    "initial_adapter_metrics": initial_adapter, "initial_native_metrics": initial_native,
    "adapter_metrics": last_adapter, "native_metrics": last_native, "verdict": last_verdict or None,
    "cutoff_residual_transactions": initial_tx, "cutoff_residual_events": initial_events,
    "drained_transactions": None if initial_tx is None or final_tx is None else initial_tx - final_tx,
    "drained_events": None if initial_events is None or final_events is None else initial_events - final_events,
}
pathlib.Path(output_name).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
if reasons: raise SystemExit("checker drain failed: " + "; ".join(reasons))
PY
}

validate_telemetry() {
  local runtime_mode=$1 samples=$2 start_ns=$3 exit_ns=$4 cutoff=$5 output=$6
  python3 - "$runtime_mode" "$samples" "$start_ns" "$exit_ns" "$cutoff" "$output" <<'PY'
import json, math, pathlib, sys
mode, samples_name, start_text, exit_text, cutoff_name, output_name = sys.argv[1:]
start, client_exit = int(start_text), int(exit_text)
reasons = []
samples = []
for number, line in enumerate(pathlib.Path(samples_name).read_text().splitlines(), 1):
    try: value = json.loads(line)
    except json.JSONDecodeError as error:
        reasons.append(f"sample {number} malformed: {error}"); continue
    timestamp = value.get("monotonic_ns")
    if isinstance(timestamp, int) and start <= timestamp:
        samples.append(value)
cutoff = json.loads(pathlib.Path(cutoff_name).read_text())
cutoff_ts = cutoff.get("monotonic_ns")
if not isinstance(cutoff_ts, int): reasons.append("cutoff timestamp missing"); cutoff_ts = client_exit
if cutoff_ts < client_exit: reasons.append("cutoff precedes client exit")
delay = (cutoff_ts - client_exit) / 1e9
if delay > 1.: reasons.append(f"cutoff observation delay {delay:.3f}s exceeds 1s")
samples = [sample for sample in samples if sample.get("monotonic_ns", cutoff_ts + 1) <= cutoff_ts]
if not samples or samples[-1].get("monotonic_ns") != cutoff_ts:
    samples.append(cutoff)
samples.sort(key=lambda sample: sample["monotonic_ns"])
timestamps = [sample["monotonic_ns"] for sample in samples]
if any(right <= left for left, right in zip(timestamps, timestamps[1:])): reasons.append("telemetry timestamps are not strictly monotonic")
gaps = [(right - left) / 1e9 for left, right in zip(timestamps, timestamps[1:])]
maximum_gap = max(gaps, default=0.)
if maximum_gap > 3.: reasons.append(f"telemetry gap {maximum_gap:.3f}s exceeds 3s")
duration = max(0., (client_exit - start) / 1e9)
expected = max(1, math.floor(duration) + 1)
coverage = min(100., len(samples) / expected * 100.)
if coverage < 95.: reasons.append(f"telemetry coverage {coverage:.2f}% is below 95%")
if any(sample.get("scrape_success") is not True for sample in samples): reasons.append("one or more telemetry scrapes failed")
if mode:
    if any(sample.get("mode") != mode or sample.get("ordering_protocol") != "serialized-global-lock" for sample in samples): reasons.append("telemetry identity mismatch")
else:
    if any(sample.get("checker_enabled") is not False or sample.get("adapter") or sample.get("native") for sample in samples): reasons.append("baseline exposed checker telemetry")
counters = ("accepted_publications", "accepted_events", "recycled_transactions", "ordered_transactions", "checked_transactions", "checked_events")
for field in counters:
    values = [sample.get("native", {}).get(field) for sample in samples]
    numeric = [value for value in values if isinstance(value, (int, float))]
    if mode and len(numeric) != len(samples): reasons.append(f"telemetry counter {field} is missing")
    if any(right < left for left, right in zip(numeric, numeric[1:])): reasons.append(f"telemetry counter {field} decreased")
def delta(field):
    first = samples[0].get("native", {}).get(field); last = samples[-1].get("native", {}).get(field)
    return last - first if isinstance(first, (int, float)) and isinstance(last, (int, float)) else 0
accepted_delta = delta("accepted_publications"); event_delta = delta("accepted_events"); recycled_delta = delta("recycled_transactions")
ordered_delta = delta("ordered_transactions"); checked_delta = delta("checked_transactions"); checked_event_delta = delta("checked_events")
def residual(sample):
    native = sample.get("native", {})
    accepted = native.get("accepted_publications", 0)
    target = native.get({"capture-only": "recycled_transactions", "order-only": "ordered_transactions", "full": "checked_transactions"}.get(mode, "accepted_publications"), accepted)
    return accepted - target
backlogs = [residual(sample) for sample in samples]
event_backlogs = [sample.get("native", {}).get("accepted_events", 0) - sample.get("native", {}).get("checked_events", 0) if mode == "full" else 0 for sample in samples]
queue_depths = [sample.get("native", {}).get("queue_depth", 0) for sample in samples]
queue_bytes = [sample.get("native", {}).get("queue_bytes", 0) for sample in samples]
capture_bytes = [sample.get("native", {}).get("capture_peak_bytes", 0) for sample in samples]
retained_bytes = [sample.get("native", {}).get("peak_retained_state_bytes", 0) for sample in samples]
sustained = len(backlogs) >= 4 and all(right > left for left, right in zip(backlogs[-4:], backlogs[-3:]))
residual_at_cutoff = backlogs[-1] if backlogs else 0
tail = 0. if accepted_delta <= 0 else max(0., residual_at_cutoff) / accepted_delta * 100.
record = {
    "format_version": 1, "passed": not reasons, "reason": "; ".join(reasons),
    "sample_count": len(samples), "expected_sample_count": expected,
    "sample_coverage_percent": coverage, "max_sample_gap_seconds": maximum_gap,
    "cutoff_observation_delay_seconds": delay,
    "accepted_publications_delta": accepted_delta, "accepted_events_delta": event_delta,
    "ordered_transactions_delta": ordered_delta, "checked_transactions_delta": checked_delta,
    "checked_events_delta": checked_event_delta,
    "transaction_service_rate_per_second": None if duration <= 0 else ({"capture-only": recycled_delta, "order-only": ordered_delta, "full": checked_delta}.get(mode, 0) / duration),
    "event_service_rate_per_second": None if duration <= 0 else (checked_event_delta / duration if mode == "full" else event_delta / duration),
    "peak_transaction_backlog": max(backlogs, default=0), "peak_event_backlog": max(event_backlogs, default=0),
    "peak_queue_depth": max(queue_depths, default=0), "peak_queue_bytes": max(queue_bytes, default=0),
    "capture_peak_bytes": max(capture_bytes, default=0), "retained_state_peak_bytes": max(retained_bytes, default=0),
    "tail_percent": tail, "sustained_backlog_growth": sustained,
}
pathlib.Path(output_name).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
if reasons: raise SystemExit("telemetry validation failed: " + "; ".join(reasons))
PY
}

validate_checker_result() {
  local runtime_mode=$1 result_path=$2 output=$3
  python3 - "$runtime_mode" "$result_path" "$output" <<'PY'
import json, pathlib, sys
mode, result_name, output_name = sys.argv[1:]
reasons = []
try: result = json.loads(pathlib.Path(result_name).read_text())
except (OSError, json.JSONDecodeError) as error: result = {}; reasons.append(f"result unavailable: {error}")
adapter = result.get("adapter_metrics", {}); native = result.get("native_metrics", {})
expected = {"capture-only": "capture-only-complete", "order-only": "order-only-complete", "full": "pass"}[mode]
def require(condition, message):
    if not condition: reasons.append(message)
require(result.get("format_version") == 1, "format mismatch")
require(result.get("mode") == mode, "mode mismatch")
require(result.get("ordering_protocol") == "serialized-global-lock", "ordering protocol mismatch")
require(result.get("outcome") == expected, "outcome mismatch")
require(result.get("close_status") == 0, "close status is nonzero")
require(result.get("runtime_error") is None and not result.get("close_error"), "runtime/close error is present")
require(isinstance(adapter, dict) and isinstance(native, dict), "metrics are missing")
registered, published, aborted = (adapter.get(name) for name in ("attempts_registered", "attempts_published", "attempts_aborted"))
accepted = adapter.get("accepted_publications")
require(registered is not None and registered == published + aborted, "attempt counters do not balance")
require(accepted is not None and accepted > 0 and accepted == published == native.get("accepted_publications"), "publication counters do not balance")
require(adapter.get("successful_write_commits", -1) + adapter.get("read_only_no_write_completions", -1) == accepted, "completion counters do not balance")
started, fragment_accepted, discarded = (adapter.get(name) for name in ("fragments_started", "fragments_accepted", "fragments_discarded"))
require(started is not None and started > 0 and started == fragment_accepted + discarded, "fragment counters do not balance")
require(adapter.get("fragments_late_discarded", 0) <= discarded, "late-discarded fragments exceed discarded fragments")
for field in ("fatal_calls", "publish_errors", "abort_errors", "fragment_capture_errors"):
    require(adapter.get(field) == 0, f"adapter {field} is nonzero")
for field in ("active_attempts", "active_finalizers", "active_fragments", "fragment_buffer_active_bytes", "fragment_retained_active_bytes"):
    require(adapter.get(field) == 0, f"adapter {field} did not drain")
for field in ("active_attempts", "queue_reserved_bytes", "queue_reserved_depth", "queue_bytes", "queue_depth", "reorder_bytes"):
    require(native.get(field) == 0, f"native {field} did not drain")
require(native.get("closed") is True and native.get("fatal") is False, "native runtime did not close cleanly")
require(native.get("captured_transactions", 0) > 0 and native.get("captured_events", 0) > 0, "native capture did not advance")
if mode == "capture-only": require(native.get("recycled_transactions") == accepted, "recycling incomplete")
else: require(native.get("ordered_transactions") == accepted, "ordering incomplete")
if mode == "full":
    require(native.get("checked_transactions") == accepted, "checking incomplete")
    require(native.get("checked_events") == native.get("accepted_events"), "event checking incomplete")
    require(native.get("violating_transactions") == 0, "violation found")
    require(result.get("verdict", {}).get("status") == "pass", "full verdict is not pass")
record = {"format_version": 1, "passed": not reasons, "reason": "; ".join(reasons), "mode": mode, "outcome": result.get("outcome")}
pathlib.Path(output_name).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
if reasons: raise SystemExit("checker result validation failed: " + "; ".join(reasons))
PY
}

validate_server_logs() {
  local directory=$1 output=$2
  python3 - "$directory" "$output" <<'PY'
import json, pathlib, re, sys
directory, output = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
patterns = {
    "panic": re.compile(r"(?:^|\s)panic:\s", re.I),
    "runtime_fatal": re.compile(r"fatal error:\s", re.I),
    "tidb_fatal": re.compile(r"\[(?:FATAL|DPANIC)\]", re.I),
    "checker_start": re.compile(r"failed to start online transaction checker", re.I),
    "checker_close": re.compile(r"online transaction checker did not close cleanly", re.I),
}
matches = []
for path in sorted(directory.glob("*server*.txt")):
    text = path.read_text(errors="replace")
    for name, pattern in patterns.items():
        if pattern.search(text): matches.append({"file": path.name, "pattern": name})
record = {"format_version": 1, "passed": not matches, "matches": matches}
output.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
if matches: raise SystemExit(f"fatal server log patterns found: {matches!r}")
PY
}

validate_post_check() {
  local initial=$1 final=$2 warmup_result=$3 workload_result=$4 output=$5
  python3 - "$initial" "$final" "$warmup_result" "$workload_result" "$DIGEST_PAGE_SIZE" "$output" <<'PY'
import json, pathlib, sys
initial_name, final_name, warmup_name, workload_name, page_size, output_name = sys.argv[1:]
initial, final, warmup, workload = (json.loads(pathlib.Path(name).read_text()) for name in (initial_name, final_name, warmup_name, workload_name))
def count(record, name): return record.get("measurements", {}).get(name, {}).get("count", 0)
expected = initial.get("total_rows", 0) + count(warmup, "INSERT") + count(workload, "INSERT") - count(workload, "DELETE")
reasons = []
if final.get("total_rows") != expected: reasons.append(f"row count {final.get('total_rows')} != {expected}")
if final.get("schema_digest_sha256") != initial.get("schema_digest_sha256"): reasons.append("schema digest changed")
if final.get("configured_page_size") != int(page_size) or final.get("maximum_observed_page_size", int(page_size) + 1) > int(page_size): reasons.append("digest page bound failed")
record = {"format_version": 1, "passed": not reasons, "reason": "; ".join(reasons), "initial_rows": initial.get("total_rows"), "expected_rows": expected, "final_rows": final.get("total_rows")}
pathlib.Path(output_name).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
if reasons: raise SystemExit("post-check validation failed: " + "; ".join(reasons))
PY
}

write_capacity_preflight() {
  local suite_root=$1
  python3 - "$suite_root/capacity-preflight.json" "$QUEUE_CAPACITY_BYTES" "$CAPTURE_CAPACITY_BYTES" "$RETAINED_STATE_CAPACITY_BYTES" "$suite_root" "$SMOKE" <<'PY'
import json, pathlib, shutil, sys
output, queue, capture, retained, root, smoke = sys.argv[1:]
queue, capture, retained = map(int, (queue, capture, retained))
meminfo = {}
for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
    name, raw = line.split(":", 1)
    meminfo[name] = int(raw.split()[0]) * 1024
available = meminfo.get("MemAvailable", meminfo.get("MemTotal", 0))
disk = shutil.disk_usage(root).free
headroom = 8 * 1024**3
requested = queue + capture + retained + headroom
passed = smoke == "true" or (requested <= available and capture + retained <= disk)
reasons = []
if smoke != "true" and requested > available: reasons.append("requested checker capacities plus 8 GiB headroom exceed available RAM")
if smoke != "true" and capture + retained > disk: reasons.append("capture/retained capacities exceed free result disk")
record = {
    "format_version": 1, "passed": passed, "reason": "; ".join(reasons),
    "queue_capacity_bytes": queue, "capture_capacity_bytes": capture,
    "retained_state_capacity_bytes": retained, "server_client_headroom_bytes": headroom,
    "available_memory_bytes": available, "free_result_disk_bytes": disk,
    "smoke_exemption": smoke == "true",
}
pathlib.Path(output).write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
if not passed: raise SystemExit("capacity preflight failed: " + "; ".join(reasons))
PY
}

load_reference_identity() {
  local workload=$1 record_count=$2 seed=$3
  python3 - "$ARTIFACT_INDEX" "$workload" "$record_count" "$seed" \
    "$FIELD_COUNT" "$FIELD_LENGTH" "$DSN_PARAMS" "$DIGEST_PAGE_SIZE" <<'PY'
import hashlib, json, pathlib, sys
manifest_name, workload, record_count, seed, field_count, field_length, dsn_params, page_size = sys.argv[1:]
manifest = json.loads(pathlib.Path(manifest_name).read_text())
artifacts = manifest["artifacts"]
payload = {
    "format": "ycsb-pristine-load-reference-v1",
    "pristine_tidb_commit": manifest["inputs"]["pristine_tidb"]["commit"],
    "go_ycsb_binary_sha256": artifacts["go_ycsb"]["sha256"],
    "go_ycsb_patch_sha256": artifacts["go_ycsb_patch"]["sha256"],
    "tidb_sql_sha256": artifacts["tidb_sql"]["sha256"],
    "profile_sha256": artifacts[f"profile_{workload}"]["sha256"],
    "record_count": int(record_count),
    "field_count": int(field_count),
    "field_length": int(field_length),
    "load_seed": int(seed),
    "dsn_params": dsn_params,
    "digest_page_size": int(page_size),
}
encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
print(hashlib.sha256(encoded).hexdigest())
PY
}

ensure_load_reference() {
  local suite_root=$1 workload=$2 record_count=$3
  local directory=${4:-$suite_root/load-references/workload-$workload}
  local profile="$(dirname "$IMMUTABLE_GO_YCSB")/profiles/workload${workload,,}.properties"
  local seed identity
  seed=$LOAD_SEED
  identity=$(load_reference_identity "$workload" "$record_count" "$seed")
  if [[ -s "$directory/completed.json" ]]; then
    python3 - "$directory/configuration.json" "$record_count" "$seed" "$identity" <<'PY'
import json, pathlib, sys
value = json.loads(pathlib.Path(sys.argv[1]).read_text())
expected = {"record_count": int(sys.argv[2]), "seed": int(sys.argv[3]), "reference_identity_sha256": sys.argv[4]}
if any(value.get(name) != item for name, item in expected.items()): raise SystemExit("load-reference configuration mismatch")
PY
    [[ -s "$directory/load-digest.json" ]] || die "completed load-reference digest is missing: $directory/load-digest.json"
    return
  fi
  [[ ! -e "$directory" ]] || die "incomplete load reference exists: $directory"
  mkdir -p "$directory"
  configure_store "$directory"
  python3 - "$directory/configuration.json" "$record_count" "$seed" "$identity" <<'PY'
import json, pathlib, sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({"format_version": 1, "record_count": int(sys.argv[2]), "seed": int(sys.argv[3]), "reference_identity_sha256": sys.argv[4]}, indent=2, sort_keys=True) + "\n")
PY
  clear_online_checker_environment
  start_server "$PRISTINE_TIDB_SERVER" "$PRISTINE_TIDB_COMMIT" "$directory" schema
  run_sql ycsb-schema "$directory/schema.json" "$directory/schema-error.txt" -field-count "$FIELD_COUNT" -field-length "$FIELD_LENGTH"
  run_sql assert-ycsb-empty "$directory/empty.json" "$directory/empty-error.txt"
  stop_server "$directory/schema-server-status.json"
  clear_online_checker_environment
  start_server "$PRISTINE_TIDB_SERVER" "$PRISTINE_TIDB_COMMIT" "$directory" load
  run_ycsb load "$workload" "$profile" "$record_count" "$record_count" 1 "$seed" \
    "$directory/load-result.json" "$directory/load.txt" "$directory/load-time.txt"
  validate_ycsb_result load "$workload" "$record_count" "$directory/load-result.json" "$directory/load-validation.json"
  run_ycsb_digest "$directory/load-digest.json" "$directory/load-digest-error.txt" \
    -field-count "$FIELD_COUNT" -field-length "$FIELD_LENGTH" -digest-page-size "$DIGEST_PAGE_SIZE"
  python3 - "$directory/load-digest.json" "$record_count" "$DIGEST_PAGE_SIZE" <<'PY'
import json, pathlib, sys
value = json.loads(pathlib.Path(sys.argv[1]).read_text())
if value.get("total_rows") != int(sys.argv[2]): raise SystemExit("pristine load-reference row count mismatch")
if value.get("configured_page_size") != int(sys.argv[3]) or value.get("maximum_observed_page_size", int(sys.argv[3]) + 1) > int(sys.argv[3]): raise SystemExit("pristine load-reference digest page bound failed")
PY
  stop_server "$directory/load-server-status.json"
  validate_server_logs "$directory" "$directory/server-log-validation.json"
  python3 - "$directory/completed.json" <<'PY'
import datetime, json, pathlib, sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({"format_version": 1, "status": "complete", "completed_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}, indent=2, sort_keys=True) + "\n")
PY
}

attempt_is_valid() {
  local suite_root=$1 workload=$2 trial=$3 mode=$4 attempt=$5
  "$IMMUTABLE_ANALYZER" "$suite_root" --validate-attempt "$workload" "$trial" "$mode" "$attempt" >/dev/null 2>&1
}

next_attempt_directory() {
  local suite_root=$1 workload=$2 trial=$3 mode=$4
  local parent="$suite_root/trials/workload-$workload/trial-$(printf '%03d' "$trial")/$mode"
  mkdir -p "$parent"
  local name
  name=$(next_numbered_directory "$parent" attempt)
  printf '%s/%s\n' "$parent" "$name"
}

newest_attempt() {
  local suite_root=$1 workload=$2 trial=$3 mode=$4
  local parent="$suite_root/trials/workload-$workload/trial-$(printf '%03d' "$trial")/$mode"
  [[ -d "$parent" ]] || return 1
  python3 - "$parent" <<'PY'
import pathlib, re, sys
parent = pathlib.Path(sys.argv[1]); pattern = re.compile(r"attempt-(\d+)$")
values = [(int(match.group(1)), path) for path in parent.iterdir() if path.is_dir() and (match := pattern.fullmatch(path.name))]
if not values: raise SystemExit(1)
print(max(values)[1])
PY
}

write_attempt_configuration() {
  local suite_root=$1 directory=$2 workload=$3 trial=$4 mode=$5 runtime_mode=$6
  python3 - "$suite_root/experiment.json" "$directory/configuration.json" "$workload" "$trial" "$mode" "$runtime_mode" <<'PY'
import json, pathlib, sys
experiment_name, output, workload, trial, mode, runtime = sys.argv[1:]
experiment = json.loads(pathlib.Path(experiment_name).read_text())
pathlib.Path(output).write_text(json.dumps({
    "format_version": 1, "experiment_sha256": experiment["experiment_sha256"],
    "artifact_manifest_sha256": experiment["artifact_manifest_sha256"],
    "workload": workload, "trial": int(trial), "mode": mode,
    "runtime_mode": runtime or None, "ordering_protocol": "serialized-global-lock" if runtime else None,
}, indent=2, sort_keys=True) + "\n")
PY
}

run_attempt() (
  local suite_root=$1 workload=$2 trial=$3 mode=$4 directory=$5
  local runtime_mode server expected_commit profile reference reference_directory
  runtime_mode=
  case "$mode" in
    baseline-original) server=$PRISTINE_TIDB_SERVER; expected_commit=$PRISTINE_TIDB_COMMIT ;;
    serialized-capture-only) server=$INSTRUMENTED_TIDB_SERVER; expected_commit=$INSTRUMENTED_TIDB_COMMIT; runtime_mode=capture-only ;;
    serialized-order-only) server=$INSTRUMENTED_TIDB_SERVER; expected_commit=$INSTRUMENTED_TIDB_COMMIT; runtime_mode=order-only ;;
    serialized-full) server=$INSTRUMENTED_TIDB_SERVER; expected_commit=$INSTRUMENTED_TIDB_COMMIT; runtime_mode=full ;;
    *) die "unknown mode: $mode" ;;
  esac
  profile="$(dirname "$IMMUTABLE_GO_YCSB")/profiles/workload${workload,,}.properties"
  reference_directory="$suite_root/load-references/shared"
  reference="$reference_directory/load-digest.json"
  local warmup_operations=${WORKLOAD_WARMUP[$workload]} measured_operations=${WORKLOAD_OPERATIONS[$workload]}
  local load_seed warmup_seed measured_seed
  load_seed=$LOAD_SEED
  warmup_seed=$WARMUP_SEED
  measured_seed=$MEASURED_SEED

  local TIDB_PID= TELEMETRY_PID= RESOURCE_MONITOR_PID= PIDSTAT_PID=
  local success=false current_phase=initialize
  cleanup_attempt() {
    local status=$?
    set +e
    [[ -z "$PIDSTAT_PID" ]] || { kill -TERM "$PIDSTAT_PID" 2>/dev/null; wait "$PIDSTAT_PID" 2>/dev/null; }
    stop_telemetry
    stop_resource_monitor
    if [[ -n "$TIDB_PID" ]]; then kill -TERM "$TIDB_PID" 2>/dev/null; wait "$TIDB_PID" 2>/dev/null; fi
    clear_online_checker_environment
    if [[ "$success" != true ]]; then
      echo "error: attempt failed in phase $current_phase (status $status): $directory" >&2
      python3 - "$directory/runner-status.json" "$status" "$current_phase" <<'PY'
import json, pathlib, sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({"format_version": 1, "status": "failed", "exit_status": int(sys.argv[2]), "last_phase": sys.argv[3]}, indent=2, sort_keys=True) + "\n")
PY
    fi
  }
  trap cleanup_attempt EXIT INT TERM

  mkdir "$directory"
  configure_store "$directory"
  write_attempt_configuration "$suite_root" "$directory" "$workload" "$trial" "$mode" "$runtime_mode"
  python3 - "$directory/seeds.json" "$load_seed" "$warmup_seed" "$measured_seed" <<'PY'
import json, pathlib, sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({"format_version": 1, "policy": "fixed-v1", "load": int(sys.argv[2]), "warmup": int(sys.argv[3]), "measured": int(sys.argv[4])}, indent=2, sort_keys=True) + "\n")
PY

  current_phase=schema-bootstrap; record_phase "$directory/phases.jsonl" "$current_phase" start
  clear_online_checker_environment
  start_server "$server" "$expected_commit" "$directory" schema
  run_sql ycsb-schema "$directory/schema.json" "$directory/schema-error.txt" -field-count "$FIELD_COUNT" -field-length "$FIELD_LENGTH"
  run_sql assert-ycsb-empty "$directory/empty.json" "$directory/empty-error.txt"
  run_ycsb_digest "$directory/schema-digest.json" "$directory/schema-digest-error.txt" \
    -field-count "$FIELD_COUNT" -field-length "$FIELD_LENGTH" -digest-page-size "$DIGEST_PAGE_SIZE"
  python3 - "$directory/schema-validation.json" <<'PY'
import json, pathlib, sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({"format_version": 1, "passed": True}, indent=2) + "\n")
PY
  stop_server "$directory/schema-server-status.json"
  record_phase "$directory/phases.jsonl" "$current_phase" complete 0

  clear_online_checker_environment
  if [[ -n "$runtime_mode" ]]; then
    export TIDB_ONLINE_CHECKER_MODE=$runtime_mode
    export TIDB_ONLINE_CHECKER_ORDERING_PROTOCOL=serialized-global-lock
    export TIDB_ONLINE_CHECKER_LIBRARY=$NATIVE_LIBRARY
    export TIDB_ONLINE_CHECKER_RESULT_PATH=$directory/checker-result.json
    export TIDB_ONLINE_CHECKER_PRODUCER_ID=1
    export TIDB_ONLINE_CHECKER_REQUIRED_PRODUCERS=1
    export TIDB_ONLINE_CHECKER_CLUSTER_EPOCH="$(date +%s%N)"
    export TIDB_ONLINE_CHECKER_QUEUE_BYTES=$QUEUE_CAPACITY_BYTES
    export TIDB_ONLINE_CHECKER_CAPTURE_BYTES=$CAPTURE_CAPACITY_BYTES
    export TIDB_ONLINE_CHECKER_RETAINED_STATE_BYTES=$RETAINED_STATE_CAPACITY_BYTES
    export TIDB_ONLINE_CHECKER_QUEUE_TIMEOUT=$QUEUE_TIMEOUT
    export TIDB_ONLINE_CHECKER_CAPTURE_TIMEOUT=$CAPTURE_TIMEOUT
    export TIDB_ONLINE_CHECKER_CHUNK_SEGMENT_BYTES=$CHUNK_SEGMENT_BYTES
    export TIDB_ONLINE_CHECKER_MAX_CACHED_CHUNK_SEGMENTS=$MAX_CACHED_CHUNK_SEGMENTS
  fi
  python3 - "$directory/checker-environment.json" <<'PY'
import json, os, pathlib, sys
names = [name for name in os.environ if name.startswith("TIDB_ONLINE_CHECKER_")]
pathlib.Path(sys.argv[1]).write_text(json.dumps({name: os.environ[name] for name in sorted(names)}, indent=2, sort_keys=True) + "\n")
PY
  current_phase=start-main-server; record_phase "$directory/phases.jsonl" "$current_phase" start
  start_server "$server" "$expected_commit" "$directory" main
  record_phase "$directory/phases.jsonl" "$current_phase" complete 0

  current_phase=load; record_phase "$directory/phases.jsonl" "$current_phase" start
  run_ycsb load "$workload" "$profile" "$RECORD_COUNT" "$RECORD_COUNT" 1 "$load_seed" \
    "$directory/load-result.json" "$directory/load.txt" "$directory/load-time.txt"
  validate_ycsb_result load "$workload" "$RECORD_COUNT" "$directory/load-result.json" "$directory/load-client-validation.json"
  run_ycsb_digest "$directory/load-digest.json" "$directory/load-digest-error.txt" \
    -field-count "$FIELD_COUNT" -field-length "$FIELD_LENGTH" -digest-page-size "$DIGEST_PAGE_SIZE"
  digest_matches_reference "$directory/load-digest.json" "$reference" "$directory/load-validation.json"
  wait_for_drain "$runtime_mode" "$directory/load-drain.json" "$directory/load-drain.prom"
  record_phase "$directory/phases.jsonl" "$current_phase" complete 0

  current_phase=capability-gate; record_phase "$directory/phases.jsonl" "$current_phase" start
  run_sql ycsb-gate "$directory/capability-gate.json" "$directory/capability-gate-error.txt" \
    -field-count "$FIELD_COUNT" -field-length "$FIELD_LENGTH"
  python3 - "$directory/gate-validation.json" <<'PY'
import json, pathlib, sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({"format_version": 1, "passed": True}, indent=2) + "\n")
PY
  wait_for_drain "$runtime_mode" "$directory/gate-drain.json" "$directory/gate-drain.prom"
  record_phase "$directory/phases.jsonl" "$current_phase" complete 0

  current_phase=stabilize; record_phase "$directory/phases.jsonl" "$current_phase" start
  sleep "$STABILIZE_SECONDS"
  record_phase "$directory/phases.jsonl" "$current_phase" complete 0

  current_phase=warmup; record_phase "$directory/phases.jsonl" "$current_phase" start
  run_ycsb run "$workload" "$profile" "$RECORD_COUNT" "$warmup_operations" "$THREADS" "$warmup_seed" \
    "$directory/warmup-result.json" "$directory/warmup.txt" "$directory/warmup-time.txt"
  validate_ycsb_result run "$workload" "$warmup_operations" "$directory/warmup-result.json" "$directory/warmup-validation.json"
  wait_for_drain "$runtime_mode" "$directory/warmup-drain.json" "$directory/warmup-drain.prom"
  record_phase "$directory/phases.jsonl" "$current_phase" complete 0

  local warmup_inserts current_record_count
  warmup_inserts=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("measurements",{}).get("INSERT",{}).get("count",0))' "$directory/warmup-result.json")
  current_record_count=$((RECORD_COUNT + warmup_inserts))

  current_phase=workload; record_phase "$directory/phases.jsonl" "$current_phase" start
  start_resource_monitor "$TIDB_PID" "$directory/tidb-resource-samples.txt" "$directory/resource-monitor-summary.json"
  pidstat -h -u -r -d -w -p "$TIDB_PID" 1 >"$directory/tidb-pidstat.txt" 2>&1 & PIDSTAT_PID=$!
  start_telemetry "$runtime_mode" "$directory/workload-checker-samples.jsonl" "$directory/workload-checker-monitor-ready.json"
  local workload_start_ns client_exit_ns
  capture_metrics "$runtime_mode" "$directory/workload-start.json"
  cat "$directory/workload-start.json" >>"$directory/workload-checker-samples.jsonl"
  workload_start_ns=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["monotonic_ns"])' "$directory/workload-start.json")
  run_ycsb run "$workload" "$profile" "$current_record_count" "$measured_operations" "$THREADS" "$measured_seed" \
    "$directory/workload-result.json" "$directory/workload.txt" "$directory/workload-time.txt"
  client_exit_ns=$(monotonic_ns)
  capture_metrics "$runtime_mode" "$directory/workload-cutoff.json"
  python3 - "$directory/workload-cutoff.json" "$client_exit_ns" <<'PY'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1]); value = json.loads(path.read_text())
value["workload_client_exit_monotonic_ns"] = int(sys.argv[2])
path.write_text(json.dumps(value, sort_keys=True) + "\n")
PY
  cat "$directory/workload-cutoff.json" >>"$directory/workload-checker-samples.jsonl"
  validate_ycsb_result run "$workload" "$measured_operations" "$directory/workload-result.json" "$directory/workload-validation.json"
  current_phase=post-workload-drain; record_phase "$directory/phases.jsonl" "$current_phase" start
  wait_for_drain "$runtime_mode" "$directory/workload-drain.json" "$directory/workload-drain.prom" "$directory/workload-cutoff.json"
  stop_telemetry
  stop_resource_monitor
  summarize_resource_interval "$directory/tidb-resource-samples.txt" "$workload_start_ns" "$client_exit_ns" \
    "$directory/resource-monitor-summary.json" "$directory/resource-summary.json"
  kill -TERM "$PIDSTAT_PID" 2>/dev/null || true; wait "$PIDSTAT_PID" 2>/dev/null || true; PIDSTAT_PID=
  validate_telemetry "$runtime_mode" "$directory/workload-checker-samples.jsonl" "$workload_start_ns" "$client_exit_ns" \
    "$directory/workload-cutoff.json" "$directory/telemetry-validation.json"
  parse_time_resource "$directory/workload-time.txt" "$directory/workload-resource.json"
  local drain_complete_ns
  drain_complete_ns=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["drain_complete_monotonic_ns"])' "$directory/workload-drain.json")
  record_phase "$directory/phases.jsonl" "$current_phase" complete 0

  current_phase=checker-close; record_phase "$directory/phases.jsonl" "$current_phase" start
  stop_server "$directory/main-server-status.json"
  if [[ -n "$runtime_mode" ]]; then
    validate_checker_result "$runtime_mode" "$directory/checker-result.json" "$directory/checker-result-validation.json"
  else
    [[ ! -e "$directory/checker-result.json" ]] || die "baseline unexpectedly created a checker result"
    printf '%s\n' '{"format_version":1,"passed":true,"checker":"disabled"}' >"$directory/checker-result-validation.json"
  fi
  python3 - "$directory/timing.json" "$workload_start_ns" "$client_exit_ns" "$drain_complete_ns" <<'PY'
import json, pathlib, sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({
    "clock": "CLOCK_MONOTONIC", "workload_start_monotonic_ns": int(sys.argv[2]),
    "workload_client_exit_monotonic_ns": int(sys.argv[3]), "checker_drain_complete_monotonic_ns": int(sys.argv[4]),
}, indent=2, sort_keys=True) + "\n")
PY
  record_phase "$directory/phases.jsonl" "$current_phase" complete 0

  current_phase=post-check; record_phase "$directory/phases.jsonl" "$current_phase" start
  clear_online_checker_environment
  start_server "$server" "$expected_commit" "$directory" post-check
  run_ycsb_digest "$directory/post-check-digest.json" "$directory/post-check-digest-error.txt" \
    -field-count "$FIELD_COUNT" -field-length "$FIELD_LENGTH" -digest-page-size "$DIGEST_PAGE_SIZE"
  validate_post_check "$directory/load-digest.json" "$directory/post-check-digest.json" \
    "$directory/warmup-result.json" "$directory/workload-result.json" "$directory/post-check-validation.json"
  stop_server "$directory/post-check-server-status.json"
  validate_server_logs "$directory" "$directory/server-log-validation.json"
  record_phase "$directory/phases.jsonl" "$current_phase" complete 0

  lscpu >"$directory/lscpu.txt"; uname -a >"$directory/uname.txt"; df -B1 "$directory" >"$directory/filesystem.txt"
  python3 - "$directory/runner-status.json" "$directory/completed.json" <<'PY'
import datetime, json, pathlib, sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({"format_version": 1, "status": "complete", "exit_status": 0, "last_phase": "complete"}, indent=2, sort_keys=True) + "\n")
pathlib.Path(sys.argv[2]).write_text(json.dumps({"format_version": 1, "status": "complete", "completed_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}, indent=2, sort_keys=True) + "\n")
PY
  attempt_is_valid "$suite_root" "$workload" "$trial" "$mode" "$directory" || die "completed attempt failed analyzer validation: $directory"
  success=true
)

refresh_analysis() {
  local suite_root=$1
  set +e
  "$IMMUTABLE_ANALYZER" "$suite_root"
  local status=$?
  set -e
  return "$status"
}

run_suite() {
  local suite_root=$1 suite_trials=$2
  write_capacity_preflight "$suite_root"
  local workload trial mode latest attempt reference_workload=${WORKLOADS[0]}
  echo "Creating/verifying one pristine seeded load reference for all workloads"
  ensure_load_reference "$suite_root" "$reference_workload" "$RECORD_COUNT" "$suite_root/load-references/shared"
  if [[ "$FINAL_RESULTS_ONLY" = true ]]; then
    delete_evidence_tree "$suite_root/load-references/shared/store" "$suite_root"
  fi
  for workload in "${WORKLOADS[@]}"; do
    for trial in $(seq 1 "$suite_trials"); do
      mapfile -t MODE_ORDER < <(python3 - "$suite_root/experiment.json" "$trial" <<'PY'
import json, pathlib, sys
value = json.loads(pathlib.Path(sys.argv[1]).read_text())
print("\n".join(value["mode_rotation"][sys.argv[2]]))
PY
      )
      for mode in "${MODE_ORDER[@]}"; do
        latest=
        latest=$(newest_attempt "$suite_root" "$workload" "$trial" "$mode" 2>/dev/null || true)
        if [[ -n "$latest" && "$RESUME" = true ]] && attempt_is_valid "$suite_root" "$workload" "$trial" "$mode" "$latest"; then
          echo "Skipping analyzer-valid workload $workload trial $trial mode $mode: $latest"
          continue
        fi
        attempt=$(next_attempt_directory "$suite_root" "$workload" "$trial" "$mode")
        echo "Running workload $workload trial $trial mode $mode: $attempt"
        run_attempt "$suite_root" "$workload" "$trial" "$mode" "$attempt"
        attempt_is_valid "$suite_root" "$workload" "$trial" "$mode" "$attempt" ||
          die "completed attempt failed analyzer validation before retention pruning: $attempt"
        prune_successful_attempt_store "$suite_root" "$attempt"
        refresh_analysis "$suite_root" >/dev/null 2>&1 || true
      done
    done
  done
  refresh_analysis "$suite_root"
  python3 - "$suite_root/ycsb-summary.json" "$suite_root/valid.json" <<'PY'
import datetime, json, pathlib, sys
summary = json.loads(pathlib.Path(sys.argv[1]).read_text())
if summary.get("invalid_rows") != 0: raise SystemExit("suite has invalid rows")
pathlib.Path(sys.argv[2]).write_text(json.dumps({
    "format_version": 1, "valid": True, "valid_rows": summary.get("valid_rows"),
    "completed_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
}, indent=2, sort_keys=True) + "\n")
PY
  prune_successful_suite "$suite_root"
}

if [[ "$SMOKE" = true ]]; then
  write_or_verify_experiment "$RESULT_ROOT" true 1 "$RECORD_COUNT" "$THREADS" \
    "$WARMUP_OPERATIONS" "$OPERATIONS" ""
  run_suite "$RESULT_ROOT" 1
  prune_build_artifacts "$RESULT_ROOT"
  echo "YCSB smoke completed: $RESULT_ROOT"
  exit 0
fi

# A publishable measured run always gates on the exact immutable artifacts in
# a fresh A-F/four-mode smoke. Preserve measured globals while the smoke runs.
MEASURED_RECORD_COUNT=$RECORD_COUNT
MEASURED_THREADS=$THREADS
MEASURED_STABILIZE_SECONDS=$STABILIZE_SECONDS
declare -A MEASURED_WARMUP=()
declare -A MEASURED_OPERATIONS=()
for workload in "${WORKLOADS[@]}"; do
  MEASURED_WARMUP[$workload]=${WORKLOAD_WARMUP[$workload]}
  MEASURED_OPERATIONS[$workload]=${WORKLOAD_OPERATIONS[$workload]}
done
MEASURED_WORKLOAD_TEXT=$WORKLOAD_TEXT
MEASURED_TRIALS=$TRIALS

SMOKE=true
TRIALS=1
if [[ "$DIAGNOSTIC_ONLY" = true ]]; then
  WORKLOAD_TEXT=$MEASURED_WORKLOAD_TEXT
  IFS=',' read -r -a WORKLOADS <<<"$WORKLOAD_TEXT"
else
  WORKLOAD_TEXT=A,B,C,D,E,F
  WORKLOADS=(A B C D E F)
fi
RECORD_COUNT=10000
THREADS=4
STABILIZE_SECONDS=2
for workload in "${WORKLOADS[@]}"; do
  WORKLOAD_WARMUP[$workload]=2000
  WORKLOAD_OPERATIONS[$workload]=10000
done
write_or_verify_experiment "$RESULT_ROOT/smoke" true 1 "$RECORD_COUNT" "$THREADS" 2000 10000 ""
run_suite "$RESULT_ROOT/smoke" 1

SMOKE=false
TRIALS=$MEASURED_TRIALS
WORKLOAD_TEXT=$MEASURED_WORKLOAD_TEXT
IFS=',' read -r -a WORKLOADS <<<"$WORKLOAD_TEXT"
RECORD_COUNT=$MEASURED_RECORD_COUNT
THREADS=$MEASURED_THREADS
STABILIZE_SECONDS=$MEASURED_STABILIZE_SECONDS
for workload in "${WORKLOADS[@]}"; do
  WORKLOAD_WARMUP[$workload]=${MEASURED_WARMUP[$workload]}
  WORKLOAD_OPERATIONS[$workload]=${MEASURED_OPERATIONS[$workload]}
done

if [[ -e "$RESULT_ROOT/calibration.json" ]]; then
  [[ "$(sha256_file "$RESULT_ROOT/calibration.json")" = "$CALIBRATION_SHA" ]] || die "frozen calibration copy changed"
else
  cp "$CALIBRATION" "$RESULT_ROOT/calibration.json"
  chmod 0444 "$RESULT_ROOT/calibration.json"
fi
write_or_verify_experiment "$RESULT_ROOT" false "$TRIALS" "$RECORD_COUNT" "$THREADS" 0 0 "$RESULT_ROOT/calibration.json"
run_suite "$RESULT_ROOT" "$TRIALS"
prune_build_artifacts "$RESULT_ROOT"
echo "YCSB measured comparison completed: $RESULT_ROOT"
