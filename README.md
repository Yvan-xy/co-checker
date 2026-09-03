# co-checker

Commit-order consistency checker framework for TiDB transaction traces.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The build fetches `yaml-cpp` into `build/_deps` through CMake `FetchContent`.

## Usage

Parse a trace:

```bash
build/co-checker traces/2s-10t-28e.yaml
```

Dump parsed state or committed version chains:

```bash
build/co-checker --dump parsed traces/2s-10t-28e.yaml
build/co-checker --dump versions traces/2s-10t-28e.yaml
```

Run the prototype snapshot checker explicitly:

```bash
build/co-checker --checker co-si traces/2s-4t-10e-mutual.yaml
```

Enable checker-specific statistics:

```bash
build/co-checker --checker snapshot-so-cl --stat traces/2s-4t-10e-mutual.yaml
```

Pack a normalized YAML trace into the compact binary trace format:

```bash
build/trace-convert traces/2s-4t-10e-mutual.yaml
build/co-checker --format bin --checker co-si-so-only traces/2s-4t-10e-mutual.trace
```

See `doc/binary_trace.md` for the binary format layout.

## Trace Generation

The `trace-gen/` directory contains the TiDB-backed trace fixture generator and
preprocessor. It runs instrumented TiDB `testkit` workloads, writes raw YAML
transaction traces, filters them into checker-ready committed traces, normalizes
session IDs, and renames files using the final
`<sessions>s-<transactions>t-<events>e[-suffix].yaml` counts. It also writes
matching compact binary traces under `./bin_traces`.

Regenerate the trace fixtures with:

```bash
./trace-gen/generate.sh
```

Generate a parameterized YCSB trace with the official PingCAP client plus the
checked-in grouped-transaction patch:

```bash
THETA=0.8 OPS_PER_TXN=8 SESSIONS=4 \
  RECORD_COUNT=1000 TXN_COUNT=1000 \
  ./trace-gen/generate_ycsb.sh
```

The fixture generator expects its historical instrumented checkout at `../tidb`;
the YCSB runner defaults to the checkout in this workspace at
`../tidb-tracegen`. Both write YAML under `./traces` and matching binary files
under `./bin_traces`. See `trace-gen/README.md` for path overrides, workload
knobs, and manual preprocessing commands.

## Online Engine Design

See the [online engine implementation plan](doc/online_engine_plan.md) and its
[architecture figure](doc/online_checker_architecture.svg) for the proposed
embedded asynchronous runtime, storage backends, commit-order gate, validation,
and rollout phases.
