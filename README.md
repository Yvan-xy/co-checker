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
