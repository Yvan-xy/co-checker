# YCSB online-checker overhead profile, 2026-08-09

## Conclusion

The high A/B/C/D/F overhead is primarily a per-SQL-transaction cost amplified
by the serialized global-finalization protocol. It is not caused by result or
TiDB log writing, and the native checker queue is not backpressured.

Three effects compound for point operations:

1. Active capture intentionally disables TiDB's prepared point-get execution
   shortcut and its `MaxUint64` start-TS optimization. Each checked point
   statement instead uses the ordinary transaction lifecycle and a real TSO so
   exact snapshot evidence can be attached safely.
2. Capture, packing, CGo/native validation, and checker work increase TiDB CPU
   per logical operation by 69-77% for A/B/C/D/F. E increases by only 19%
   because its scan already has much more database work per operation.
3. Every physical transaction, including a read-only point SELECT, enters the
   single process-wide finalization coordinator. Read-only completion obtains a
   fresh completion TSO while holding the guard. Writes acquire the guard from
   the before-final-commit-TS hook and retain it through commit finalization.

For the headline 100,000-row run, C accumulated 10.65 waiter-thread-equivalents
at the global guard; three delayed goroutine snapshots independently found 10,
10, and 11 of the 16 client sessions stopped in
`txnCaptureFinalizationCoordinator.acquire`. E accumulated only 0.65. The full
per-workload decomposition is in `overhead_profile_20260809.csv`.

## Why E is different

Workload E is 95% range scans with a uniform length of 1-100 and 5% inserts.
It captured 47.37 events per published transaction, compared with 1.00 for C
and 1.05-1.50 for A/B/D/F. Its pristine database cost was 786.7 TiDB CPU
microseconds per operation, versus 118.5 for C. E therefore amortizes the
fixed transaction activation, real-TSO, finalization, and publication costs
over dozens of rows.

The CPU profiles agree:

- E baseline: 83.90 CPU-seconds in a 14.39-second profile. `runtime.memmove`
  was 15.29% flat; `ChunkDecoder.DecodeToChunk` and `Column.AppendBytes` were
  27.14% and 24.09% cumulative. The workload is dominated by scan row
  decoding/copying.
- E full: 102.29 CPU-seconds in 14.77 seconds. The checker shared library was
  10.54% flat and the server remained CPU-saturated at 692% during the profile.
  This additional work can overlap because clients rarely queue at the global
  guard, so headline throughput loss was 13.56%.
- C full: 47.17 CPU-seconds in 9.92 seconds. CGo, the checker library, and libc
  were 8.56%, 6.85%, and 7.29% flat. The lower 476% total CPU, despite a
  pristine 714% mean, is consistent with a lock convoy idling cores rather than
  a saturated native checker.

Profiles were collected in separate diagnostic attempts and are not used as
headline throughput samples.

## Workload-specific interpretation

- A: 50% updates. Write transactions hold the ordering guard across the final
  commit phase and conflict/retry work adds more physical attempts.
- B: only 5% updates, but the 95% reads also require real checked transactions
  and serialized completion; the guard is not write-only.
- C: pure point reads isolate the fixed transaction/fast-path/finalization
  cost. It falls from 60,243 to 25,731 operations/s.
- D: 95% latest-distribution point reads plus 5% inserts, so it has the same
  point-transaction behavior.
- E: one relatively expensive scan transaction returns many rows, amortizing
  the fixed cost.
- F: stock go-ycsb implements each logical RMW as a `Read` followed by an
  `Update`, not one multi-statement transaction. The measured ratio is 1.500
  physical publications per logical operation, amplifying the fixed cost.

## TPC-C comparison

The retained serialized-global-lock TPC-C result at
`/home/dyf/code/db-consistency/results/tpcc-global-lock-20260724-3x600s`
reports 7.12% median full-mode overhead across three trials (4.17-9.01%), not
17%. If 17% refers to a different TPC-C configuration, the transaction-shape
explanation is unchanged.

That saved TPC-C run executes about 1,423 successful NewOrder transactions/s
in full mode and captures about 63 events per accepted publication. In
contrast, full C finalizes 25,731 one-event transactions/s. TPC-C therefore
invokes the serialized per-transaction path roughly an order of magnitude less
often and amortizes it across a much larger transaction. Its median global-lock
wait and hold are individually longer (1.049 ms and 0.443 ms), but the useful
transaction itself is also much longer and the arrival rate is far lower.

## Excluded causes and next optimization targets

- All headline cells have zero queue-backpressure count/time, no sustained
  backlog, no residual tail, and small queue peaks. The checker consumer is not
  the point-workload throughput limiter.
- Diagnostic TiDB logs were 9.9 KiB per attempt and stdout was empty. The
  read-only C `pidstat` samples reported 0 KiB/s writes; E full was generally
  below 0.6 MiB/s. Log writing does not explain the measured loss.
- The first optimization target should be the point-read execution path: retain
  exact capture ownership while recovering as much of the prepared point-get
  shortcut as correctness permits.
- The second target should shorten the serialized critical path. A promising
  design is to assign/enqueue the authoritative order under the guard, release
  it, and wait for ordered native acceptance outside the guard. This requires a
  proof that database success cannot be returned after lost evidence and that
  publication order remains identical to authoritative commit/completion
  order; simply moving TSO or publication outside the guard is not safe.
- Retry-heavy A/F should be profiled separately after those fixed costs are
  reduced. Their intentionally optimistic checked replays are another real
  difference from pristine TiDB's pessimistic retry replay.

## Artifacts

Headline metrics:
`/home/dyf/code/db-consistency/ycsb-online-results/milestone7_baseline_full_fixed_100k_basic_20260809T193500Z`.

Compact CPU, perf, blocking-stack, diagnostic summary, and TPC-C reference
artifacts:
`/home/dyf/code/db-consistency/ycsb-online-results/ycsb_overhead_profile_20260809T200930Z`.
The directory is 5.2 MiB; `SHA256SUMS` has SHA-256
`b2e072b27d03d9546090d6abc80830749046e025ba4a93041444016f4d0f262e`.
The multi-gigabyte temporary stores and ordinary logs are pruned after this
finding is recorded in the living ExecPlan.
