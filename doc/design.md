# Commit-Order Consistency Checker Design

## Purpose

`co-checker` reads preprocessed TiDB transaction traces and checks consistency
properties over the observed execution. The current implementation focuses on
snapshot-isolation checks with commit order as the primary ordering relation.

The implementation priorities are:

- Parse YAML trace streams into typed `Trace`, `Transaction`, and `Event`
  objects.
- Preserve commit-order transaction ids.
- Build reusable committed-version indexes.
- Keep parser, index construction, checking, diagnostics, and dumps separate.
- Keep checker output deterministic enough for tests and trace debugging.

The checker currently assumes traces are already reduced to committed
application transactions. It does not decode TiDB keys or values into SQL-level
rows.

## Trace Input

The CLI consumes a YAML document stream. Each YAML document represents one
committed transaction:

```yaml
---
session_id: 1
start_ts: 10
commit_ts: 20
events:
  - seq: 1
    op: R
    key_hex: 01
    value_hex: 0a
```

Required transaction fields:

- `session_id`: normalized session id.
- `start_ts`: transaction snapshot/start timestamp.
- `commit_ts`: commit timestamp. Transactions must appear in nondecreasing
  commit order when `ParseOptions::require_commit_order` is true.
- `events`: event list, possibly empty.

Required event fields:

- `seq`: event sequence inside its transaction. It must be strictly increasing.
- `op`: `R` for read or `W` for write.
- `key_hex`: hex-encoded key bytes.

Optional event fields:

- `value_hex`: hex-encoded value bytes.
- `delete`: true for delete writes.
- `miss`: true for read misses.

Parser constraints:

- `key_hex` and `value_hex` must be valid even-length hex strings.
- Duplicate `commit_ts` values are rejected by default.
- `miss` and `value_hex` cannot both be present.
- `delete` and `value_hex` cannot both be present.
- Read events cannot have `delete`.
- Write events cannot have `miss`.
- Unknown metadata fields are ignored. For example, traces may contain
  `source: snapshot`, which is not part of the typed model.

## Core Data Model

Primitive ids and timestamps are defined in `common/trace/types.h`:

```cpp
using TxnId = std::uint32_t;
using EventId = std::uint32_t;
using SessionId = std::uint32_t;
using Timestamp = std::uint64_t;
using EventSeq = std::uint32_t;
```

`HexValue` stores decoded bytes in two inline `uint64_t` words plus
`extra_words` for longer keys or values. `Key` and `Value` are aliases for
`HexValue`, and equality/hashing operate on the packed representation rather
than the original strings.

`common/trace/trace.h` defines:

```cpp
enum class EventOp { Read, Write };

struct Event {
  EventId id;
  TxnId txn_id;
  EventSeq seq;
  EventOp op;
  Key key;
  std::optional<Value> value;
  bool is_delete;
  bool is_miss;

  std::string to_string(std::string_view indent = "") const;
  void print(std::ostream&, std::string_view indent = "") const;
  void print() const; // prints to std::cout
};

struct Transaction {
  TxnId id;
  SessionId session_id;
  Timestamp start_ts;
  Timestamp commit_ts;
  std::vector<EventId> event_ids;

  std::string to_string(std::string_view indent = "") const;
  void print(std::ostream&, std::string_view indent = "") const;
  void print() const; // prints to std::cout
};

struct Trace {
  std::vector<Transaction> transactions;
  std::vector<Event> events;

  const Transaction& getTxn(TxnId) const;
  const Event& getEvent(EventId) const;
};
```

`Trace::transactions` is ordered by parsed commit order, and parser-assigned
`TxnId` values are dense indexes into that vector. `Trace::events` owns all
events exactly once; transactions refer to their events by `EventId`.

## Snapshots

`common/trace/snapshot.h` defines snapshot helpers used by SO-only checking and
debugging.

`Snapshot` represents materialized key/value state:

```cpp
struct Snapshot {
  std::unordered_map<Key, Value, HexValueHash> data;

  void copy_from(const Snapshot&);
  Snapshot clone() const;
  bool has_key(const Key&) const;
  std::optional<Value> get(const Key&) const;
  void put(const Key&, const Value&);
  void erase(const Key&);
  void set(const Key&, const std::optional<Value>&);

  std::string to_string(std::string_view indent = "") const;
  void print(std::ostream&, std::string_view indent = "") const;
  void print() const;
};
```

Missing keys represent absent/deleted keys. Mutation is intentionally explicit:
there is no `operator[]` because returning `std::optional<Value>` by value makes
assignments such as `snapshot[key] = value` silently update only a temporary.

`SnapshotOpt` records observed key presence even when the value is null:

```cpp
struct SnapshotOpt {
  std::unordered_map<Key, std::optional<Value>, HexValueHash> data;

  SnapshotOpt clone() const;
  std::optional<Value> get(const Key&) const;
  bool has_key(const Key&) const;
  void set(const Key&, const std::optional<Value>&);

  std::string to_string(std::string_view indent = "") const;
  void print(std::ostream&, std::string_view indent = "") const;
  void print() const;
};
```

This is used for transaction read sets where "the key was read and returned
miss/null" is different from "the key was not read".

`SnapshotMap<T>` and `SnapshotVectorMap<T>` are unordered-map aliases with
deterministic print helpers. Snapshot formatting implementations live in
`src/trace/trace.cpp`.

## Trace Index

`common/trace/trace_index.h` defines committed versions and lookup indexes:

```cpp
struct Version {
  TxnId writer;
  EventId write_event;
  Timestamp commit_ts;
  Key key;
  std::optional<Value> value;
  bool is_delete;
};

struct TraceIndex {
  std::unordered_map<Key, std::vector<Version>, HexValueHash> versions_by_key;
  std::unordered_map<Timestamp, TxnId> txn_by_commit_ts;
};
```

`TraceIndexBuilder` validates event ownership and event id bounds while building
the index. For each transaction it keeps only the final write per key in that
transaction. Deletes become committed tombstone versions.

There is currently no `txns_by_session` index in `TraceIndex`; session-local
state used by the SO-only checker is maintained in its checker context.

## Parser

`TraceParser` returns `Result<Trace>` where errors are `DiagnosticList`:

```cpp
struct ParseOptions {
  bool require_commit_order = true;
  bool reject_duplicate_commit_ts = true;
};

class TraceParser {
public:
  explicit TraceParser(ParseOptions options = {});
  Result<Trace> parse_file(std::string_view path) const;
  Result<Trace> parse_stream(std::istream& input) const;
};
```

The parser uses `yaml-cpp`; YAML is not parsed by hand. Diagnostics include a
human-readable message with document index, event index, field name, and reason
where applicable. Diagnostics no longer carry separate stable code strings.

## Checker Interface

All checkers implement `checker::Checker`:

```cpp
struct CheckerContext {
  const common::Trace& trace;
  const common::TraceIndex& index;
  common::Logger* logger = nullptr;
};

class Checker {
public:
  virtual ~Checker() = default;
  virtual std::string_view name() const = 0;
  virtual CheckResult run(const CheckerContext& ctx) const = 0;

  virtual common::Diagnostic make_diagnostic(
      std::string message,
      const common::Transaction& txn,
      const common::Event& event) const;
};
```

`CheckerRegistry` owns checker instances and supports lookup by name. The
default registry currently registers:

- `co-si-basic`
- `co-si-so-only`

## Checkers

### `co-si-basic`

`CoSnapshotBasicChecker` validates each read against the latest committed
version visible at the reader transaction's `start_ts`.

For each read:

- Find the reader transaction from `event.txn_id`.
- Look up the committed version list for `event.key`.
- Use binary search to find the latest version with
  `version.commit_ts <= txn.start_ts`.
- If no visible version exists, or the visible version is a delete, the read
  must be a miss.
- If a visible non-delete version exists, the read must not be a miss and must
  include the same value.

The checker reports diagnostics for unexpected values, unexpected misses,
missing read values, invalid non-delete versions, and invalid event ownership.
It sets `CheckStatus::Fail` when diagnostics are present.

### `co-si-so-only`

`SnapshotSOChecker` is an experimental checker that uses session-order state to
eliminate some snapshot candidates.

It maintains:

- `session_snapshots`: latest materialized snapshot per session.
- `latest_txns_by_session`: latest transaction id per session.
- `latest`: a running materialized snapshot in commit order.

For a transaction, the checker:

1. Finds the latest previous transaction in the same session.
2. Starts from that session snapshot.
3. Builds a `SnapshotOpt` read set so read misses are remembered.
4. Tracks keys whose observed reads disagree with the session snapshot.
5. Scans transactions between the session predecessor and current transaction
   to see whether intervening writes can eliminate all disagreements.
6. Emits diagnostics for remaining wrong keys.

This checker is still conservative and under active development. It now sets
`CheckStatus::Fail` whenever diagnostics are produced.

## Diagnostics and Results

Current result types:

```cpp
enum class CheckStatus {
  Pass,
  Fail,
  Inconclusive,
};

enum class Severity {
  Info,
  Warning,
  Error,
};

struct Diagnostic {
  Severity severity = Severity::Error;
  std::string checker;
  std::string message;
  std::optional<TxnId> txn_id;
  std::optional<EventId> event_id;
  std::optional<EventSeq> event_seq;
  std::optional<Key> key;
};

struct CheckResult {
  CheckStatus status = CheckStatus::Pass;
  DiagnosticList diagnostics;
};
```

Diagnostics are message-oriented; the earlier `Diagnostic::code` field has been
removed. CLI diagnostic output includes severity, checker, message, and
available transaction/event/key context.

CLI exit codes:

- `0`: parsing/indexing succeeded and all selected checkers passed.
- `1`: at least one selected checker returned `Fail`.
- `2`: parse or index error.
- `3`: CLI/configuration error.

## CLI and Dumps

CLI usage:

```bash
build/co-checker [--checker none|co-si-basic|co-si-so-only|all] \
  [--dump parsed|versions|all] <trace.yaml>
```

Behavior:

- Default checker is `none`; the CLI parses and indexes the trace and prints
  `trace: pass` if no dump is requested.
- `--checker all` runs all registered checkers.
- `--dump parsed` prints transactions and events using their `print` methods.
- `--dump versions` prints committed version chains by key.
- `--dump all` prints both parsed trace and version chains.

There is no current `--log-level`, `--dump-dir`, `reads` dump, or JSON output.

## Test and Trace Driver

Unit tests are split by module:

- `test_trace.cpp`: hex values, parser layout/errors, trace formatting.
- `test_snapshot.cpp`: `Snapshot`, `SnapshotOpt`, and formatting.
- `test_checker.cpp`: basic snapshot checker and SO-only status behavior.
- `test_index.cpp`: committed version index behavior.
- `test_harness.cpp`: shared helpers.
- `test_main.cpp`: dispatches module test runners.

Build and run tests:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`test/run_traces.py` runs `co-checker` across trace files:

```bash
test/run_traces.py --checker none
test/run_traces.py --checker all
test/run_traces.py --trace '*mixed*' --checker co-si-so-only
test/run_traces.py --list
```

Driver options include `--checker-bin`, `--traces-dir`, repeated `--trace`
selectors, `--fail-fast`, and `--verbose`. Unrecognized arguments are forwarded
to `co-checker`.

## Performance Notes

The implementation currently loads the full trace into memory. Important
efficiency choices already in place:

- Hex is decoded and packed once during parsing.
- Events are stored once in `Trace::events`.
- Transactions store event ids, not duplicated event objects.
- `TraceIndex` stores per-key version vectors once.
- The basic checker uses binary search in each key's version list.
- `Snapshot::copy_from` clears and reserves before insertion to reduce rehashing
  when copying materialized snapshots.

Expected basic checker complexity:

- Index build: `O(total_events)`.
- Read checking: `O(read_events * log(versions_for_key))`.

The SO-only checker uses materialized snapshot copies and is not yet optimized
for very large traces. Future work may use persistent snapshots, deltas, or
copy-on-write structures, but delete/miss semantics must remain explicit.

## Open Questions

- Should `Snapshot` stay fully materialized, or should later checkers use a
  parent-plus-delta representation?
- Should equal `commit_ts` values remain rejected, or become a partial-order
  relation if TiDB traces require it?
- Should diagnostics regain stable machine-readable codes in addition to
  messages?
- Should dumps add JSON output for downstream tooling?
- Should TiDB key/value decoding be introduced as a separate optional layer?
