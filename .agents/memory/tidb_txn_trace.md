# TiDB Transaction Trace Instrumentation

The instrumented TiDB checkout is `/home/dyf/code/db-consistency/tidb` on branch `trace`.

Trace generation is controlled only by environment variables:

- `TIDB_TXN_TRACE_ENABLE`: enabled when set to `1`, `true`, `on`, or `yes`.
- `TIDB_TXN_TRACE_PATH`: append-only YAML output path. The parent directory must already exist.

The trace is sampled when TiDB creates each `tikvTxn` in `pkg/store/driver/txn/txn_driver.go`, not dynamically per event. There is no TiDB config key, SQL variable, or server flag for this trace.

Output is a stream of YAML documents, one per committed, read-only committed, or fully rolled-back transaction:

- Transaction fields: `session_id`, `status`, `start_ts`, `commit_ts`, `events`.
- `status` is `"committed"` for committed/read-only committed transactions or `"rolled_back"` for full transaction rollback.
- Event fields: `seq`, `op`, `key_hex`, `value_hex`, `delete`, `miss`, `source`.
- `op` is only `"R"` or `"W"`.
- `source` is `"snapshot"` for persisted MVCC snapshot reads or `"mem_buffer"` for same-transaction uncommitted buffer reads.
- `key_hex` and `value_hex` are raw TiKV encoded bytes; the checker must decode TiDB tablecodec keys/values.
- `delete: true` marks a write event as a delete.
- `miss: true` marks a read of a nonexistent key.

Commit-order semantics:

- For committed write transactions, numeric `commit_ts` is the authoritative commit order.
- File append order is not a global commit-order guarantee.
- Read-only transactions are flushed with `commit_ts == start_ts`; treat this as a snapshot/read marker, not a storage commit.
- Rolled-back transactions are flushed with `status: "rolled_back"` and no `commit_ts`; preprocessors should remove them before consistency checking.
- `session_id` is a TiDB SQL connection id and is not globally unique across TiDB processes or restarts.

Instrumentation locations:

- `pkg/store/driver/txn/txn_driver.go`: trace buffer/sink, transaction wiring, commit/rollback flushing, direct txn Get/BatchGet/Iter/Set/Delete hooks.
- `pkg/store/driver/txn/snapshot.go`: direct snapshot Get/BatchGet/Iter/IterReverse read hooks.
- `pkg/store/driver/txn/unionstore_driver.go`: mem-buffer write/delete hooks and UnionScan-style mem-buffer read hooks.
- `pkg/session/session.go`: read-only transaction flush via `TraceReadOnlyCommit`.
- `pkg/sessiontxn/isolation/base.go`: session id attached through `kv.SessionID`.

Important caveats for the checker:

- No explicit begin, commit, or abort event records exist.
- Full transaction rollback emits a `status: "rolled_back"` document. Commit errors still discard the trace buffer without emitting a document.
- The trace has no per-event timestamp, read timestamp, or observed MVCC version commit timestamp.
- Snapshot visibility must be inferred from transaction `start_ts`, transaction `commit_ts`, event `source`, and decoded key/value content.
- Statement rollback/savepoint rollback does not roll back already appended trace events, so committed transaction traces may include speculative events from failed statements, `ROLLBACK TO SAVEPOINT`, FK cascade rollback, or internal retry paths.
- Range/coprocessor reads may be missing unless they pass through instrumented `kv.Snapshot` or txn Get/BatchGet/Iterator paths.
- Writes are traced when added to the transaction buffer, not when prewritten or committed.
- Pipelined DML writes are traced when added, but pipelined mem-buffer snapshot reads are suppressed by empty retrievers/iterators.
- The trace sink uses a process-local mutex, opens the file with append, writes one YAML document, and does not fsync; multiple TiDB processes writing the same path have no shared file lock.
