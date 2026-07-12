# co-checker online runtime

This directory is an independently buildable implementation of the online
checker. The frozen protocol and consistency semantics are defined by the
repository's authoritative online-engine plan.

Build and run its tests from the checker repository root:

```sh
cmake -S online -B online/build -G Ninja
cmake --build online/build
ctest --test-dir online/build --output-on-failure
```

Sanitizer builds use the CMake options
`CO_CHECKER_ONLINE_ENABLE_ASAN`, `CO_CHECKER_ONLINE_ENABLE_UBSAN`, and
`CO_CHECKER_ONLINE_ENABLE_TSAN`.

The versioned native ABI is described in `doc/c_api_v1.md`. Reproducible native
and live TiDB validation commands are in `doc/reproducibility.md` and
`doc/tpcc_workflow.md`; recorded conclusions are indexed by `results/README.md`.
