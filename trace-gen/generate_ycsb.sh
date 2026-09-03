#!/usr/bin/env bash
set -euo pipefail

usage() {
	printf '%s\n' \
		'usage: ./trace-gen/generate_ycsb.sh' \
		'' \
		'Generate an offline-checker trace with instrumented TiDB and patched' \
		'PingCAP go-ycsb. Configuration is supplied through environment variables.' \
		'' \
		'Primary experiment knobs:' \
		'  THETA=0.99       Zipfian key-distribution theta (0 < theta < 1)' \
		'  OPS_PER_TXN=1    logical YCSB operations in every measured transaction' \
		'  SESSIONS=1       measured go-ycsb workers / pinned TiDB connections' \
		'' \
		'Trace-size knobs:' \
		'  RECORD_COUNT=1000  initial rows (their load transactions stay in the trace)' \
		'  TXN_COUNT=1000     measured transactions across all sessions' \
		'  LOAD_THREADS=1     go-ycsb workers used only for the initial load' \
		'  LOAD_BATCH_SIZE=100 rows inserted by each load statement' \
		'  BINARY_ONLY=0       stream raw YAML directly to the final .trace file' \
		'  FAIL_ON_RUN_ERRORS=0 fail instead of filtering conflict/deadlock groups' \
		'  RETRY_CONFLICTED_TXNS=1 retry 9007/1213/8022 until TXN_COUNT commits succeed' \
		'' \
		'Common overrides:' \
		'  TIDB_REPO=../tidb-tracegen  WORKLOAD_FILE=trace-gen/ycsb_workloada.properties' \
		'  GO_YCSB_BIN=/path/to/patched/go-ycsb  TIDB_SERVER_BIN=/path/to/tidb-server' \
		'  OUTPUT_DIR=traces/ycsb  BIN_TRACE_DIR=bin_traces' \
		'  HOST=127.0.0.1  PORT=4020  STATUS_PORT=10200  DB_NAME=test' \
		'  YCSB_DATABASE=tidb  TXN_MODE=optimistic  MYSQL_PARAMS=tidb_txn_mode=optimistic'
}

if (( $# > 0 )); then
	case $1 in
	-h | --help)
		usage
		exit 0
		;;
	*)
		printf 'unknown argument: %s\n' "$1" >&2
		usage >&2
		exit 2
		;;
	esac
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "${script_dir}/.." && pwd)

record_count=${RECORD_COUNT:-1000}
txn_count=${TXN_COUNT:-1000}
ops_per_txn=${OPS_PER_TXN:-1}
sessions=${SESSIONS:-1}
theta=${THETA:-0.99}
load_threads=${LOAD_THREADS:-1}
load_batch_size=${LOAD_BATCH_SIZE:-100}
target=${TARGET:-0}
binary_only=${BINARY_ONLY:-0}
fail_on_run_errors=${FAIL_ON_RUN_ERRORS:-0}
retry_conflicted_txns=${RETRY_CONFLICTED_TXNS:-1}

host=${HOST:-127.0.0.1}
port=${PORT:-4020}
status_port=${STATUS_PORT:-10200}
db_name=${DB_NAME:-test}
table_name=${TABLE_NAME:-usertable}
db_user=${DB_USER:-root}
db_password=${DB_PASSWORD:-}
ycsb_database=${YCSB_DATABASE:-tidb}
txn_mode=${TXN_MODE:-optimistic}
mysql_params=${MYSQL_PARAMS-"tidb_txn_mode=${txn_mode}"}
tidb_cluster_index=${TIDB_CLUSTER_INDEX:-true}

tidb_repo=${TIDB_REPO:-"${repo_root}/../tidb-tracegen"}
output_dir=${OUTPUT_DIR:-"${repo_root}/traces/ycsb"}
bin_trace_dir=${BIN_TRACE_DIR:-"${repo_root}/bin_traces"}
raw_dir="${output_dir}/raw"
workload_file=${WORKLOAD_FILE:-"${script_dir}/ycsb_workloada.properties"}

go_ycsb_url=${GO_YCSB_URL:-https://github.com/pingcap/go-ycsb.git}
go_ycsb_ref=${GO_YCSB_REF:-v1.0.3}
go_ycsb_commit=${GO_YCSB_COMMIT-f030f9942393a8febbf4365c2d582711723159f5}
go_ycsb_source=${GO_YCSB_SOURCE_DIR:-"${repo_root}/build/go-ycsb-v1.0.3"}
go_ycsb_patch="${script_dir}/go_ycsb_v1.0.3_grouped_txn.patch"
go_ycsb=${GO_YCSB_BIN:-"${go_ycsb_source}/bin/go-ycsb"}
if [[ "${go_ycsb_source}" != /* ]]; then
	go_ycsb_source="${PWD}/${go_ycsb_source}"
fi
if [[ "${go_ycsb}" != /* ]]; then
	go_ycsb="${PWD}/${go_ycsb}"
fi

require_positive_integer() {
	local name=$1
	local value=$2
	if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
		printf '%s must be a positive integer without leading zeros: %s\n' "${name}" "${value}" >&2
		exit 2
	fi
}

require_nonnegative_integer() {
	local name=$1
	local value=$2
	if [[ ! "${value}" =~ ^(0|[1-9][0-9]*)$ ]]; then
		printf '%s must be a nonnegative integer without leading zeros: %s\n' "${name}" "${value}" >&2
		exit 2
	fi
}

require_positive_integer RECORD_COUNT "${record_count}"
require_positive_integer TXN_COUNT "${txn_count}"
require_positive_integer OPS_PER_TXN "${ops_per_txn}"
require_positive_integer SESSIONS "${sessions}"
require_positive_integer LOAD_THREADS "${load_threads}"
require_positive_integer LOAD_BATCH_SIZE "${load_batch_size}"
require_positive_integer PORT "${port}"
require_positive_integer STATUS_PORT "${status_port}"
require_nonnegative_integer TARGET "${target}"
if (( port > 65535 || status_port > 65535 )); then
	printf 'PORT and STATUS_PORT must not exceed 65535: %s, %s\n' "${port}" "${status_port}" >&2
	exit 2
fi
if [[ "${fail_on_run_errors}" != "0" && "${fail_on_run_errors}" != "1" ]]; then
	printf 'FAIL_ON_RUN_ERRORS must be 0 or 1: %s\n' "${fail_on_run_errors}" >&2
	exit 2
fi
if [[ "${retry_conflicted_txns}" != "0" && "${retry_conflicted_txns}" != "1" ]]; then
	printf 'RETRY_CONFLICTED_TXNS must be 0 or 1: %s\n' "${retry_conflicted_txns}" >&2
	exit 2
fi
retry_conflicted_property=false
if [[ "${retry_conflicted_txns}" == "1" ]]; then
	retry_conflicted_property=true
fi
if [[ "${binary_only}" != "0" && "${binary_only}" != "1" ]]; then
	printf 'BINARY_ONLY must be 0 or 1: %s\n' "${binary_only}" >&2
	exit 2
fi

if ! command -v python3 >/dev/null 2>&1; then
	printf 'python3 is not available on PATH\n' >&2
	exit 1
fi

theta_input=${theta}
if ! theta=$(python3 - "${theta_input}" <<'PY'
import math
import sys

try:
    theta = float(sys.argv[1])
except ValueError:
    raise SystemExit(1)
if not math.isfinite(theta) or not 0.0 < theta < 1.0:
    raise SystemExit(1)
print(repr(theta))
PY
); then
	printf 'THETA must be a finite number strictly between 0 and 1: %s\n' "${theta_input}" >&2
	exit 2
fi

if (( txn_count < sessions )); then
	printf 'TXN_COUNT (%s) must be at least SESSIONS (%s) so every requested session attempts work\n' \
		"${txn_count}" "${sessions}" >&2
	exit 2
fi

load_group_size=$((load_threads * load_batch_size))
if (( record_count % load_group_size != 0 )); then
	printf 'RECORD_COUNT (%s) must be divisible by LOAD_THREADS * LOAD_BATCH_SIZE (%s)\n' \
		"${record_count}" "${load_group_size}" >&2
	exit 2
fi

logical_operation_count=$((txn_count * ops_per_txn))

if [[ ! -d "${tidb_repo}/cmd/tidb-server" ]]; then
	printf 'instrumented TiDB repo not found at %s\n' "${tidb_repo}" >&2
	printf 'Set TIDB_REPO to the tidb-tracegen checkout.\n' >&2
	exit 1
fi
tidb_repo=$(cd -- "${tidb_repo}" && pwd)

if [[ ! -f "${workload_file}" ]]; then
	printf 'go-ycsb workload properties not found at %s\n' "${workload_file}" >&2
	exit 1
fi
workload_file=$(cd -- "$(dirname -- "${workload_file}")" && pwd)/$(basename -- "${workload_file}")

if command -v go >/dev/null 2>&1; then
	go_cmd=$(command -v go)
elif [[ -x /usr/local/go/bin/go ]]; then
	go_cmd=/usr/local/go/bin/go
else
	printf 'go is not available on PATH or at /usr/local/go/bin/go\n' >&2
	exit 1
fi

if [[ "${binary_only}" == "1" ]]; then
	raw_trace_to_binary=${RAW_TRACE_TO_BINARY:-"${script_dir}/raw_trace_to_binary.py"}
	if [[ ! -x "${raw_trace_to_binary}" ]]; then
		printf 'bounded-memory raw trace converter is not executable: %s\n' "${raw_trace_to_binary}" >&2
		exit 1
	fi
	if [[ "${raw_trace_to_binary}" != /* ]]; then
		raw_trace_to_binary="${PWD}/${raw_trace_to_binary}"
	fi
else
	trace_preprocess=${TRACE_PREPROCESS:-"${repo_root}/build/trace-preprocess"}
	if [[ ! -x "${trace_preprocess}" ]]; then
		if [[ -z "${TRACE_PREPROCESS:-}" && ( -f "${repo_root}/build/build.ninja" || -f "${repo_root}/build/Makefile" ) ]]; then
			cmake --build "${repo_root}/build" --target trace-preprocess
		fi
	fi
	if [[ ! -x "${trace_preprocess}" ]]; then
		printf 'trace-preprocess executable not found at %s\n' "${trace_preprocess}" >&2
		printf 'Configure co-checker first, then build it with: cmake --build build --target trace-preprocess\n' >&2
		exit 1
	fi

	trace_convert=${TRACE_CONVERT:-"${repo_root}/build/trace-convert"}
	if [[ ! -x "${trace_convert}" ]]; then
		if [[ -z "${TRACE_CONVERT:-}" && ( -f "${repo_root}/build/build.ninja" || -f "${repo_root}/build/Makefile" ) ]]; then
			cmake --build "${repo_root}/build" --target trace-convert
		fi
	fi
	if [[ ! -x "${trace_convert}" ]]; then
		printf 'trace-convert executable not found at %s\n' "${trace_convert}" >&2
		printf 'Configure co-checker first, then build it with: cmake --build build --target trace-convert\n' >&2
		exit 1
	fi
fi

binary_path_for_trace() {
	local trace_path=$1
	local relative_path
	if [[ "${trace_path}" == "${repo_root}/traces/"* ]]; then
		relative_path=${trace_path#"${repo_root}/traces/"}
	elif [[ "${trace_path}" == "${output_dir}/"* ]]; then
		relative_path=${trace_path#"${output_dir}/"}
	else
		relative_path=$(basename -- "${trace_path}")
	fi
	printf '%s/%s.trace\n' "${bin_trace_dir}" "${relative_path%.yaml}"
}

if [[ -n "${GO_YCSB_BIN:-}" && ! -x "${go_ycsb}" ]]; then
	printf 'GO_YCSB_BIN is not executable: %s\n' "${go_ycsb}" >&2
	exit 1
fi

go_ycsb_needs_build=0
if [[ -z "${GO_YCSB_BIN:-}" ]]; then
	if [[ ! -f "${go_ycsb_patch}" ]]; then
		printf 'go-ycsb grouped-transaction patch not found at %s\n' "${go_ycsb_patch}" >&2
		exit 1
	fi
	go_ycsb_patch_hash=$(python3 - "${go_ycsb_patch}" <<'PY'
import hashlib
import sys

with open(sys.argv[1], "rb") as patch_file:
    print(hashlib.sha256(patch_file.read()).hexdigest())
PY
)
	go_ycsb_stamp="${go_ycsb}.patch.sha256"
	if [[ ! -x "${go_ycsb}" || ! -f "${go_ycsb_stamp}" || "$(<"${go_ycsb_stamp}")" != "${go_ycsb_patch_hash}" ]]; then
		go_ycsb_needs_build=1
	fi
fi

if (( go_ycsb_needs_build == 1 )); then
	if ! command -v git >/dev/null 2>&1; then
		printf 'git is required to fetch pinned go-ycsb\n' >&2
		exit 1
	fi
	if [[ ! -d "${go_ycsb_source}/.git" ]]; then
		if [[ -e "${go_ycsb_source}" ]]; then
			printf 'GO_YCSB_SOURCE_DIR exists but is not a git checkout: %s\n' "${go_ycsb_source}" >&2
			exit 1
		fi
		mkdir -p "$(dirname -- "${go_ycsb_source}")"
		printf 'Cloning PingCAP go-ycsb %s\n' "${go_ycsb_ref}"
		git clone --depth 1 --branch "${go_ycsb_ref}" "${go_ycsb_url}" "${go_ycsb_source}"
	fi
	actual_commit=$(git -C "${go_ycsb_source}" rev-parse HEAD)
	if [[ -n "${go_ycsb_commit}" && "${actual_commit}" != "${go_ycsb_commit}" ]]; then
		printf 'go-ycsb checkout is %s, expected %s\n' "${actual_commit}" "${go_ycsb_commit}" >&2
		printf 'Set GO_YCSB_COMMIT to the intended pinned commit, or use a fresh GO_YCSB_SOURCE_DIR.\n' >&2
		exit 1
	fi
	if git -C "${go_ycsb_source}" apply --check "${go_ycsb_patch}" >/dev/null 2>&1; then
		git -C "${go_ycsb_source}" apply "${go_ycsb_patch}"
	elif ! git -C "${go_ycsb_source}" apply --reverse --check "${go_ycsb_patch}" >/dev/null 2>&1; then
		printf 'go-ycsb patch is neither cleanly applicable nor already applied in %s\n' "${go_ycsb_source}" >&2
		exit 1
	fi
	mkdir -p "$(dirname -- "${go_ycsb}")"
	printf 'Building patched PingCAP go-ycsb: %s\n' "${go_ycsb}"
	(
		cd "${go_ycsb_source}"
		"${go_cmd}" build -o "${go_ycsb}.tmp" ./cmd/go-ycsb
	)
	mv -f -- "${go_ycsb}.tmp" "${go_ycsb}"
	printf '%s\n' "${go_ycsb_patch_hash}" >"${go_ycsb_stamp}"
fi

tidb_server=${TIDB_SERVER_BIN:-"${repo_root}/build/tidb-server-trace"}
if [[ "${tidb_server}" != /* ]]; then
	tidb_server="${PWD}/${tidb_server}"
fi
if [[ ! -x "${tidb_server}" ]]; then
	if [[ -n "${TIDB_SERVER_BIN:-}" ]]; then
		printf 'TIDB_SERVER_BIN is not executable: %s\n' "${tidb_server}" >&2
		exit 1
	fi
	mkdir -p "$(dirname -- "${tidb_server}")"
	printf 'Building instrumented TiDB server: %s\n' "${tidb_server}"
	(
		cd "${tidb_repo}"
		"${go_cmd}" build -o "${tidb_server}" ./cmd/tidb-server
	)
fi

workload_tag=$(basename -- "${workload_file}")
workload_tag=${workload_tag%.*}
suffix="ycsb-${workload_tag}-${record_count}records-${txn_count}txns-${sessions}sessions-${ops_per_txn}ops-theta-${theta}-${txn_mode}"
if [[ "${retry_conflicted_txns}" == "1" ]]; then
	suffix="${suffix}-retry-conflicts"
fi
suffix=${suffix//[^A-Za-z0-9_.-]/_}
raw_path="${raw_dir}/${suffix}.raw.yaml"
processed_seed="${output_dir}/${suffix}.yaml"
tidb_log_path="${raw_dir}/${suffix}.tidb.log"
load_log_path="${raw_dir}/${suffix}.load.log"
run_log_path="${raw_dir}/${suffix}.run.log"

store_is_temporary=0
if [[ -n "${STORE_DIR:-}" ]]; then
	store_dir=${STORE_DIR}
else
	store_dir=$(mktemp -d /tmp/co-checker-ycsb-store.XXXXXX)
	store_is_temporary=1
fi
keep_store=${KEEP_STORE:-0}

mkdir -p "${raw_dir}" "${output_dir}"
rm -f "${raw_path}" "${processed_seed}" "${processed_seed}.tmp" \
	"${tidb_log_path}" "${load_log_path}" "${run_log_path}"

tidb_pid=""
cleanup() {
	if [[ -n "${tidb_pid}" ]] && kill -0 "${tidb_pid}" >/dev/null 2>&1; then
		kill "${tidb_pid}" >/dev/null 2>&1 || true
		wait "${tidb_pid}" >/dev/null 2>&1 || true
	fi
	tidb_pid=""
	if (( store_is_temporary == 1 )) && [[ "${keep_store}" != "1" ]]; then
		rm -rf -- "${store_dir}"
	fi
}
trap cleanup EXIT INT TERM

printf 'Starting instrumented TiDB on %s:%s\n' "${host}" "${port}"
(
	export TIDB_TXN_TRACE_ENABLE=1
	export TIDB_TXN_TRACE_PATH="${raw_path}"
	exec "${tidb_server}" \
		-store=unistore \
		-path="${store_dir}" \
		-P "${port}" \
		-status "${status_port}" \
		-host "${host}" \
		-status-host "${host}" \
		-L error
) >"${tidb_log_path}" 2>&1 &
tidb_pid=$!

python3 - "${host}" "${port}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
deadline = time.monotonic() + 120
while time.monotonic() < deadline:
    sock = socket.socket()
    sock.settimeout(0.5)
    try:
        sock.connect((host, port))
    except OSError:
        time.sleep(1)
    else:
        sock.close()
        sys.exit(0)
    finally:
        sock.close()
print(f"timed out waiting for TiDB at {host}:{port}", file=sys.stderr)
sys.exit(1)
PY

common_ycsb_args=(
	-P "${workload_file}"
	-p "mysql.host=${host}"
	-p "mysql.port=${port}"
	-p "mysql.user=${db_user}"
	-p "mysql.password=${db_password}"
	-p "mysql.db=${db_name}"
	-p "mysql.params=${mysql_params}"
	-p "tidb.cluster_index=${tidb_cluster_index}"
	-p "table=${table_name}"
	-p "recordcount=${record_count}"
	-p 'insertstart=0'
	-p "insertcount=${record_count}"
	-p 'warmuptime=0'
	-p 'requestdistribution=zipfian'
	-p "zipfianconstant=${theta}"
	-p 'debug.pprof=127.0.0.1:0'
	-p 'silence=false'
)

run_ycsb_stage() {
	local stage=$1
	local client_log=$2
	local fail_on_operation_errors=$3
	shift 3
	if ! "$@" 2>&1 | tee "${client_log}"; then
		printf 'go-ycsb %s failed; see %s\n' "${stage}" "${client_log}" >&2
		exit 1
	fi
	local operation_error_count
	operation_error_count=$(grep -Fc 'operation err:' "${client_log}" || true)
	if (( operation_error_count > 0 )); then
		local expected_abort_count
		expected_abort_count=$(grep -Ec '^operation err: Error (9007|1213|8022):' "${client_log}" || true)
		local unexpected_error_count=$((operation_error_count - expected_abort_count))
		if (( unexpected_error_count > 0 )); then
			printf 'go-ycsb %s reported %s unexpected operation error(s); see %s\n' \
				"${stage}" "${unexpected_error_count}" "${client_log}" >&2
			exit 1
		fi
		if [[ "${fail_on_operation_errors}" == "1" ]]; then
			printf 'go-ycsb %s reported %s retryable conflict/deadlock abort(s); see %s\n' \
				"${stage}" "${expected_abort_count}" "${client_log}" >&2
			exit 1
		fi
		printf 'Warning: go-ycsb %s reported %s retryable conflict/deadlock abort(s); preprocessing will remove them.\n' \
			"${stage}" "${expected_abort_count}" >&2
	fi
	local retried_conflict_count
	retried_conflict_count=$(awk '/^retryable transaction conflicts: [0-9]+$/ { total += $4 } END { print total + 0 }' "${client_log}")
	if (( retried_conflict_count > 0 )); then
		printf 'go-ycsb %s retried %s conflict/deadlock abort(s) before reaching the successful transaction quota.\n' \
			"${stage}" "${retried_conflict_count}"
	fi
}

printf 'Loading YCSB seed state: records=%s threads=%s batch=%s\n' \
	"${record_count}" "${load_threads}" "${load_batch_size}"
run_ycsb_stage load "${load_log_path}" 1 \
	"${go_ycsb}" load "${ycsb_database}" "${common_ycsb_args[@]}" \
	-p 'dropdata=true' \
	-p "threadcount=${load_threads}" \
	-p "batch.size=${load_batch_size}" \
	-p 'analyzeafterload=false'

printf 'Running grouped YCSB: txns=%s ops_per_txn=%s sessions=%s theta=%s target_ops_per_sec=%s\n' \
	"${txn_count}" "${ops_per_txn}" "${sessions}" "${theta}" "${target}"
if [[ "${retry_conflicted_txns}" == "1" ]]; then
	printf 'Retrying TiDB conflict/deadlock aborts until each worker reaches its successful transaction quota\n'
fi
run_ycsb_stage run "${run_log_path}" "${fail_on_run_errors}" \
	"${go_ycsb}" run "${ycsb_database}" "${common_ycsb_args[@]}" \
	-p 'dropdata=false' \
	-p 'batch.size=1' \
	-p "threadcount=${sessions}" \
	-p "operationcount=${logical_operation_count}" \
	-p "operationspertransaction=${ops_per_txn}" \
	-p "retryconflictedtransactions=${retry_conflicted_property}" \
	-p "target=${target}"

printf 'Stopping TiDB\n'
cleanup
trap - EXIT INT TERM

if [[ ! -s "${raw_path}" ]]; then
	printf 'instrumented TiDB produced no trace at %s; see %s\n' "${raw_path}" "${tidb_log_path}" >&2
	exit 1
fi

if [[ "${binary_only}" == "1" ]]; then
	binary_seed="${bin_trace_dir}/${suffix}.trace"
	printf 'Converting raw trace directly to binary: %s\n' "${binary_seed}"
	binary_path=$("${raw_trace_to_binary}" --rename -o "${binary_seed}" "${raw_path}")
else
	printf 'Preprocessing raw trace: %s\n' "${raw_path}"
	processed_path=$("${trace_preprocess}" -o "${processed_seed}" --rename "${raw_path}")
	binary_path=$(binary_path_for_trace "${processed_path}")

	printf 'Converting checker trace to binary: %s\n' "${binary_path}"
	"${trace_convert}" -o "${binary_path}" "${processed_path}" >/dev/null
fi

printf 'Raw trace: %s\n' "${raw_path}"
printf 'TiDB log: %s\n' "${tidb_log_path}"
printf 'go-ycsb load log: %s\n' "${load_log_path}"
printf 'go-ycsb run log: %s\n' "${run_log_path}"
if [[ "${binary_only}" != "1" ]]; then
	printf 'Checker trace: %s\n' "${processed_path}"
fi
printf 'Binary trace: %s\n' "${binary_path}"
if (( store_is_temporary == 1 )) && [[ "${keep_store}" == "1" ]]; then
	printf 'TiDB store: %s\n' "${store_dir}"
fi
