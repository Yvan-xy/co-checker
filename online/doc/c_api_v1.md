# Native C ABI V1

The public header is `include/co_checker/online/c_api.h`. A normal CMake build
produces `libco_checker_online_c.so`; consumers should resolve the versioned
`co_checker_online_*_v1` symbols from a configured library path.

## Runtime modes

- `CAPTURE_ONLY`: validate, copy into native chunks, and recycle immediately.
- `ORDER_ONLY`: capture, queue, and commit-order release without SOWW checking.
- `FULL`: capture, order, and check.

Only `FULL` can return `PASS` or `VIOLATION`. The other modes deliberately
return an unavailable verdict. Queue, reorder, capture, retained engine state,
timeouts, chunk size, cluster epoch, and mode are explicit fixed-width options.
The default chunk segment is 4 KiB, selected by the checked-in layout corpus.

## Identity and attempt lifecycle

1. Create a runtime with the complete required-producer membership.
2. Create a session for one producer. The runtime returns its fresh epoch,
   producer ID, and monotonic local session counter.
3. Allocate a logical transaction with a strictly increasing session sequence.
   Native allocation guarantees that its 128-bit transaction ID is unique in
   the runtime.
4. Register an attempt with the real `start_ts`. Success is synchronous: its
   active-start guard has linearized before the call returns.
5. Either abort that physical attempt or publish exactly one committed flat
   buffer. Abort leaves the logical ID valid for an automatic retry; a retry
   receives a fresh attempt ID.
6. On normal process shutdown, call `quiescent_close`. It stops admission,
   flushes accepted publications, rejects active evidence, closes every
   frontier at the highest observed timestamp, drains, verifies quiescence,
   and joins the consumer.

Per-producer sequence assignment and publication use the same producer mutex.
A producer delivery frontier takes that mutex and snapshots the sequence
cutoff, so it cannot overtake a native batch waiting to publish.

## Flat committed input

Each publish borrows one contiguous buffer only for the duration of the call.
The native adapter validates the complete buffer, reserves its exact chunk
charge, and copies keys/values into `ChunkBatchBuilder`. Once publish returns
success, the caller may immediately reuse or free the input bytes.

The buffer uses host ABI byte order and relative offsets; no pointer is stored
inside it:

| Region | Size | Rule |
| --- | ---: | --- |
| `co_checker_online_txn_input_header_v1` | 64 bytes | Starts at offset 0; `events_offset` is 64 |
| `co_checker_online_event_input_v1[event_count]` | 32 bytes/event | Strictly increasing event sequence |
| Key/value bytes | variable | Every nonempty offset points at or after the event array |

Read-only input has `commit_ts == 0` and orders at `start_ts`. Write input has a
nonzero `commit_ts > start_ts`. `Present(empty)` is encoded as `Present` with a
zero offset/size; `Absent` also has zero offset/size but a distinct value-state
tag. Writes use read-source zero. Snapshot and local reads use their respective
read-source constants; local reads are validated but omitted from the sealed
SOWW batch.

The read-only bit is the authoritative token kind, not a derivation from the
normalized SOWW write count. Therefore a write-path transaction whose internal
mutations are all filtered untouched-index markers remains valid with zero
normalized writes and orders at `commit_ts`. The converse is fatal: an input
declared read-only may not contain a normalized write.

## Errors, verdict, and metrics

All calls return a fixed `uint32_t` status. Invalid API arguments do not claim a
checker result. Malformed evidence, allocation/backpressure timeout, ordering
failure, external producer fatal, or incomplete shutdown becomes a separate
sticky error and permanently makes the verdict unavailable.

`error_get` and `metrics_json` use caller-owned output buffers. Query the
required size with a null/zero buffer; returned sizes include the trailing NUL.
Metrics cover capture budget/pool activity, queue and reorder pressure, lag,
ordered/checked transaction counts, retained engine state, and cumulative
parse/reserve/build/handoff/ordering/checking time in nanoseconds. These timers
can overlap because ordering and checking run on the consumer thread.
