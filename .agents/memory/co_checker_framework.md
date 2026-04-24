# Co-Checker Framework Notes

The C++ checker currently consumes preprocessed YAML only: committed documents,
sorted by ascending `commit_ts`, with dense per-file `session_id` values. Raw
TiDB traces must pass through `trace-gen/preprocess_trace.py` first because raw
documents can contain `status: "rolled_back"` and omit `commit_ts`.

Important current mismatch: TiDB and the preprocessor preserve event
`source: snapshot|mem_buffer`, but the C++ `Event` model does not store
`source`. `co-si` therefore checks all reads as snapshot reads at `start_ts`.
The generated `traces/2s-12t-67e-mixed.yaml` fixture contains `mem_buffer`
reads and fails `build/co-checker --checker co-si` for that reason.

Useful validation commands:

```bash
ctest --test-dir build --output-on-failure
python3 -m unittest trace-gen/test_preprocess_trace.py
build/co-checker --checker co-si traces/2s-4t-10e-mutual.yaml
build/co-checker --checker co-si traces/2s-12t-67e-mixed.yaml
```

