# Commit-Order Consistency Checker Design

## Goals

Build a small, testable C++ framework that reads TiDB transaction traces, converts
them into explicit internal models, and runs one or more consistency checkers over
that model.

The framework should:

- Parse YAML trace streams into typed `Trace`, `Transaction`, and `Event`
  structures.
- Preserve commit-order semantics as the primary ordering relation.
- Make it easy to add new checkers without changing parser or CLI code.
- Provide structured diagnostics and optional debug dumps for parser output,
  derived relations, and checker decisions.
- Keep parsing, model normalization, checking, and reporting separate.

Non-goals for the first version:

- Decode TiDB row values into SQL rows or columns.
- Prove every isolation level. The first target is snapshot isolation with
  commit-order checks.
- Support raw, unpreprocessed TiDB traces with rolled-back transactions. The
  generator currently preprocesses traces into committed-only, commit-ordered
  YAML.

## Input Trace Contract

The checker consumes a YAML document stream. Each document is one committed
transaction after preprocessing:

```yaml
---
session_id: 1
start_ts: 465819165210968064
commit_ts: 465819165210968066
events:
  - seq: 1
    op: R
    key_hex: 7480000000000000725f728000000000000001
    value_hex: '80000100000002010064'
```

Required transaction fields:

- `session_id`: normalized per-trace session id starting at `1`.
- `start_ts`: TiDB transaction snapshot/start timestamp.
- `commit_ts`: TiDB commit timestamp. Commit order is ascending `commit_ts`.
- `events`: list of events, possibly empty.

Required event fields:

- `seq`: event sequence inside the transaction.
- `op`: `R` for read, `W` for write.
- `key_hex`: raw TiDB KV key bytes, hex encoded.

Optional event fields:

- `value_hex`: raw TiDB KV value bytes, hex encoded.
- `delete`: true for delete writes.
- `miss`: true for read misses.

Parser assumptions:

- Input is already filtered to committed application transactions.
- Input is already sorted by commit order, but the parser should verify this.
- `commit_ts` is globally unique for the initial checker. If equal commit
  timestamps appear, the parser should reject by default unless a later mode
  explicitly supports ties.
- `seq` values must be strictly increasing inside each transaction.
- `key_hex` and `value_hex` must be valid even-length hex strings.

## Architecture

The code should be organized around four layers:

1. Parse: YAML to typed model.
2. Normalize/index: validate invariants and build lookup indexes.
3. Check: run checker plugins over immutable trace data.
4. Report/dump: produce human-readable and machine-readable output.

Proposed source layout:

```text
src/
  cli/
    main.cpp
  trace/
    event.h
    transaction.h
    trace.h
    trace_parser.h
    trace_parser.cpp
    trace_validator.h
    trace_validator.cpp
    trace_index.h
    trace_index.cpp
  checker/
    checker.h
    checker_registry.h
    checker_registry.cpp
    check_result.h
    co_snapshot_checker.h
    co_snapshot_checker.cpp
  diag/
    diagnostic.h
    logger.h
    trace_dump.h
    trace_dump.cpp
    relation_dump.h
  util/
    hex.h
    yaml.h
test/
  trace_parser_test.cpp
  co_snapshot_checker_test.cpp
```

## Core Data Model

Keep the model simple and explicit. Avoid encoding checker-specific assumptions
in parser objects.

```cpp
using TxnId = uint32_t;
using EventId = uint32_t;
using SessionId = uint32_t;
using Timestamp = uint64_t;
using EventSeq = uint32_t;

struct HexValue {
    // Number of meaningful bytes represented by the packed words.
    uint32_t byte_len = 0;

    // Common case storage. These two words cover values up to 16 bytes.
    // Words are packed in input byte order; the final partial word is padded.
    std::array<uint64_t, 2> inline_words{};

    // Used only when the hex value is longer than 16 bytes.
    std::vector<uint64_t> extra_words;

    bool operator==(const HexValue& rhs) const {
        return byte_len == rhs.byte_len &&
               inline_words == rhs.inline_words &&
               extra_words == rhs.extra_words;
    }

    bool operator!=(const HexValue& rhs) const {
        return !(*this == rhs);
    }

    std::string to_hex() const;
};

struct HexValueHash {
    std::size_t operator()(const HexValue& value) const;
};

using Key = HexValue;
using Value = HexValue;

enum class EventOp {
    Read,
    Write,
};

struct Event {
    EventId id;                 // dense index into Trace::events
    TxnId txn_id;               // owning transaction
    EventSeq seq;
    EventOp op;
    Key key;
    std::optional<Value> value;
    bool is_delete = false;
    bool is_miss = false;
};

struct Transaction {
    TxnId id;                  // dense parser-assigned id, commit-order index
    SessionId session_id;
    Timestamp start_ts;
    Timestamp commit_ts;
    std::vector<EventId> event_ids; // event order inside this transaction
};

struct Trace {
    std::vector<Transaction> transactions; // sorted by commit_ts
    std::vector<Event> events;             // all events, stored once
};
```

The two inline `uint64_t` words are an optimization, not a hard size limit.
Current traces have values that fit in 16 bytes, but some TiDB keys are longer,
so `extra_words` is required for correctness.

`Trace::events` owns every event. `Transaction::event_ids` stores only stable
ids into that global event array. This keeps event storage contiguous and avoids
duplicating event payloads in transaction objects.

Important derived indexes:

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
    // All committed writes per key, sorted by commit_ts.
    std::unordered_map<Key, std::vector<Version>, HexValueHash> versions_by_key;

    // Transaction lookup by id and commit timestamp.
    std::unordered_map<Timestamp, TxnId> txn_by_commit_ts;

    // Transaction ids grouped by session.
    std::unordered_map<SessionId, std::vector<TxnId>> txns_by_session;
};
```

The parser owns syntax and type validation. The index builder owns derived
relations and semantic validation that depends on the full trace.

## Parser Design

Parser API:

```cpp
struct ParseOptions {
    bool require_commit_order = true;
    bool reject_duplicate_commit_ts = true;
};

class TraceParser {
public:
    explicit TraceParser(ParseOptions options);

    expected<Trace, DiagnosticList> parse_file(std::string_view path) const;
    expected<Trace, DiagnosticList> parse_stream(std::istream& input) const;
};
```

Parser responsibilities:

- Read YAML document stream.
- Validate required fields and field types.
- Parse `key_hex` and `value_hex` into packed `HexValue` objects.
- Validate that hex strings have even length and contain only hex digits.
- Pack the first 16 bytes into two inline `uint64_t` words.
- Pack bytes beyond 16 into `extra_words`.
- Convert operation strings into enums.
- Validate event-level constraints:
  - `R` may have `miss`.
  - `W` may have `delete`.
  - `miss` and `value_hex` must not both be present.
  - `delete` and `value_hex` should not both be present for the first version.
  - `seq` is strictly increasing.
- Ignore unrecognized event metadata by default, while keeping an optional strict
  mode available for schema debugging.
- Assign dense `TxnId` values in commit order.
- Assign dense `EventId` values as events are appended to `Trace::events`.
- Store each transaction's event order as `Transaction::event_ids`.
- Validate commit order and duplicate `commit_ts`.

Parsing should produce precise diagnostics with document number, event index,
field name, and reason. Do not silently coerce malformed fields.

YAML dependency:

- Use a real YAML parser. Do not parse YAML with ad hoc string logic.
- Prefer `yaml-cpp` for C++ implementation unless the build system makes another
  parser clearly better.

## Checker Extension Interface

All checkers should implement a narrow interface:

```cpp
struct CheckerContext {
    const Trace& trace;
    const TraceIndex& index;
    Logger* logger; // nullable
};

class Checker {
public:
    virtual ~Checker() = default;
    virtual std::string_view name() const = 0;
    virtual CheckResult run(const CheckerContext& ctx) const = 0;
};
```

Registry:

```cpp
class CheckerRegistry {
public:
    void register_checker(std::unique_ptr<Checker> checker);
    const Checker* find(std::string_view name) const;
    std::vector<std::string_view> names() const;
};
```

This lets the CLI run:

```bash
co-checker traces/2s-10t-28e.yaml
co-checker --checker co-si traces/2s-10t-28e.yaml
co-checker --checker all traces/2s-10t-28e.yaml
```

The default CLI mode should parse and index the trace only. Checkers are run
only when `--checker` is provided.

Adding a checker should require:

1. Implementing `Checker`.
2. Registering it in one place.
3. Adding focused tests.

No parser changes should be needed unless the input trace schema changes.

## Initial Co-Snapshot Checker

The first checker should validate snapshot reads against commit order.

For each read event:

- Use `event.txn_id` to find the reader transaction and its `start_ts`.
- Find the latest committed write to the same key with `writer.commit_ts <=
  reader.start_ts`.
- If no such write exists, the read must be a miss.
- If the latest visible write is a delete, the read must be a miss.
- Otherwise the read must be non-miss and `value` must equal the visible write
  value.

For write events:

- Insert them into `versions_by_key` during index construction.
- A delete write becomes a committed tombstone version.

This checker is intentionally conservative. It checks observed snapshot
visibility; it does not yet infer anti-dependencies, serialization graphs, or
all write skew patterns.

## Diagnostics and Results

Checker output should separate status from evidence:

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
    Severity severity;
    std::string checker;
    std::string code;
    std::string message;
    std::optional<TxnId> txn_id;
    std::optional<EventId> event_id;
    std::optional<EventSeq> event_seq;
    std::optional<Key> key;
};

struct CheckResult {
    CheckStatus status;
    std::vector<Diagnostic> diagnostics;
};
```

Examples:

- `parse.missing_field`: document is missing `commit_ts`.
- `parse.invalid_hex`: `value_hex` is not valid hex.
- `co_si.stale_read`: read value does not match latest visible committed
  version.
- `co_si.unexpected_miss`: read missed a key whose latest visible version exists.
- `co_si.unexpected_value`: read value differs from visible version.

The CLI should return:

- `0`: all selected checkers pass.
- `1`: at least one checker fails.
- `2`: parse or input validation error.
- `3`: CLI/configuration error.

## Logging and Dumps

The framework needs two related outputs:

- Logs: execution progress and debug information.
- Dumps: deterministic artifacts showing parsed and derived state.

Logger interface:

```cpp
enum class LogLevel {
    Error,
    Warn,
    Info,
    Debug,
    Trace,
};

class Logger {
public:
    virtual ~Logger() = default;
    virtual bool enabled(LogLevel level) const = 0;
    virtual void log(LogLevel level, std::string_view message) = 0;
};
```

CLI options:

```bash
co-checker --log-level debug trace.yaml
co-checker --dump parsed trace.yaml
co-checker --dump versions trace.yaml
co-checker --dump all --dump-dir out trace.yaml
```

Dump formats:

- `parsed`: normalized transaction list with event id lists plus the global event
  array. Keys and values should render through `HexValue::to_hex()`.
- `versions`: per-key committed version chain in commit order.
- `reads`: each read with the visible version selected by the checker.
- `result`: checker result summary plus diagnostics.

Prefer stable text or JSON for dumps. Stable output matters for tests and diffing
trace-analysis changes.

## CLI Flow

High-level flow:

1. Parse flags.
2. Parse trace file into `Trace`.
3. Build `TraceIndex`.
4. Optionally dump parsed/indexed state.
5. Run selected checker or checker set, if requested.
6. Print summary and diagnostics.
7. Return appropriate exit code.

Pseudo-code:

```cpp
int main(int argc, char** argv) {
    auto config = parse_cli(argc, argv);
    auto logger = make_logger(config.log_level);

    auto trace = TraceParser(config.parse_options).parse_file(config.trace_path);
    if (!trace) {
        print_diagnostics(trace.error());
        return 2;
    }

    auto index = TraceIndexBuilder().build(*trace);
    if (!index) {
        print_diagnostics(index.error());
        return 2;
    }

    dump_if_requested(*trace, *index, config);
    if (config.checkers.empty()) {
        return 0;
    }

    auto results = run_checkers(config.checkers, *trace, *index, logger.get());
    print_results(results);
    return any_failed(results) ? 1 : 0;
}
```

## Testing Strategy

Parser tests:

- Valid minimal trace.
- Multiple transactions in commit order.
- Reject missing `session_id`, `start_ts`, `commit_ts`, `events`.
- Reject duplicate or decreasing `commit_ts`.
- Reject malformed hex.
- Reject invalid read/write field combinations.
- Validate `seq` ordering.
- Validate transaction `event_ids` point to events whose `txn_id` matches the
  owning transaction.
- Validate `HexValue` equality and hashing for both inline-only values and values
  using `extra_words`.

Index tests:

- Multiple versions for one key.
- Deletes create tombstone versions.
- Session grouping.
- Commit timestamp lookup.

Checker tests:

- Read sees latest version before `start_ts`.
- Read does not see a future write.
- Read miss is valid before first write.
- Read miss is valid after visible delete.
- Stale read is reported with transaction id, event id, and event seq.
- Unexpected miss is reported.
- Unexpected value is reported.

Integration tests:

- Run the checker over every generated trace in `traces/`.
- Include at least one hand-written invalid trace per parser failure class.

## Performance Notes

The first implementation can load a full trace into memory. The data model should
still avoid unnecessary copying:

- Validate and pack hex once during parsing.
- Append `Event` objects once into `Trace::events`.
- Store `EventId` values in each `Transaction`.
- Store keys and values as packed `HexValue` objects, avoiding string comparison
  in checker hot paths.
- Build per-key version vectors once.
- Use binary search over each key's version list for visible-version lookup.

Expected checker complexity for snapshot reads:

- Build index: `O(total_events)`.
- Check reads: `O(read_events * log(versions_for_key))`.

For large traces, later work can add interning for repeated `HexValue` objects,
streaming parse validation, and compressed version storage.

## Open Questions

- Should the checker keep packed TiDB KV keys as the canonical key forever, or
  should a later decoding layer expose table id, row handle, and index key
  structure?
- Should read-only transactions with no events be retained if the generator ever
  emits them with `session_id`?
- Should equal `commit_ts` be rejected permanently, or supported as a partial
  order if TiDB ever emits ties?
- Should dumps include an optional decoded TiDB key view for debugging?

## Milestones

1. Add build system and parser dependency.
2. Implement model headers and parser.
3. Implement trace validation and `TraceIndex`.
4. Implement dumps for parsed trace and version chains.
5. Implement checker registry and result/diagnostic types.
6. Implement initial `co-si` snapshot visibility checker.
7. Add CLI.
8. Add parser, index, checker, and generated-trace integration tests.
