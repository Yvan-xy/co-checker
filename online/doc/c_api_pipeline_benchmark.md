# Native C ABI pipeline benchmark

This benchmark isolates the native boundary and checker runtime from TiDB and
the Go adapter. It complements, but does not replace, the end-to-end TPC-C
measurements.

## Workload

`co_checker_online_c_api_bench` sends one producer's committed point
transactions through the public C ABI. Every transaction has a unique 16-byte
key and two normalized events:

1. a snapshot read with `Absent` value state;
2. a write with a 32-byte `Present` value.

The benchmark uses monotonically increasing start and commit timestamps, emits
both database and producer frontiers every 256 transactions, and runs exactly
the same transaction/event counts in capture-only, order-only, and full modes.
Each mode gets a separate warm-up runtime and a fresh runtime per measured
repetition. Full mode must close with `Pass`; the two diagnostic modes must
close with verdict `Unavailable`.

Outer timers divide the caller's work into identity/registration, input
packing, publish, frontier, and close calls. Native cumulative timers divide
publish into parsing, capture reservation, producer-mutex wait, chunk building,
and runtime handoff. Runtime timers record commit ordering and SOWW checking on
the consumer thread. Consumer timers overlap caller time, so native timer rows
must not be summed with the outer timers.

## Release build and primary run

Run from the checker repository root:

```sh
cmake -S online -B online/build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCO_CHECKER_ONLINE_BUILD_TESTING=ON
cmake --build online/build-release -j2
ctest --test-dir online/build-release --output-on-failure

taskset -c 0,1 online/build-release/co_checker_online_c_api_bench \
  --transactions 50000 \
  --warmup 5000 \
  --repetitions 5 \
  --frontier-every 256 \
  --plateau-batches 128
```

The executable writes `CONFIG_JSON`, `RESULT_JSON`, and `PLATEAU_JSON` lines.
The recorded stdout, with host and command metadata, is in
`online/results/c_api_pipeline_raw.jsonl`.

## Stalled-frontier byte plateau

The plateau subtest creates an order-only runtime whose capture and reorder
budgets are both exactly `N * reserved_bytes_per_batch`. It publishes `N`
transactions without either frontier, verifies that all bytes remain owned by
the reorder buffer, and attempts one additional publish. The last reservation
must wait for the configured 50 ms timeout and return `ResourceError` without
incrementing captured-transaction/event counters. The runtime is then fatal and
cannot issue a consistency verdict; incomplete evidence is never reported as a
violation.

## Profiling commands

The throughput run is the authority for latency and throughput. `perf` and
`strace` substantially perturb this synchronization-heavy workload and are used
only for attribution.

```sh
perf stat \
  -e task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations,page-faults \
  -- taskset -c 0,1 online/build-release/co_checker_online_c_api_bench \
     --mode full --transactions 20000 --warmup 2000 --repetitions 1 \
     --frontier-every 256 --skip-plateau

perf record -e cycles:u -F 499 -g --call-graph dwarf \
  -o online/results/c_api_pipeline_full_user.perf.data -- \
  taskset -c 0,1 online/build-release/co_checker_online_c_api_bench \
    --mode full --transactions 100000 --warmup 5000 --repetitions 1 \
    --frontier-every 256 --skip-plateau

perf report --stdio --no-children --call-graph none --no-demangle \
  --sort dso,symbol --percent-limit 0.50 \
  -i online/results/c_api_pipeline_full_user.perf.data

perf script -i online/results/c_api_pipeline_full_user.perf.data \
  | online/worktrees/FlameGraph/stackcollapse-perf.pl \
  > online/results/c_api_pipeline_full_user.folded

online/worktrees/FlameGraph/flamegraph.pl \
  --title 'Online C ABI Full Pipeline (user cycles)' \
  --subtitle '100k point transactions; cycles:u; perf -F 499; CPUs 0-1' \
  --countname samples \
  online/results/c_api_pipeline_full_user.folded \
  > online/results/c_api_pipeline_full_user_flame.svg

strace -f -c taskset -c 0,1 \
  online/build-release/co_checker_online_c_api_bench \
  --mode full --transactions 10000 --warmup 1000 --repetitions 1 \
  --frontier-every 256 --skip-plateau
```

Kernel symbols are restricted on the recorded host. The `cycles:u` profile and
native function symbols are usable; unresolved kernel frames in the older
all-cycles capture should not be interpreted as checker functions.
