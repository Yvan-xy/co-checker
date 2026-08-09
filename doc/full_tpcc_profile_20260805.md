# Full-mix TPC-C checker slowdown profile (2026-08-05)

## Conclusion

The severe slowdown is caused by the lifecycle of the native checker's
`RepeatedReadTable`. Commit ordering, history traversal, and duplicate-read
hashing are rejected as primary causes; rare full-mix types are not required
to reproduce the slowdown.

Deterministic preparation contains a 2,400,331-event transaction. The checker
uses the total transaction event count as the repeated-snapshot-read table's
capacity hint, grows the persistent table to 4,194,304 slots, and never shrinks
it. Every later transaction then scans the full retained capacity twice: once
from `reset()` and once from the end-of-transaction guard. Diagnostic counters
measure exactly 8,388,608 slot visits per checked transaction in every mix and
transaction shape.

The 60-second `perf` window attributes 97.85% of native consumer cycles to
`RepeatedReadTable::reset`, `RepeatedReadTable::clear`, and their `memset`.
The full-mix diagnostic measured 4.410 ms/check while visiting only 0.163
history entries per transaction on average. NewOrder-only measured 4.296
ms/check and visited no history at all. This explains the near-constant
per-transaction service time, the 2 GiB queue saturation, and the multi-minute
drain.

No optimization was implemented. The only code changes made for this profile
are isolated, opt-in diagnostic counters and benchmark tooling, with focused
tests. Checker semantics and packed V1 evidence format are unchanged.

## Safety and provenance

- Read the checker and TiDB `AGENTS.md` files and `doc/full_tpcc_plan.md` in
  full before acting.
- Inspected status and all existing diffs in both dirty worktrees. Existing
  edits and untracked files were preserved; no reset, clean, switch, commit, or
  overwrite was performed.
- The requested pristine path did not exist. A clean detached worktree was
  created at `/tmp/tidb-online-pristine-profile-20260805-u1000` from pinned
  commit `b94006d3abffd428634ecc5c499f4bcb7b4d06d1`.
- The named historical and active-result roots were absent. No benchmark was
  running before profiling. All partial and rejected attempts below were
  preserved.
- The first duplicate-diagnostic root,
  `/tmp/co-checker-full-tpcc-diagnostics-20260805-mix-w8-s8192`, is preserved.
  It exposed that the opt-in diagnostic gate leaked into bootstrap while the
  checker mode was unset; the runner now scopes the gate to the checked server.
- Checker source commit: `18d95e5513f2835f7973ff204b6fac30e88d0a05`.
- Instrumented TiDB source commit:
  `a570cb08e95e5fd544b669eed508c54255b2d7ac`.
- The primary decomposition used native library SHA-256
  `75631bdaa659f661de2e6f13d43e38ecda8aeb4d6783d01ca627d9b1e5e9af13`,
  exactly matching the historical result named in the task. Its DSO build ID
  is `aa0f14064d956f7c7e39b249c75984bbbefa8f53`.
- Diagnostic reruns necessarily use a counter-instrumented library (SHA-256
  `d73ffa5be27cf397a2692dda6a73db83046439f4a29484d86f21a8c56c6b97cd`).
  Full mix and NewOrder diagnostics have identical source fingerprints,
  build settings, preparation manifests, and database digests. Independently
  rebuilt TiDB binaries differ only in embedded build timestamps/Go build IDs,
  so their binary SHA-256 values are not identical.

Exact dirty-tree/build provenance is frozen in:

- `/tmp/co-checker-full-tpcc-profile-20260805-r2-s8192/artifacts/build-attempt-001/manifest.json`
- `/tmp/co-checker-full-tpcc-diagnostics-20260805-mix-w8-v2-s8192/artifacts/build-attempt-001/manifest.json`

| Frozen identity | Primary profile | Counter diagnostics |
|---|---|---|
| Checker diff SHA | `9df21d7b89a771841147ab3eae2f2aaf8b706e71176e8e0b4beb67f3b6f51893` | `cf943965c8acd727d33b08f52d8f2f958a8f85ec8567649cb7cc21685cecc0aa` |
| Checker source-state SHA | `a2615ac4cf2059abe1a8685773f449285b2077a4fd8f0947f83276ac63a98d7c` | `6647e9e5ddb36e91883b76ad216260c3d71761d4a6e1489eeccb3caf8314d174` |
| TiDB diff SHA | `bb12095c242a6b3ef942c9f11d7c8ad0f055e255371554a3711625be794299a3` | `574b8f1ce47a35caa9c6648e649637c99d70f1c6f33609fb18e7fa6fa7e19a58` |
| TiDB source-state SHA | `23f211b81e722c29406448a21f1834c257765f93cb85aa7902ea1e654ccd00cf` | `ace85a48f2fbaea8b0dabfc82178123721d903b656ffe35c240a5a55624176b8` |
| Frozen runner SHA | `41526cbff0072854469d18cd2f81246d58e9b52d39c06a189c8bc35544749b68` | `0aa07a12f114e5cc23d24aa3847f7d50db5a5794fa48fcc40c4c3afd2fedeba3` |
| Frozen analyzer SHA | `b2c0afe0ca2ab4a4ad1d785d8159d8eb9cd94e2cb4ada403a8945932fa4ae837` | `067a1bb9f8ae6b4ac045e9d6d5bcf4bdd45968d52880aeb80dcc29cd3c6aea28` |
| Native DSO SHA | `75631bdaa659f661de2e6f13d43e38ecda8aeb4d6783d01ca627d9b1e5e9af13` | `d73ffa5be27cf397a2692dda6a73db83046439f4a29484d86f21a8c56c6b97cd` |
| Instrumented TiDB binary SHA | `c1db19641fe22adf8dbf4d574bae57678ba5a8189ee32af2ebb945712df4898b` | `6698f0ed5fbdee8c36ed5ae171f43b5cba025ae3da68e4427535b6715e2ff0a7` |
| Pristine TiDB binary SHA | `285f63cb2c7c058cea1a0b3af2b7d84f9ed40310d6958bcd3666f870a5f9a6a7` | `15f9e29efa8199706da07a597c2555ffebb45dc62824e035e3bd38f4f863cbee` |

The command blocks below record the exact argv/environment used at execution
time. The live runner has since acquired the counter-diagnostic changes and is
not byte-identical to the primary frozen runner. Reconstruct the primary build
from its manifest, frozen patches/untracked snapshots, build settings, patched
go-tpc, and recorded binaries rather than assuming the current script alone is
sufficient.

## Primary controlled reproduction

The first root,
`/tmp/co-checker-full-tpcc-profile-20260805-r1-s8192`, is an intentionally
preserved partial run whose smoke warm-up validation was too short. The valid
smoke and complete first trial are in:

```text
/tmp/co-checker-full-tpcc-profile-20260805-r2-s8192
```

Exact runner command:

```sh
env CHUNK_SEGMENT_BYTES=8192 \
  SMOKE_WARMUP_TIME=2s SMOKE_RUN_TIME=5s \
  SMOKE_STABILIZE_SECONDS=1 \
  online/scripts/run_tpcc_global_lock_comparison.sh \
  --trials 5 --warmup-time 20s --run-time 120s \
  --warehouses 8 --threads 16 --prepare-threads 1 \
  --output-interval 10s --stabilize-seconds 3 \
  --result-root /tmp/co-checker-full-tpcc-profile-20260805-r2-s8192 \
  --tidb /home/dyf/code/db-consistency/tidb-online \
  --go-tpc /home/dyf/go/bin/go-tpc \
  --baseline-worktree /tmp/tidb-online-pristine-profile-20260805-u1000 \
  --tidb-cpus 0-7 --client-cpus 8-15 --build-jobs 4 \
  --checker-drain-timeout 30m
```

The smoke suite and every mode in trial 1 completed with `fatal=0`, advancing
pass verdicts, clean terminal drains, and passing post-checks. Trial 2 had only
started the pristine baseline when it was cleanly stopped; its partial
artifacts remain in place. No process from the run remains active.

### Historical comparison

The historical values below are the task-provided audit result; its named root
was no longer present. The current values are measured in the preserved roots
listed in this report.

| Metric | Historical NewOrder only | Current primary full mix | Current fresh NewOrder diagnostic |
|---|---:|---:|---:|
| Average check | about 101 us/tx | 5,273 us/tx, 52.2x historical | 4,296 us/tx, 42.5x historical |
| Check/event | about 1.28 us | 53.423 us, 41.7x historical | 74.288 us |
| Queue | about 250 tx, 6-8 MiB | 45,643 tx at cutoff, 2 GiB | cutoff telemetry invalid; terminal drain complete |
| Backpressure | zero | 1,280.096 s aggregated | queue tail present |
| Completion tail | about 1.3 s | 236.603 s | 72.736 s |
| Fixed-window overhead | about 6.13% | 68.05% | raw 15.95%, suite invalid only on telemetry gates |

The same transaction type now costs over 42x the historical average when it
uses the current deterministic preparation. This isolates the regression from
the 45/43/4/4/4 workload mix itself.

The 42.5x NewOrder factor comes from the counter-instrumented diagnostic DSO;
the binary-identical historical DSO identity is established by the primary
full-mix reproduction. The diagnostic counters do not change checker semantics,
but this distinction is retained in the provenance table.

### Mode decomposition

All values are trial 1. Rates shown are go-tpc tpmC (NewOrder/minute).

| Mode | NewOrder count | go-tpc tpmC | Main diagnostic evidence |
|---|---:|---:|---|
| Pristine baseline | 99,210 | 49,590.2 | 120.092 s |
| Capture only | 84,684 | 42,307.9 | 177,046 accepted publications; 15,097,957 accepted events; no backlog |
| Serialized order only | 85,075 | 42,517.8 | 177,720 publications; ordering 1.154 s total, 6.49 us/publication; no backlog |
| Full checker | 32,085 | 15,844.2 | 67,667 measured publications; 6,679,280 events; 356.830 s checking |

Capture alone costs about 14.7% fixed-window throughput, but capture and order
are nearly identical in this trial and neither builds a queue. The severe
loss appears only when `SnapshotSOWWEngine` checking is enabled.

Capture-only warm-up-subtracted costs:

- 5,618,181,296 flat bytes.
- Packing: 1.905722873 s, or 10.76 us/accepted publication.
- Native publish: 4.258759461 s.
- Fragment acceptance: 0.430348384 s.
- No capture or checker backpressure.

Full-checker warm-up-subtracted costs:

- 67,667 accepted/checked publications and 6,679,280 accepted/checked events
  (98.708 events/publication).
- Checking: 356.830397476 s, or 5.273 ms/publication and 53.423 us/event.
- Ordering: 359.112584 ms, or 5.31 us/publication.
- Parse/reserve/build: 88.922254 ms / 15.856736 ms / 2.667851613 s.
- Packing: 944.330078 ms.
- Fragment acceptance: 198.158017 ms for 4,202,633 events and
  1,753,761,305 payload bytes.
- Aggregated native handoff time: 1,280.300534421 s, of which
  1,280.096107216 s is backpressure accumulated across blocked producer
  goroutines. This is a consequence of the consumer service deficit.

Counter-instrumented fresh-store phase costs further separate producer and
native work:

| Phase, per accepted publication | Full mix | NewOrder only |
|---|---:|---:|
| Producer packing | 16.212 us | 5.950 us |
| Native publish | 73.187 us | 38.219 us |
| Native parse | 1.418 us | 0.877 us |
| Native reserve | 0.212 us | 0.224 us |
| Native batch build | 61.333 us | 26.718 us |
| Native handoff | 2.450 us | 2.579 us |
| Commit ordering | 4.229 us | 4.161 us |

There is no independent clone or fragment-attachment timer. Capture-only
throughput bounds their combined cost; packing and bulk fragment acceptance
are timed directly, while raw/captured/flat byte counters quantify their volume.

### Queue formation and tail

| Workload time | Accepted | Checked | Queue depth | Queue bytes | Average check |
|---:|---:|---:|---:|---:|---:|
| 0 s | 0 | 0 | 0 | 0 | - |
| 10.5 s | 14,170 | 1,826 | 12,293 | 662 MiB | 5.657 ms |
| 20 s | 26,959 | 3,587 | 23,362 | 1,280 MiB | - |
| 29.6 s | - | - | 34,808 | 1,895 MiB | - |
| about 40 s | - | - | 37,946 | 2,048 MiB | - |
| 120 s | 67,429 | 21,629 | 45,798 | 2,048 MiB | 5.483 ms |

At the exact cutoff, 45,643 transactions and 4,266,300 events remained. The
queue required 236.271938 s to drain. Workload start through checker completion
was 358.175292 s, producing an end-to-verdict rate of 5,374.74 NewOrder/minute.
Increasing the queue would only postpone backpressure; it cannot correct the
consumer service rate.

### Client transaction types

| Type | Successes | Client p99 |
|---|---:|---:|
| Delivery | 2,815 | 419.4 ms |
| NewOrder | 32,085 | 100.7 ms |
| OrderStatus | 3,141 | 6,710.9 ms |
| Payment | 26,861 | 37.7 ms |
| StockLevel | 3,134 | 1,275.1 ms |

The high client latencies occur while finalization is backpressured. The
primary slowest-check gauge was 1.497113010 s before the workload and did not
change during it; that DSO records the 2,400,331-event maximum but not its
identity. Counter-instrumented reruns independently identify the unchanged
preparation maximum as commit sequence 4103, corroborating this attribution.

## Native CPU profile

Profile root and exact data path:

```text
/tmp/co-checker-full-tpcc-profile-20260805-r2-s8192-perf/
  trials/trial-001/serialized-full/attempt-001/
    native-consumer-workload.perf.data
```

The exact command embedded in the perf header was:

```sh
/usr/lib/linux-tools-6.8.0-136/perf record \
  -o /tmp/co-checker-full-tpcc-profile-20260805-r2-s8192-perf/trials/trial-001/serialized-full/attempt-001/native-consumer-workload.perf.data \
  -e cycles:u -F 99 --call-graph dwarf,8192 --mmap-pages 1024 \
  --no-buildid-cache -T -p 58731 -- sleep 60
```

The gate waited 30 seconds after workload start. It selected TiDB PID 58731
and checker consumer TID 58750. The recording contains 15,543 samples, zero
lost samples, and exactly 60.006392 seconds from first to last sample.

Whole-process flat CPU:

| Symbol | All TiDB process cycles |
|---|---:|
| `RepeatedReadTable::reset` | 26.87% |
| `RepeatedReadTable::clear` | 18.24% |
| `__memset_avx2_unaligned_erms` | 4.55% |
| Sum | 49.66% |

Consumer-thread-normalized CPU:

| Symbol | Checker consumer cycles |
|---|---:|
| `RepeatedReadTable::reset` | 52.93% |
| `RepeatedReadTable::clear` | 35.93% |
| `__memset_avx2_unaligned_erms` | 8.99% |
| Sum | 97.85% |
| `memcmp` | 0.47% |
| repeated-read hash | 0.34% |
| repeated-read insert | 0.34% |
| other `consume_range` | 0.37% |
| `has_compatible_snapshot` | about 0.07% |

The call graph is:

```text
consumer_loop
  -> consume_ordered
    -> SnapshotSOWWEngine::consume
      -> consume_range
        -> RepeatedReadTable::reset / clear / memset
```

Profile artifacts:

- `perf-gate.txt`: selected PIDs, event, delay, and duration.
- `perf-header.txt`: exact command, host, samples, and loss count.
- `process-flat-no-callchain.txt`: whole-process flat report.
- `consumer-flat-relative.csv`: consumer-normalized flat report.
- `consumer.perf-script`: consumer call chains.
- `consumer.folded`: folded stacks.
- `consumer.svg`: flame graph.
- `perf-buildids.txt`: build identities.

Artifact SHA-256 values:

- `native-consumer-workload.perf.data`:
  `6da4a7399d2b1e9aae13d8224a632e4902cf934eddc3ab9d0658d7a7153f4188`
- `consumer.folded`:
  `5db53a9dd93132fb9e91db5eae118ef56c531d51fc4c99326d7b7b40c57601a4`
- `consumer.svg`:
  `e10a57a5f70e09c3e07ffe07c7213228be168ba77386acc0c39c59b8e55474bb`

FlameGraph was checked out at
`/tmp/co-checker-profile-tools-20260805/FlameGraph`, commit
`41fee1f99f9276008b7cd112fca19dc3ea84ac32`.

The original shell argv for post-processing were not retained in the profile
root, so they cannot be certified after the fact. To make this deliverable
reproducible, the immutable `perf.data` was reprocessed again with the following
exact commands:

```sh
profile_data=/tmp/co-checker-full-tpcc-profile-20260805-r2-s8192-perf/trials/trial-001/serialized-full/attempt-001/native-consumer-workload.perf.data
profile_out=/tmp/co-checker-full-tpcc-profile-20260805-r2-s8192-perf/reprocessed-exact-20260805
flame_dir=/tmp/co-checker-profile-tools-20260805/FlameGraph
mkdir -p "$profile_out"
perf report -i "$profile_data" --stdio --no-children --call-graph none \
  --percent-limit 0 --sort pid,comm,dso,symbol \
  > "$profile_out/process-flat-no-callchain.txt"
perf report -i "$profile_data" --stdio --no-children --call-graph none \
  --percentage relative --tid 58750 --percent-limit 0 \
  --sort dso,symbol --field-separator ";" \
  > "$profile_out/consumer-flat-relative.csv"
perf script -i "$profile_data" --tid 58750 \
  > "$profile_out/consumer.perf-script"
"$flame_dir/stackcollapse-perf.pl" "$profile_out/consumer.perf-script" \
  > "$profile_out/consumer.folded"
"$flame_dir/flamegraph.pl" \
  --title "TPC-C full mix: native checker consumer" \
  --subtitle "120s workload, cycles:u at 99 Hz, consumer TID 58750" \
  "$profile_out/consumer.folded" > "$profile_out/consumer.svg"
```

Reprocessed artifact SHA-256 values:

| Artifact | SHA-256 |
|---|---|
| `process-flat-no-callchain.txt` | `6e6dc210a0c32f2eecb6333f094b7975940dd06445eecd92b1d5f36251c038b5` |
| `consumer-flat-relative.csv` | `8beabd02728f99701110e017e1ead403e44c22a55953ae51609e30ddfc02966a` |
| `consumer.perf-script` | `c228dc98132ff836add10736117f7be2184a40cf3d15a3ec13c516d40d50dda8` |
| `consumer.folded` | `5db53a9dd93132fb9e91db5eae118ef56c531d51fc4c99326d7b7b40c57601a4` |
| `consumer.svg` | `07c337141d89fade3c1c7a424efc8aa5effcc20a824e682d02d78c6fa4e92e7e` |

The reprocessed consumer CSV, perf-script, and folded stacks are byte-identical
to the original artifacts. The new SVG is a separately hashed
deliverable from the exact command above.

## Confirmed mechanism

The source path is:

- `online/src/engine.cpp:339-346`: `consume_range` reserves temporary maps and
  passes the transaction's total `event_count` to `repeated_reads_.reset()`.
- `online/src/repeated_read_table.cpp:204`: `reset()` calls `clear()` before it
  checks whether capacity must grow. There is no shrink path.
- `online/src/repeated_read_table.cpp:215`: `clear()` scans every retained slot
  and fills the complete control allocation.
- `online/src/engine.cpp:348`: `BorrowedReadGuard` clears the same table again
  at transaction exit.
- `online/src/engine.cpp:819`: the table is an engine
  member, so capacity persists across transactions.

For the preparation outlier:

```text
event_count hint                       2,400,331
minimum slots at configured load       2,743,236
next power-of-two table capacity       4,194,304
two full clears per later transaction  8,388,608 slot visits
```

The main 120-second measured phase checked 67,667 transactions. Source
behavior plus the exact diagnostic invariant imply 567,631,937,536 slot visits
during that measured phase. Counting only one control-byte read and one
control-byte write per visit gives about 1.135 TB of logical control-byte
accesses. This is not a measured DRAM or bus-traffic value; cache residency and additional occupied-slot writes
were not measured.

Counter evidence from every diagnostic workload exactly satisfies:

```text
clear_slot_visits = checked_transactions * 2 * 4,194,304
clear_calls       = checked_transactions * 2
reset_calls       = checked_transactions
```

## Fresh-store full mix versus NewOrder-only

Both diagnostics use 8 warehouses, deterministic preparation, the same source
and build settings, fresh stores, 5-second warm-up, 30-second workload, four
clients, and `CHUNK_SEGMENT_BYTES=8192`.

Full mix command:

```sh
env TIDB_ONLINE_CHECKER_DUPLICATE_READ_DIAGNOSTICS=1 \
  CHUNK_SEGMENT_BYTES=8192 SMOKE_WARMUP_TIME=5s \
  SMOKE_RUN_TIME=30s SMOKE_OUTPUT_INTERVAL=2s \
  SMOKE_STABILIZE_SECONDS=1 SMOKE_THREADS=4 SMOKE_WAREHOUSES=8 \
  online/scripts/run_tpcc_global_lock_comparison.sh \
  --smoke --baseline-full-only --warehouses 8 --threads 4 \
  --prepare-threads 1 \
  --result-root /tmp/co-checker-full-tpcc-diagnostics-20260805-mix-w8-v2-s8192 \
  --tidb /home/dyf/code/db-consistency/tidb-online \
  --go-tpc /home/dyf/go/bin/go-tpc \
  --baseline-worktree /tmp/tidb-online-pristine-profile-20260805-u1000 \
  --tidb-cpus 0-7 --client-cpus 8-15 --build-jobs 4 \
  --checker-drain-timeout 30m
```

NewOrder uses the same command with:

```sh
--transaction-weights 100,0,0,0,0 \
--result-root /tmp/co-checker-full-tpcc-diagnostics-20260805-neworder-w8-v2-s8192
```

For each selected attempt 2, the exact command above was rerun with `--resume`;
the runner skipped the already-valid pristine baseline and preserved attempt 1.

Selected full attempts:

- Full mix:
  `/tmp/co-checker-full-tpcc-diagnostics-20260805-mix-w8-v2-s8192/trials/trial-001/serialized-full/attempt-002`.
  The complete two-mode suite is valid. Attempt 1 is preserved but superseded
  because coverage was 93.548% and cutoff delay was 1.071508465 s.
- NewOrder:
  `/tmp/co-checker-full-tpcc-diagnostics-20260805-neworder-w8-v2-s8192/trials/trial-001/serialized-full/attempt-002`.
  Workload, exact terminal drain, verdict/close, post-check, and server-log
  validation all pass. The analyzer still marks this attempt invalid because
  sampling coverage was 90.3226% and cutoff observation delay was
  1.543249541 s. Its warm-up-to-terminal counters are complete, but its exact
  cutoff/tail telemetry must not be treated as valid. Attempt 1 failed during
  cutoff scrape and is preserved.

Warm-up-subtracted comparison:

| Metric | Full mix | NewOrder only |
|---|---:|---:|
| Accepted publications | 25,661 | 23,859 |
| Accepted events | 3,732,726 | 1,379,827 |
| Events/publication | 145.463 | 57.833 |
| Checking time | 113.165451230 s | 102.504183324 s |
| Average check | 4.410017 ms | 4.296248 ms |
| Check time/event | 30.317 us | 74.288 us |
| Histogram p50 upper bound | 4.719 ms | 4.719 ms |
| Histogram p95 upper bound | 5.243 ms | 4.719 ms |
| Histogram p99 upper bound | 6.816 ms | 4.719 ms |
| History entries visited/tx | 0.163 | 0 |
| History deltas visited/tx | 2.448 | 0 |
| Clear slot visits/tx | 8,388,608 | 8,388,608 |

The maximum checks were 1.473 s (mix) and 1.487 s (NewOrder), both the same
2,400,331-event preparation transaction. The gauges did not increase in
warm-up or measured workload.

Throughput:

- Full mix valid baseline/full fixed rates: 31,028.498 / 23,593.916
  NewOrder/minute. Fixed overhead 23.96%; end-to-verdict overhead 79.87%;
  drain 82.841742 s.
- NewOrder baseline/full fixed rates: 57,338.466 / 48,190.508
  NewOrder/minute. The raw ratio is 15.95% fixed overhead; end-to-verdict is
  75.44%. These are diagnostic values, not a valid suite result, because of
  the sampling caveat above. Terminal drain took 70.976479 s.

The near-identical per-transaction checker time across a 2.5x event-count
difference proves that transaction-mix event volume is secondary to the fixed
retained-table clearing work. It also reproduces the slowdown in NewOrder-only,
so full-mix-only SQL operators are not required to trigger it.

## Duplicate-read audit

All counters include accepted, successfully published transactions only.
Snapshot and local reads are separate. An identical duplicate requires equal
presence state and, for present values, byte-equal payload. Present-empty and
absent remain distinct. State/value conflicts are counted separately.

| Metric | Full mix | NewOrder only |
|---|---:|---:|
| Snapshot reads | 3,252,621 | 808,809 |
| Unique snapshot keys | 2,997,013 | 547,159 |
| Identical duplicates | 255,608 | 261,650 |
| Conflicting duplicates | 0 | 0 |
| Duplicate/read ratio | 7.8585% | 32.3500% |
| Snapshot packed bytes | 1,412,531,371 | 246,913,801 |
| Duplicate marginal packed bytes | 70,742,631 | 97,784,384 |
| Duplicate/snapshot-byte ratio | 5.0082% | 39.6026% |
| Duplicate/all-flat-byte ratio | 4.510% | 24.356% |
| Direct snapshot reads | 473,406 | 808,809 |
| Coprocessor snapshot reads | 2,779,215 | 0 |
| Local reads, separately counted | 240,556 | 0 |
| Writes / final unique writes | 480,105 / 480,105 | 571,018 / 571,018 |

Full-mix duplicate paths:

| First/repeat path | Count | Share |
|---|---:|---:|
| Direct/direct | 140,022 | 54.78% |
| Direct/coprocessor | 10,470 | 4.10% |
| Same coprocessor fragment | 0 | 0% |
| Cross coprocessor fragment | 105,116 | 41.12% |

NewOrder duplicates are all direct/direct. The producer and native checker
counts and exact marginal byte totals agree. No duplicate final writes or
conflicting repeated reads were observed.

The diagnostic can attribute direct versus coprocessor reads and same/cross
RPC fragment relationships. Existing evidence does not retain executor
operator, logical execution path, or physical storage-page identity, so those
cannot be recovered without extending capture metadata. No packed-format
change is justified by the current result.

### Transaction-type diagnostics

Short 99%-weight fresh-store runs were used for directional attribution. Each
still carries 1% NewOrder because the runner deliberately requires positive
NewOrder weight. Values below are exact aggregate window counters, not a claim
that individual events can be tagged with a TPC-C type.

The exact Payment command was:

```sh
env TIDB_ONLINE_CHECKER_DUPLICATE_READ_DIAGNOSTICS=1 \
  CHUNK_SEGMENT_BYTES=8192 SMOKE_WARMUP_TIME=2s SMOKE_RUN_TIME=10s \
  SMOKE_OUTPUT_INTERVAL=1s SMOKE_STABILIZE_SECONDS=1 \
  SMOKE_THREADS=2 SMOKE_WAREHOUSES=8 \
  online/scripts/run_tpcc_global_lock_comparison.sh --smoke \
  --baseline-full-only --transaction-weights 1,99,0,0,0 \
  --warehouses 8 --threads 2 --prepare-threads 1 \
  --result-root /tmp/co-checker-full-tpcc-diagnostics-20260805-payment99-w8-s8192 \
  --tidb /home/dyf/code/db-consistency/tidb-online \
  --go-tpc /home/dyf/go/bin/go-tpc \
  --baseline-worktree /tmp/tidb-online-pristine-profile-20260805-u1000 \
  --tidb-cpus 0-7 --client-cpus 8-15 --build-jobs 4 \
  --checker-drain-timeout 30m
```

The other exact invocations used the same command tail and these complete
environment/argument substitutions:

```text
OrderStatus:
  SMOKE_WARMUP_TIME=1s SMOKE_RUN_TIME=5s SMOKE_OUTPUT_INTERVAL=1s
  SMOKE_STABILIZE_SECONDS=1 SMOKE_THREADS=1 SMOKE_WAREHOUSES=8
  --transaction-weights 1,0,99,0,0 --warehouses 8 --threads 1
  --prepare-threads 1
  --result-root /tmp/co-checker-full-tpcc-diagnostics-20260805-orderstatus99-w8-s8192

Delivery:
  SMOKE_WARMUP_TIME=5s SMOKE_RUN_TIME=5s SMOKE_OUTPUT_INTERVAL=1s
  SMOKE_STABILIZE_SECONDS=1 SMOKE_THREADS=1 SMOKE_WAREHOUSES=8
  --transaction-weights 1,0,0,99,0 --warehouses 8 --threads 1
  --prepare-threads 1
  --result-root /tmp/co-checker-full-tpcc-diagnostics-20260805-delivery99-w8-s8192

StockLevel:
  SMOKE_WARMUP_TIME=5s SMOKE_RUN_TIME=5s SMOKE_OUTPUT_INTERVAL=1s
  SMOKE_STABILIZE_SECONDS=1 SMOKE_THREADS=1 SMOKE_WAREHOUSES=8
  --transaction-weights 1,0,0,0,99 --warehouses 8 --threads 1
  --prepare-threads 1
  --result-root /tmp/co-checker-full-tpcc-diagnostics-20260805-stocklevel99-w8-s8192
```

All three retained `TIDB_ONLINE_CHECKER_DUPLICATE_READ_DIAGNOSTICS=1`,
`CHUNK_SEGMENT_BYTES=8192`, the same TiDB/go-tpc/pristine paths, CPU sets,
build jobs, and 30-minute drain timeout shown in the Payment command.

Counter boundaries are explicit: NewOrder, Payment, Delivery, and StockLevel
subtract `warmup-drain.json` from `workload-drain.json`. The diagnostic-only
OrderStatus row subtracts `preparation-drain.json` from clean-close
`checker-result.json` (`adapter_metrics` and `native_metrics_before_close`).

| Window | Successful type counts | Native events/tx | Snapshot reads/tx | Duplicate ratio | Duplicate/snapshot bytes | Local reads/tx | Check ms/tx | History entries/deltas visited per tx |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| NewOrder exact | 23,859 NO | 57.83 | 33.90 | 32.350% | 39.603% | 0 | 4.296 | 0 / 0 |
| Payment-heavy | 8,067 Payment + 87 NO | 11.99 | 5.80 | 20.714% | 37.803% | 1.98 | 4.276 | 0.0038 / 0.0228 |
| OrderStatus-heavy | 56 OS + 1 NO | 5,171.14 | 5,170.58 | 0.0051% | 0.0056% | 0 | 7.653 | 0 / 0 |
| Delivery-heavy | 516 Delivery + 6 NO | 364.19 | 236.14 | 45.781% | 39.297% | 206.04 | 4.696 | 0 / 0 |
| StockLevel-heavy | 1,349 Stock + 14 NO | 397.67 | 397.42 | 0.0294% | 0.0416% | 0 | 4.814 | 0 / 0 |

Roots and validity:

- Payment:
  `/tmp/co-checker-full-tpcc-diagnostics-20260805-payment99-w8-s8192`.
  The workload/checker/post-check/server-log all pass, but the analyzer marks
  it invalid for 81.818% monitoring coverage and 1.811526702 s cutoff delay.
  Warm-up-to-terminal counters and the 22.887013058 s exact drain are complete.
- OrderStatus:
  `/tmp/co-checker-full-tpcc-diagnostics-20260805-orderstatus99-w8-s8192`.
  Diagnostic-only preparation-to-clean-close delta. The runner stopped after
  the one-second warm-up because it emitted no periodic NewOrder interval,
  although the command and checker passed. No measured workload or post-check
  exists; do not use it as a throughput result.
- Delivery:
  `/tmp/co-checker-full-tpcc-diagnostics-20260805-delivery99-w8-s8192`.
  Workload command and both drains pass, with 100% monitoring coverage and
  zero cutoff residual. Workload validation failed only because it saw three
  rather than four periodic NewOrder intervals; the runner then stopped before
  terminal checker-result/post-check/server-log validation. Treat it only as a
  phase-counter diagnostic, not a complete attempt or throughput result.
- StockLevel:
  `/tmp/co-checker-full-tpcc-diagnostics-20260805-stocklevel99-w8-s8192`.
  Complete valid two-mode suite; the full checker drained in 0.852941 s.

Delivery's 56,432 duplicates comprise 51,196 cross-coprocessor-fragment
(90.72%), 5,160 direct/coprocessor (9.14%), 76 direct/direct, and zero
same-fragment repeats. Payment's 9,792 duplicates are all direct/direct. The
OrderStatus-heavy and StockLevel-heavy aggregate duplicate ratios are 0.0051%
and 0.0294%; their 15 and 159 aggregate repeats are all direct/direct.
Because events are not type-tagged, those few repeats cannot be attributed to a
TPC-C type after the fact.

The duplicate-diagnostic `local_reads` counter covers successfully finalized
publications only. Local reads are serialized into the flat C input, then
dropped during native normalization before the sealed checker batch. In
these windows, where no duplicate writes were observed, native accepted events conserve as `snapshot reads + final unique writes`. The
producer `captured_events` counter is broader and also includes local reads and
prepared attempts later aborted. Local reads are not part of the duplicate-read
denominator or duplicate path classes.

OrderStatus is a genuinely large aggregate shape at about 5,171 native events
per transaction and costs about 7.65 ms instead of 4-5 ms. No type-window run
increased the preparation-time maximum, and none shows costly history
traversal. These aggregate, partly diagnostic-only windows show no evidence that
rare types are the principal queue-saturation mechanism, but they do not rule
out every individual rare-type outlier.

## History growth and compaction assessment

The primary run's live samples show stable cost while history grows:

| Workload interval | History entries | Average check |
|---|---:|---:|
| 0-29.6 s | 26,128 -> 30,940 | 5.530 ms |
| 29.6-60.4 s | 30,940 -> 36,000 | 5.470 ms |
| 60.4-90.0 s | 36,000 -> 40,820 | 5.497 ms |
| 90.0-119.6 s | 40,820 -> 45,708 | 5.437 ms |

The live sample schema stores cumulative checking time/count and current history
size, but not individual check durations. Therefore percentiles and maxima
cannot be conditioned on these history-size buckets after the fact. The table
reports bucket averages; the global diagnostic p50/p95/p99 bounds and unchanged
preparation maximum are reported above. This is an explicit telemetry limit,
not an inferred zero tail.

The diagnostic full mix visited 4,174 history entries, 62,822 deltas, and
1,793 relevant deltas across 25,661 checks: 0.163, 2.448, and 0.070 per check.
Maxima were 108 entries and 2,173 deltas. `has_compatible_snapshot` consumes
about 0.07% of consumer cycles. NewOrder, OrderStatus, Delivery, and StockLevel
diagnostics visited no history at all.

History compaction therefore cannot materially improve the current CPU
backlog. It remains a long-duration memory concern: the primary terminal state
retained 87,520 history entries, 6,808,650 deltas, and 1,187,228,617 bytes.
Any bounded-history design must preserve snapshot lower-bound and compatibility
semantics, so its correctness risk is high and it should be evaluated as a
separate change.

## Early same-key compaction assessment

Early identical-read compaction would reduce full-mix flat evidence bytes by
only 4.51% and snapshot bytes by 5.01%. It would reduce NewOrder and Delivery
bytes more substantially, but both still pay the same 8,388,608 slot visits per
check. The equal 4.3-4.4 ms NewOrder/full-mix service time despite very different
duplicate rates is direct evidence that compaction would not eliminate the
severe backlog.

Compaction may be a useful follow-up for capture bandwidth after the table
lifecycle is fixed. It must:

- collapse only identical snapshot observations;
- keep local reads separate;
- preserve explicit absent versus present-empty state;
- retain the first observation and enough conflicting observations/event
  sequence to report `InconsistentRepeatedRead` unchanged;
- preserve direct/coprocessor and fragment provenance if diagnostics require
  it.

There is no evidence that the packed format must change.

## Rejected hypotheses

| Hypothesis | Evidence against it |
|---|---|
| Capture/cloning/fragment packing is the severe cause | Capture-only is about 14.7% below baseline, has no backlog, and runs near order-only. Packing and fragment timing are microseconds per accepted transaction in diagnostics. |
| Serialized commit ordering is the cause | Order-only has 6.49 us/publication ordering cost and no queue; full diagnostics are about 4.2 us/publication. |
| Duplicate hashing/comparison is the cause | Hash and insert are each 0.34% of consumer CPU; key comparisons are essentially the duplicate count (full mix: 255,612 versus 255,608 identical duplicates). NewOrder has 32.35% duplicates and mix 7.86%, yet check times are nearly equal. |
| History traversal grows until the checker collapses | Cost is flat as history grows; traversal is 0.07% CPU and 0-0.163 entries/check in diagnostics. |
| Rare Delivery/Stock/OrderStatus/Payment transactions are the principal cause | Rare types are not required: exact NewOrder reproduces 4.296 ms checks. Aggregate type windows are 4.28-7.65 ms and never raise the preparation max. These runs do not exclude every individual outlier. |
| The 2 GiB queue is simply too small | The consumer is slower than producers; the queue reaches capacity by about 40 s. More space only delays backpressure and increases the eventual drain. |
| Probe SIMD/hash choice is material | Hash, insert, `memcmp`, and all non-clear consume work together are under 2.2% of consumer cycles. |
| Different native library caused historical/current divergence | The primary reproduction uses the exact historical native SHA-256. |

## Ranked optimization candidates (not implemented)

| Rank | Candidate | Expected benefit | Correctness risk |
|---:|---|---|---|
| 1 | Replace full-capacity clearing with generation/epoch tags or touched-slot clearing, while explicitly invalidating borrowed observations at transaction boundaries. | Very high: attacks the measured 97.85% consumer hot path and both clears. | Medium: generation wrap, table reuse, exception paths, and borrowed key/value lifetimes must be proven. |
| 2 | Stop sizing the repeated-snapshot table from total event count; size from actual snapshot-read demand and define a safe shrink/rebuild policy after preparation outliers. | High: prevents a preparation transaction from permanently setting steady-state work. | Low-medium: allocation behavior and iterator/view stability need focused tests; no semantic change should be needed. |
| 3 | Remove or make constant-time the redundant end-of-transaction full clear; retain RAII lifetime invalidation. | Medium-high: removes roughly one of two complete scans, but leaves the other and may not restore enough service rate alone. | Medium: the guard currently protects borrowed observation lifetimes on every exit path. |
| 4 | Compact identical snapshot reads before packing while retaining conflicting evidence and provenance. | Modest for full mix (4.51% flat bytes), larger for NewOrder/Delivery capture bandwidth; does not solve checker clearing. | Medium-high: state/value equality, ordering, and violation diagnostics must remain exact. |
| 5 | Reduce producer clone/fragment allocation and packing work. | Moderate after the native bottleneck is fixed; capture-only overhead is about 14.7%. | Medium: storage ownership and successful-consumption visibility are subtle. |
| 6 | Design correctness-preserving bounded history compaction. | Negligible current CPU benefit; important for long-run retained memory. | High: snapshot compatibility and lower-bound proofs can change semantics. |
| 7 | Prefer a wider probe implementation or micro-optimize hashing. | Negligible for this incident (well under 1% each). | Low, but not worth prioritizing. |

The first optimization should be developed as a separate change with focused
table-lifecycle regression tests, ASan/UBSan, workload reproduction, and a
before/after perf profile. Queue size and history compaction should not be used
as substitutes for fixing the confirmed scratch-table lifecycle.

## Diagnostic changes and validation

Native checker diagnostics add counters for repeated-read table capacity and
work, snapshot duplicates/bytes, history traversal, fixed-memory duration
histograms, and slowest-check identity. Producer diagnostics are gated by:

```text
TIDB_ONLINE_CHECKER_DUPLICATE_READ_DIAGNOSTICS=0|1
```

The default is off. Disabled mode installs no observer and retains no fragment
ranges/maps. Enabled work is O(events) with O(unique snapshot keys) temporary
state and publishes aggregates only after successful transaction publication.
The benchmark runner exposes smoke-only transaction weights. The gate is
recorded suite-wide in `experiment.json` and per attempt in
`checker-environment.json`.

Final validation:

```sh
ctest --test-dir online/build --output-on-failure \
  -R 'co_checker_online_(tests|c_dlopen_test|layout_sampler_tests)$'
# 3/3 passed

python3 online/tests/test_analyze_tpcc_global_lock.py
# 20/20 passed

bash -n online/scripts/run_tpcc_global_lock_comparison.sh
# passed

(cd /home/dyf/code/db-consistency/tidb-online && \
  ./tools/check/failpoint-go-test.sh pkg/store/driver/txn \
  -run 'Test(TxnCapture|CopReadCapture)' -count=1)
# passed

(cd /home/dyf/code/db-consistency/tidb-online/pkg/store/driver/txn/captureadapter && \
  go test -tags=intest,deadlock -count=1)
# passed
```

Additional native configurations already validated the same changes:

- Clang warnings-as-errors build and tests: 3/3 passed.
- ASan+UBSan build and tests: 3/3 passed.
- TSan build passed. TSan execution is unavailable on this host because the
  runtime exits with `FATAL: ThreadSanitizer: unexpected memory mapping`.
- `git diff --check` passed in both repositories.

No performance optimization was implemented or benchmarked in this task.
