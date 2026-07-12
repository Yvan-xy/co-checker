# Reproducible commands

All commands are run from `/home/dyf/code/db-consistency/co-checker` unless a
different directory is shown.

## Build and test

```sh
cmake -S online -B online/build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build online/build -j2
ctest --test-dir online/build --output-on-failure
```

ASan, UBSan, and leak checking:

```sh
cmake -S online -B online/build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCO_CHECKER_ONLINE_ENABLE_ASAN=ON \
  -DCO_CHECKER_ONLINE_ENABLE_UBSAN=ON
cmake --build online/build-asan -j2
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
  ctest --test-dir online/build-asan --output-on-failure
```

Unchanged offline regression suite:

```sh
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

## Trace replay and layout sampling

```sh
online/build-release/co_checker_online_trace_replay \
  bin_traces/tpcc/4s-900t-729344e-tpcc-optimistic-1w-1th-1s.trace

python3 online/tools/sample_trace_layout.py \
  bin_traces/tpcc/4s-900t-729344e-tpcc-optimistic-1w-1th-1s.trace \
  --output online/results/layout/tpcc-1w-1th-1s.json
```

The sampler's candidate sizes are configurable through `--chunk-sizes`.

Pinned scalar/SIMD microbenchmark:

```sh
cmake --build online/build-release -j2
for run in 1 2 3 4 5; do
  taskset -c 2 online/build-release/co_checker_online_microbench 200000 30
done
```

Compare all top-level fixtures with the unchanged offline executable:

```sh
python3 online/tools/differential_replay.py \
  --trace-dir traces \
  --trace-convert build/trace-convert \
  --offline-checker build/co-checker \
  --online-replay online/build/co_checker_online_trace_replay \
  --work-dir online/results/differential/binaries \
  --output online/results/differential/report.json
```

## Initial C++ profile

```sh
perf stat -e cycles,instructions,branches,branch-misses,cache-misses \
  online/build-release/co_checker_online_trace_replay \
  bin_traces/tpcc/4s-900t-729344e-tpcc-optimistic-1w-1th-1s.trace

perf record -o online/results/perf-core-replay.data -g --call-graph fp \
  online/build-release/co_checker_online_trace_replay \
  bin_traces/tpcc/4s-900t-729344e-tpcc-optimistic-1w-1th-1s.trace
```

Optimized profile and flame graph:

```sh
perf stat -x, -o online/results/perf-core-replay-flat-interner.stat \
  -e cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  online/build-release/co_checker_online_trace_replay \
  bin_traces/tpcc/4s-900t-729344e-tpcc-optimistic-1w-1th-1s.trace

perf record -F 999 -g --call-graph dwarf \
  -o online/results/perf-core-replay-flat-interner.data -- \
  online/build-release/co_checker_online_trace_replay \
  bin_traces/tpcc/4s-900t-729344e-tpcc-optimistic-1w-1th-1s.trace

perf script -i online/results/perf-core-replay-flat-interner.data \
  | online/worktrees/FlameGraph/stackcollapse-perf.pl \
  | online/worktrees/FlameGraph/flamegraph.pl \
      --title 'Online SOWW C++ replay (729344 events)' \
      --countname cycles \
  > online/results/perf-core-replay-flat-interner.svg
```

The recorded FlameGraph checkout is commit
`41fee1f99f9276008b7cd112fca19dc3ea84ac32`.

## Native C ABI pipeline benchmark

The focused capture/order/check benchmark, stalled-frontier byte plateau, and
profiling commands are documented in `c_api_pipeline_benchmark.md`. The primary
recorded run is:

```sh
taskset -c 0,1 online/build-release/co_checker_online_c_api_bench \
  --transactions 50000 --warmup 5000 --repetitions 5 \
  --frontier-every 256 --plateau-batches 128
```

Raw JSON lines and the interpreted medians are in
`online/results/c_api_pipeline_raw.jsonl` and
`online/results/c_api_pipeline_benchmark.md`.

## Live TiDB TPC-C

The complete empty-template, four-mode, capability-gate, graceful-close,
post-check, pprof, and fail-closed workflow is documented in
`online/doc/tpcc_workflow.md`. Recorded measurements and their machine-readable
audit are in `online/results/tpcc-live/summary.md` and `summary.json`.

## TiDB driver validation

```sh
cd /home/dyf/code/db-consistency/tidb-online
TIDB_ONLINE_CHECKER_TEST_LIBRARY=\
/home/dyf/code/db-consistency/co-checker/online/build-release/libco_checker_online_c.so \
  go test ./pkg/store/driver/txn ./pkg/store/driver/txn/captureadapter
TIDB_ONLINE_CHECKER_TEST_LIBRARY=\
/home/dyf/code/db-consistency/co-checker/online/build-release/libco_checker_online_c.so \
  go test -race ./pkg/store/driver/txn ./pkg/store/driver/txn/captureadapter
GOEXPERIMENT=cgocheck2 TIDB_ONLINE_CHECKER_TEST_LIBRARY=\
/home/dyf/code/db-consistency/co-checker/online/build-release/libco_checker_online_c.so \
  go test ./pkg/store/driver/txn/captureadapter \
    -run '^TestNativeAdapterModesLifecycle$'
```

`make bazel_prepare` must also be run when Bazel is available.
