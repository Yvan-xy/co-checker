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

Parse and index a trace:

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
`<sessions>s-<transactions>t-<events>e[-suffix].yaml` counts.

Regenerate the trace fixtures with:

```bash
./trace-gen/generate.sh
```

By default this expects the instrumented TiDB checkout at `../tidb` and writes
fixtures into `./traces`. See `trace-gen/README.md` for path overrides and
manual preprocessing commands.
