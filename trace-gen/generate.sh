#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "${script_dir}/.." && pwd)
tidb_repo=${TIDB_REPO:-"$(cd -- "${repo_root}/../tidb" && pwd)"}
output_dir=${1:-"${repo_root}/traces"}

if [[ ! -d "${tidb_repo}/pkg/session/test/txn" ]]; then
	printf 'TiDB repo not found at %s\n' "${tidb_repo}" >&2
	printf 'Set TIDB_REPO=/path/to/tidb or run from the expected db-consistency layout.\n' >&2
	exit 1
fi

if ! command -v go >/dev/null 2>&1; then
	if [[ -x /usr/local/go/bin/go ]]; then
		export PATH="/usr/local/go/bin:${PATH}"
	else
		printf 'go is not available on PATH\n' >&2
		exit 1
	fi
fi

if ! command -v python3 >/dev/null 2>&1; then
	printf 'python3 is not available on PATH\n' >&2
	exit 1
fi

if [[ "${output_dir}" != /* ]]; then
	output_dir="${PWD}/${output_dir}"
fi

mkdir -p "${output_dir}"
shopt -s nullglob
trace_paths=(
	"${output_dir}/1s-3t-7e-basic.yaml"
	"${output_dir}/1s-3t-5e-delete-miss.yaml"
	"${output_dir}/2s-4t-10e-mutual.yaml"
	"${output_dir}/2s-10t-28e.yaml"
	"${output_dir}/2s-12t-67e-mixed.yaml"
	"${output_dir}/1s-overwrite-chain.yaml"
	"${output_dir}/2s-delete-reinsert.yaml"
	"${output_dir}/3s-fanout.yaml"
	"${output_dir}/3s-large-roundrobin.yaml"
	"${output_dir}/3s-large-mixed.yaml"
	"${output_dir}/3s-very-large-roundrobin.yaml"
	"${output_dir}/3s-very-large-mixed.yaml"
)
legacy_trace_paths=(
	"${output_dir}/small-basic.yaml"
	"${output_dir}/small-delete-miss.yaml"
	"${output_dir}/small-two-session-mutual.yaml"
	"${output_dir}/medium-mixed.yaml"
	"${output_dir}"/*-overwrite-chain.yaml
	"${output_dir}"/*-delete-reinsert.yaml
	"${output_dir}"/*-fanout.yaml
	"${output_dir}"/*-mixed.yaml
	"${output_dir}"/*-large-roundrobin.yaml
	"${output_dir}"/*-large-mixed.yaml
	"${output_dir}"/*-very-large-roundrobin.yaml
	"${output_dir}"/*-very-large-mixed.yaml
)
rm -f "${trace_paths[@]}" "${legacy_trace_paths[@]}"

tmp_test="${tidb_repo}/pkg/session/test/txn/checker_tracegen_test.go"

cleanup() {
	rm -f "${tmp_test}"
}
trap cleanup EXIT INT TERM

cp "${script_dir}/co_checker_tracegen_test.go" "${tmp_test}"

(
	cd "${tidb_repo}/pkg/session/test/txn"
	export CO_CHECKER_TRACE_OUTPUT_DIR="${output_dir}"
	go test -run '^TestCoCheckerTraceGen$' -tags=intest,deadlock -count=1
)

printf '\nPreprocessing traces:\n'
generated_trace_paths=()
for path in "${trace_paths[@]}"; do
	preprocessed_path=$(python3 "${script_dir}/preprocess_trace.py" --in-place --rename "${path}")
	generated_trace_paths+=("${preprocessed_path}")
	printf '  %s -> %s\n' "${path}" "${preprocessed_path}"
done

printf '\nGenerated preprocessed traces:\n'
for path in "${generated_trace_paths[@]}"; do
	printf '  %s\n' "${path}"
done
