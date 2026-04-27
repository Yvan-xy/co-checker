# Binary Trace Format

`*.trace` is the compact binary form of a checker-ready trace. It stores the
same logical model as the normalized YAML files:

- transactions are stored in commit order
- transaction ids are inferred from transaction record order
- event ids are inferred from event record order
- each transaction stores only its metadata and event count
- each event stores its sequence, operation, flags, key bytes, and optional
  value bytes

All integers are little-endian. Version 1 has no compression and no random
access index. It is intended to make checker startup a fast sequential read; a
separate index can be added later if we need partial loading.

## Header

```text
bytes  field
8      magic: ASCII "COTRACE1"
4      version: uint32, currently 1
4      flags: uint32, currently 0
8      transaction_count: uint64
8      event_count: uint64
```

## Transaction Record

Repeated `transaction_count` times:

```text
bytes  field
4      session_id: uint32
8      start_ts: uint64
8      commit_ts: uint64
4      event_count: uint32
```

The transaction id is the zero-based transaction record index.

## Event Record

Repeated `event_count` times inside each transaction:

```text
bytes  field
4      seq: uint32
1      op: uint8, 0 = read, 1 = write
1      flags: uint8
2      reserved: uint16, must be 0
var    key bytes
var    value bytes, present only when flags.has_value is set
```

Flags:

```text
bit 0  has_value
bit 1  delete
bit 2  miss
```

Each byte string is encoded as:

```text
4      byte_len: uint32
N      raw bytes
```

The YAML `key_hex` and `value_hex` strings are decoded before writing, so the
binary file stores one byte per two hex characters and does not repeat YAML
field names.

## Converter

Build:

```bash
cmake --build build --target trace-convert
```

Convert a normalized YAML trace:

```bash
build/trace-convert traces/2s-4t-10e-mutual.yaml
```

This writes `traces/2s-4t-10e-mutual.trace`. Use `-o` to choose a different
path:

```bash
build/trace-convert -o traces/tpcc/tpcc.trace traces/tpcc/tpcc.yaml
```

Use `--rename` to rewrite the output name using the actual
`<sessions>s-<transactions>t-<events>e` counts:

```bash
build/trace-convert --rename -o traces/tpcc/tpcc.trace traces/tpcc/tpcc.yaml
```

`co-checker` can force binary parsing with `--format bin`:

```bash
build/co-checker --format bin --checker co-si-so-only traces/2s-4t-10e-mutual.trace
```

For YAML, use `--format yaml`. The default is `--format auto`, which keeps the
old behavior and detects binary traces by magic header.

## Generated Binary Traces

Trace generation scripts write normalized YAML under `./traces` and mirror the
same relative structure under `./bin_traces`. For example:

```text
traces/tpcc/4s-879t-724729e-tpcc-1w-1th-1s.yaml
bin_traces/tpcc/4s-879t-724729e-tpcc-1w-1th-1s.trace
```

Override the binary output root with `BIN_TRACE_DIR`.
