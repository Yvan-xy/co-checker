#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "${script_dir}/.." && pwd)
tidb_repo=${TIDB_REPO:-"$(cd -- "${repo_root}/../tidb" && pwd)"}
output_dir=${OUTPUT_DIR:-"${repo_root}/traces/tpcc"}
bin_trace_dir=${BIN_TRACE_DIR:-"${repo_root}/bin_traces"}
raw_dir="${output_dir}/raw"

warehouses=${WAREHOUSES:-1}
threads=${THREADS:-1}
run_time=${RUN_TIME:-5s}
transaction_weights=${TRANSACTION_WEIGHTS:-45,43,4,4,4}
port=${PORT:-4020}
status_port=${STATUS_PORT:-10200}
host=${HOST:-127.0.0.1}
db_name=${DB_NAME:-"tpcc_trace_optimistic_${warehouses}w_${threads}t"}
store_dir=${STORE_DIR:-"$(mktemp -d /tmp/co-checker-tpcc-store.XXXXXX)"}
keep_store=${KEEP_STORE:-0}
conn_params="tidb_txn_mode=optimistic"

suffix="tpcc-optimistic-${warehouses}w-${threads}th-${run_time}"
if [[ "${transaction_weights}" != "45,43,4,4,4" ]]; then
	weight_suffix=${transaction_weights//,/-}
	suffix+="-weights-${weight_suffix}"
fi
suffix=${suffix//[^A-Za-z0-9_.-]/_}
raw_path="${raw_dir}/${suffix}.raw.yaml"
processed_seed="${output_dir}/${suffix}.yaml"
log_path="${raw_dir}/${suffix}.tidb.log"

if [[ ! -d "${tidb_repo}/cmd/tidb-server" ]]; then
	printf 'TiDB repo not found at %s\n' "${tidb_repo}" >&2
	exit 1
fi

if ! command -v go >/dev/null 2>&1; then
	printf 'go is not available on PATH\n' >&2
	exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
	printf 'python3 is not available on PATH\n' >&2
	exit 1
fi

IFS=',' read -r -a weight_values <<<"${transaction_weights}"
if (( ${#weight_values[@]} != 5 )); then
	printf 'TRANSACTION_WEIGHTS must contain five comma-separated integers: %s\n' "${transaction_weights}" >&2
	exit 1
fi
weight_sum=0
for weight in "${weight_values[@]}"; do
	if [[ ! "${weight}" =~ ^[0-9]+$ ]]; then
		printf 'TRANSACTION_WEIGHTS contains a non-negative-integer violation: %s\n' "${transaction_weights}" >&2
		exit 1
	fi
	weight_sum=$((weight_sum + weight))
done
if (( weight_sum != 100 )); then
	printf 'TRANSACTION_WEIGHTS must sum to 100, got %s: %s\n' "${weight_sum}" "${transaction_weights}" >&2
	exit 1
fi

trace_preprocess=${TRACE_PREPROCESS:-"${repo_root}/build/trace-preprocess"}
if [[ ! -x "${trace_preprocess}" ]]; then
	if [[ -f "${repo_root}/build/build.ninja" || -f "${repo_root}/build/Makefile" ]]; then
		cmake --build "${repo_root}/build" --target trace-preprocess
	fi
fi
if [[ ! -x "${trace_preprocess}" ]]; then
	printf 'trace-preprocess executable not found at %s\n' "${trace_preprocess}" >&2
	printf 'Build it with: cmake --build build --target trace-preprocess\n' >&2
	exit 1
fi

trace_convert=${TRACE_CONVERT:-"${repo_root}/build/trace-convert"}
if [[ ! -x "${trace_convert}" ]]; then
	if [[ -f "${repo_root}/build/build.ninja" || -f "${repo_root}/build/Makefile" ]]; then
		cmake --build "${repo_root}/build" --target trace-convert
	fi
fi
if [[ ! -x "${trace_convert}" ]]; then
	printf 'trace-convert executable not found at %s\n' "${trace_convert}" >&2
	printf 'Build it with: cmake --build build --target trace-convert\n' >&2
	exit 1
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

go_tpc=${GO_TPC:-}
if [[ -z "${go_tpc}" ]]; then
	gobin=$(go env GOBIN)
	if [[ -z "${gobin}" ]]; then
		gobin="$(go env GOPATH)/bin"
	fi
	go_tpc="${gobin}/go-tpc"
fi
if [[ ! -x "${go_tpc}" ]]; then
	printf 'go-tpc not found; installing github.com/pingcap/go-tpc/cmd/go-tpc@latest\n' >&2
	go install github.com/pingcap/go-tpc/cmd/go-tpc@latest
fi
if [[ ! -x "${go_tpc}" ]]; then
	printf 'go-tpc executable not found at %s\n' "${go_tpc}" >&2
	exit 1
fi

tidb_server=${TIDB_SERVER_BIN:-"${repo_root}/build/tidb-server-trace"}
if [[ ! -x "${tidb_server}" ]]; then
	mkdir -p "$(dirname -- "${tidb_server}")"
	(
		cd "${tidb_repo}"
		go build -o "${tidb_server}" ./cmd/tidb-server
	)
fi
if [[ ! -x "${tidb_server}" ]]; then
	printf 'tidb-server executable not found at %s\n' "${tidb_server}" >&2
	exit 1
fi

mkdir -p "${raw_dir}" "${output_dir}"
rm -f "${raw_path}" "${processed_seed}" "${processed_seed}.tmp" "${log_path}"

tidb_pid=""
cleanup() {
	if [[ -n "${tidb_pid}" ]] && kill -0 "${tidb_pid}" >/dev/null 2>&1; then
		kill "${tidb_pid}" >/dev/null 2>&1 || true
		wait "${tidb_pid}" >/dev/null 2>&1 || true
	fi
	if [[ "${keep_store}" != "1" ]]; then
		rm -rf "${store_dir}"
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
) >"${log_path}" 2>&1 &
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

printf 'Running go-tpc prepare: warehouses=%s threads=%s db=%s conn_params=%s\n' "${warehouses}" "${threads}" "${db_name}" "${conn_params}"
"${go_tpc}" -H "${host}" -P "${port}" -D "${db_name}" --conn-params "${conn_params}" tpcc --warehouses "${warehouses}" prepare -T "${threads}"

printf 'Running go-tpc workload: warehouses=%s threads=%s time=%s weights=%s db=%s conn_params=%s\n' "${warehouses}" "${threads}" "${run_time}" "${transaction_weights}" "${db_name}" "${conn_params}"
"${go_tpc}" -H "${host}" -P "${port}" -D "${db_name}" --conn-params "${conn_params}" tpcc --warehouses "${warehouses}" run -T "${threads}" --time "${run_time}" --weight "${transaction_weights}" --silence

printf 'Stopping TiDB\n'
cleanup
trap - EXIT INT TERM

printf 'Preprocessing raw trace: %s\n' "${raw_path}"
processed_path=$("${trace_preprocess}" -o "${processed_seed}" --rename "${raw_path}")
binary_path=$(binary_path_for_trace "${processed_path}")

printf 'Converting checker trace to binary: %s\n' "${binary_path}"
"${trace_convert}" -o "${binary_path}" "${processed_path}" >/dev/null

printf 'Raw trace: %s\n' "${raw_path}"
printf 'TiDB log: %s\n' "${log_path}"
printf 'Checker trace: %s\n' "${processed_path}"
printf 'Binary trace: %s\n' "${binary_path}"
