# Dense-ID `snapshot-so-ww` benchmark (2026-08-07)

## Result

Replacing timed `HexValue` hash tables and copies with collision-checked dense
`uint32_t` IDs improves median checker time by 11.9x to 32.1x on five
representative traces. End-to-end wall time improves by 3.5% to 38.4%, and peak
RSS falls by 2.7% to 44.1%. All measured trials and all 25 retained-trace
regressions returned `pass`.

Each median uses one warm-up and three measured trials. The baseline is the
committed pre-interning Release binary with SHA-256
`85cd87f6fdba7757269690987f7f7ff11426d045a1ea82b08f574263d833cf08`.
The candidate Release binary has SHA-256
`9176b6236530302ea29593eea918db3b6b11f1c481aef92b4bbe2f17ebc93008`.

| Workload | Events | Baseline ms | Dense-ID ms | Speedup | Baseline wall s | Dense-ID wall s | Wall reduction | RSS reduction |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Small TPCC, write-heavy | 729,344 | 478.545 | 14.919 | 32.076x | 0.86 | 0.53 | 38.4% | 44.1% |
| NewOrder 100%, 45.77% reads | 3,282,833 | 1,568.431 | 74.216 | 21.133x | 3.69 | 3.56 | 3.5% | 26.1% |
| TPCC full mix 360s, 40.65% reads | 4,794,984 | 2,471.953 | 134.315 | 18.404x | 5.44 | 5.14 | 5.5% | 24.8% |
| Synthetic compact, 98.68% reads | 4,980,736 | 587.933 | 49.463 | 11.886x | 1.66 | 1.35 | 18.7% | 2.7% |
| Largest TPCC, write-heavy | 10,047,372 | 7,932.671 | 296.501 | 26.754x | 13.28 | 9.25 | 30.3% | 43.6% |

The smaller end-to-end gain on the two real read-heavy traces reflects their
large binary parsing share. Interning is input-model construction and remains
outside the checker timer for the same reason TimeKiller's JSON-to-transaction
materialization is outside its checker timer; GNU wall time includes it.

## Comparison with patched TimeKiller

The candidate and TimeKiller used the matched post-parse timing boundary. The
same candidate run produced:

| Workload | Dense-ID co ms | TimeKiller ms | co wall s | TimeKiller wall s | co/TK peak RSS MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| Small TPCC | 14.919 | 703 | 0.53 | 1.43 | 252 / 2,087 |
| NewOrder 100% | 74.216 | 1,926 | 3.56 | 4.00 | 1,278 / 3,287 |
| TPCC full mix 360s | 134.315 | 2,929 | 5.14 | 5.90 | 1,833 / 4,530 |
| Synthetic compact reads | 49.463 | 401 | 1.35 | 3.09 | 676 / 3,785 |
| Largest TPCC | 296.501 | 6,781 | 9.25 | 12.91 | 3,551 / 17,608 |

Both checkers returned `pass` in every trial. The earlier synthetic case where
TimeKiller won checker-only time is no longer a counterexample after both tools
use compact IDs in their hot paths.

## Implementation

- YAML and `COTRACE1` parsers assign deterministic first-seen `KeyId` and
  non-null `ValueId` values.
- A flat power-of-two table stores an avalanched hash and ID; equality against
  the referenced original bytes resolves every hash collision.
- `Trace` keeps compact per-event ID arrays without changing the on-disk format
  or removing raw bytes needed by other checkers and diagnostics.
- `snapshot-so-ww` uses vector-indexed value state and generation-stamped arrays
  for final writes, reads, and wrong keys.
- Rollback deltas contain two 32-bit IDs instead of an event ID plus a copied
  `optional<HexValue>`.
- Deltas reserve their final size and move into the change list instead of
  being deep-copied.

No explicit checker SIMD was added. The post-change profile shows the checker
itself at roughly 1.5% of process samples, while byte equality already resolves
through glibc's AVX2 `memcmp`. `HexValueHash` is about 2% of total samples and
is paid during parsing, so hand-written SIMD there would add portability and
dispatch complexity for little remaining end-to-end benefit.

## Validation and evidence

- GCC 13 Release build: `-O3 -DNDEBUG` in the dedicated
  `/tmp/co-timekiller-bench/co-checker-interned-release` directory.
- Existing and focused unit tests passed in Release.
- AddressSanitizer and UndefinedBehaviorSanitizer tests passed.
- The optimized Release checker passed all 20 retained TPCC binaries and all 5
  retained synthetic binaries.
- The five-trace benchmark recorded no failure, timeout, OOM, malformed output,
  or verdict difference.

Candidate CSVs, commands, environment capture, and per-trial logs are under
`/tmp/co-timekiller-bench/read-heavy/results/interned-selected-20260807/`.
Baseline raw results are under the previously recorded `full-20260807`,
`tpcc-neworder-180s-matched-20260807`, `tpcc-360s-matched-20260807`, and
`synthetic-matched-20260807` result directories.

The before/after co-checker medians were collected in separate benchmark runs
on the same host and day rather than an interleaved A/B schedule. Fixed process
order, page-cache state, CPU frequency, and background activity remain sources
of wall-time noise; the checker-only differences are much larger than that
noise, while the 3.5% wall improvement should be treated conservatively.
