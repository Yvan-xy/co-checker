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
4      flags: uint32, 0 = raw bytes, bit 0 = dense IDs
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

## Dense-ID Binary Variant

`cotrace-densify` converts a raw binary trace into a checker-oriented variant
with header flag bit 0 set. The header and transaction records are unchanged.
Each variable-width raw event record is replaced by this fixed 16-byte record:

```text
bytes  field
4      seq: uint32
1      op: uint8, 0 = read, 1 = write
1      flags: uint8, identical to the raw event flags
2      reserved: uint16, must be 0
4      key_id: uint32
4      value_id: uint32, UINT32_MAX when has_value is not set
```

Key IDs and non-null value IDs form independent, zero-based namespaces. IDs
are assigned in first-seen order and must be contiguous. The loader validates
that invariant with constant auxiliary memory and derives the distinct key and
value counts while loading.

The dense variant deliberately omits the original key/value bytes. It is
accepted only by `snapshot-so-ww`, whose decision procedure uses the dense ID
arrays. Raw-dependent checkers and all dump modes are rejected. Violations on
dense input identify the affected `key_id` instead of printing unavailable raw
key bytes.

Build and convert without materializing the complete raw trace:

```bash
cmake --build build --target cotrace-densify
build/cotrace-densify \
  --stats bin_traces/dense/example.stats.json \
  bin_traces/example.trace \
  bin_traces/dense/example.dense.trace
```

Run the supported checker directly on the dense file:

```bash
build/co-checker --format bin --checker snapshot-so-ww \
  bin_traces/dense/example.dense.trace
```

## In-memory Dense IDs

The raw on-disk representation retains the original binary key/value bytes.
Both YAML and raw binary parsers additionally assign independent zero-based
`uint32_t` key and non-null value IDs in deterministic first-seen order. The
dense binary parser loads those IDs directly. Per-event IDs are stored in
compact parallel arrays on `Trace`; absent values, read misses, and delete
writes use `kNullValueId`.

Interning uses a temporary flat open-addressed table containing a mixed hash,
the dense ID, and a reference to the first event with that byte string. Hashes
only select candidates: full `HexValue` equality resolves collisions, so
distinct byte strings can never share an ID. Parser storage is reserved before
representative references are recorded and the temporary table is destroyed
before checking starts.

`snapshot-so-ww` uses these IDs for vector-indexed snapshots, read state,
write deduplication, wrong-key state, and rollback deltas. Other checkers and
diagnostics continue to use the original byte strings, so their behavior and
the external trace format are unaffected.

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

## TimeKiller JSON Converter

The `cotrace-to-timekiller` executable streams either raw or dense-ID
`COTRACE1` to the key-value JSON array accepted by TimeKiller/Chronos:

```bash
build/cotrace-to-timekiller \
  --stats /tmp/tpcc.stats.json \
  bin_traces/tpcc/tpcc.trace \
  /tmp/tpcc.json
```

Transaction record positions become `tid`; session and physical timestamp
fields are copied to `sid`, `sts.p`, and `cts.p`, with both logical
timestamp fields set to zero. Event order is preserved and operations are
written as `r` or `w`.

For raw input, arbitrary key and non-null value byte strings are assigned
independent, zero-based IDs in first-occurrence order. The dictionaries use the
full byte strings for equality, so hash collisions cannot merge distinct
inputs. For dense input, the validated IDs are emitted directly without
rebuilding dictionaries. Repeated values reuse their earlier ID in both cases.
Read misses, delete writes, and other absent values are emitted as JSON `null`.

The converter validates the binary structure and requires all timestamps and
generated IDs to fit a signed Java `long`. JSON is written incrementally to a
temporary file and renamed only after conversion succeeds. The optional stats
file records transaction, event, distinct-key, distinct-non-null-value, and
JSON-byte counts for independent validation.

## Generated Binary Traces

Trace generation scripts write normalized YAML under `./traces` and mirror the
same relative structure under `./bin_traces`. For example:

```text
traces/tpcc/4s-879t-724729e-tpcc-1w-1th-1s.yaml
bin_traces/tpcc/4s-879t-724729e-tpcc-1w-1th-1s.trace
```

Override the binary output root with `BIN_TRACE_DIR`.
