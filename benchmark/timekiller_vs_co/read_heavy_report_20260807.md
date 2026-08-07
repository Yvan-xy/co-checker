# Matched-boundary read-heavy benchmark (2026-08-07)

## Result

With both internal timers excluding only input decoding and neutral trace-model
construction, `snapshot-so-ww` is faster than patched TimeKiller on every real
TPC-C trace measured here. The strongest new workload is a retained 180-second,
100% New-Order capture with 3,282,833 events and 45.77% reads: co-checker takes
1,568.431 ms versus TimeKiller's 1,931 ms median checker time. It also wins
end-to-end wall time (3.69 s versus 3.99 s) and peak RSS (1,729 versus 3,466 MiB).

High read percentage alone is not sufficient to guarantee this result. A
synthetic 4.98-million-event latest-value-read trace still favors TimeKiller's
compact `Long` representation inside the checker timer. The real TPC-C result
therefore should not be generalized to arbitrary read-only histories.

## Matched timing boundary

The pinned TimeKiller checkout was patched so `Checking time` starts after JSON
decoding and construction of its neutral transaction/operation graph, but
before all checker-specific work:

- explicit pre-check garbage collection;
- dense initial-state and per-key writer-index construction;
- history sorting;
- `checker.check(history)`.

Result printing remains outside the timer. This matches co-checker's boundary:
binary parsing and neutral `Trace` construction are excluded, while
snapshot/change-list construction, checking, and checker-local teardown are
included. The reproducible source patch is
`timekiller-post-parse-timing.patch`.

## Real TPC-C results

Each row is the median of three successful measured trials after one warm-up.
`TK/co` greater than one means co-checker is faster. Both tools returned pass.

| Workload | Read % | Events | co ms | TK ms | TK/co | co wall s | TK wall s | co/TK peak RSS MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Existing 1w/4th full mix, 360 s | 40.650 | 4,794,984 | 2,471.953 | 2,843 | 1.150 | 5.44 | 5.68 | 2,437 / 4,276 |
| NewOrder 100%, 180 s | 45.768 | 3,282,833 | 1,568.431 | 1,931 | 1.231 | 3.69 | 3.99 | 1,729 / 3,466 |
| Payment 100%, 60 s | 12.019 | 1,064,931 | 604.808 | 984 | 1.627 | 1.22 | 1.93 | 635 / 2,107 |
| StockLevel 100% pilot, 33.5 s | 10.049 | 798,882 | 518.304 | 691 | 1.333 | 0.93 | 1.61 | 477 / 1,790 |
| OrderStatus 100% pilot, 25.3 s | 0.392 | 721,620 | 478.467 | 684 | 1.430 | 0.87 | 1.39 | 449 / 2,036 |

The short specialized captures include the roughly 729,000-event prepare phase,
which dilutes their run-phase read ratios. Payment generates more surviving
point reads than StockLevel and OrderStatus, but New-Order is a much better
read-heavy target for this pipeline. Many analytical/range reads from
StockLevel and OrderStatus do not become one checker event per returned row.

The StockLevel and OrderStatus pilots were manually stopped after their stated
durations and are named `pilot-...-interrupted` so they cannot be mistaken for
complete 60-second runs. Their committed trace data is valid and retained.

## Synthetic stress results

The deterministic generator prepares non-null values and then creates ordered
read transactions. `compact` uses 8-byte keys and values; `wide` uses 48-byte
keys and 128-byte values. History stress traces insert 64 writer rounds between
per-reader snapshot anchors and stale reads.

| Workload | Read % | Events | co ms | TK ms | co wall s | TK wall s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Latest reads, compact | 93.446 | 999,936 | 143.677 | 213 | 0.37 | 1.02 |
| Latest reads, compact | 98.684 | 4,980,736 | 587.933 | 390 | 1.66 | 3.06 |
| Latest reads, wide | 98.684 | 4,980,736 | 1,047.135 | 384 | 3.52 | 3.06 |
| Stale reads, 64 history rounds | 79.554 | 321,792 | 315.417 | 245 | 0.38 | 0.66 |
| Stale reads, 64 history rounds | 97.494 | 2,625,792 | 3,253.553 | 1,761 | 3.83 | 3.21 |

These are useful negative controls. TimeKiller checks dense converted `Long`
IDs, whereas co-checker hashes and copies the original binary key/value payloads.
The wide-versus-compact pair quantifies that representation cost. The stale
history construction also makes co-checker's global rollback path scan many
prior committed read transactions, so it does not favor co-checker despite its
high read ratio.

## Retained inputs and raw results

Generated TPC-C artifacts are kept in the repository's ignored trace trees:

- checker-ready YAML: `traces/tpcc/`;
- raw instrumented TiDB YAML and server logs: `traces/tpcc/raw/`;
- binary checker traces: `bin_traces/tpcc/`;
- deterministic synthetic traces and stats: `bin_traces/read-heavy/`.

The principal new binary is
`bin_traces/tpcc/22s-44836t-3282833e-tpcc-optimistic-1w-4th-180s-weights-100-0-0-0-0.trace`.
It was generated with:

```text
WAREHOUSES=1 THREADS=4 RUN_TIME=180s \
TRANSACTION_WEIGHTS=100,0,0,0,0 ./trace-gen/generate_tpcc.sh
```

Machine-readable benchmark outputs and per-trial logs remain under:

- `/tmp/co-timekiller-bench/read-heavy/results/tpcc-neworder-180s-matched-20260807/`;
- `/tmp/co-timekiller-bench/read-heavy/results/tpcc-360s-matched-20260807/`;
- `/tmp/co-timekiller-bench/read-heavy/results/tpcc-weighted-matched-20260807/`;
- `/tmp/co-timekiller-bench/read-heavy/results/synthetic-matched-20260807/`;
- `/tmp/co-timekiller-bench/read-heavy/results/stale-matched-20260807/`.

No generated trace, converted JSON, or benchmark log is tracked by Git.
