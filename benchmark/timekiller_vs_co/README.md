# TimeKiller/Chronos versus `snapshot-so-ww`

`run_benchmark.py` is a standard-library-only driver for the offline
performance comparison. It accepts pre-generated dense `COTRACE1` inputs
(`flags == 1`), orders them by event count, projects each input once to
TimeKiller JSON, runs ten measured trials of each tool without a smoke test or
warm-up, and preserves the evidence needed to reproduce every number. It does
not reorder events inside a trace; the binary input is already
commit-timestamp sorted.

## Prerequisites

Build a dedicated Release copy of this repository containing `co-checker`,
`cotrace-densify`, and `cotrace-to-timekiller`. Build TimeKiller commit
`5b7a4a754cd2cf49d3a12b6a7b9f10a99cc5493f` as the assembly jar. The harness
checks that source commit before starting.

That commit leaves JSON decoding and transaction/operation materialization in
`Loading time`. The self-identifying `Checking time (excluding sorting)`
includes checker-specific GC, dense initial-state/per-key-index construction,
and the SI checker, while commit-timestamp sorting is reported separately. This
matches co-checker's policy: COTRACE1 is already commit sorted, so its checker
timer also excludes input-format loading and ordering while including
checker-owned preparation and checking.

The default expected paths are:

```text
<repo>/build-benchmark-release/co-checker
<repo>/build-benchmark-release/cotrace-densify
<repo>/build-benchmark-release/cotrace-to-timekiller
<workspace>/timekiller/target/TimeKiller-jar-with-dependencies.jar
```

Paths are configurable. Generated JSON and raw results default to an
operating-system temporary-data directory outside the repository.

## Prepare dense traces

Raw `COTRACE1` files (`flags == 0`) are preprocessing inputs and are not valid
benchmark inputs. Convert them once with `cotrace-densify`; keep the resulting
`*.dense.trace` files in a persistent directory outside the source repository.
For example, set `RAW_TRACE_ROOT`, `DENSE_TRACE_ROOT`, and `CO_BUILD_DIR` for
the current environment, then convert one testcase with:

```bash
mkdir -p "${DENSE_TRACE_ROOT}/tpcc"

"${CO_BUILD_DIR}/cotrace-densify" \
  --stats "${DENSE_TRACE_ROOT}/tpcc/testcase.dense.stats.json" \
  "${RAW_TRACE_ROOT}/tpcc/testcase.trace" \
  "${DENSE_TRACE_ROOT}/tpcc/testcase.dense.trace"
```

Use corresponding persistent subdirectories for other workload families, for
example `ycsb/event_sweep/` and `ycsb/session_sweep/`. Preserve testcase names
when replacing `.trace` with `.dense.trace` so raw and dense artifacts remain
easy to match.

The densifier assigns deterministic first-seen key and value IDs. co-checker
reads those IDs directly from the dense binary. `cotrace-to-timekiller` reads
the same dense file and writes those existing IDs to temporary JSON without
re-interning raw keys or values. Thus both measured tools start from compact
IDs produced by the same preprocessing step.

## Run

List and validate the trace order without requiring built tools:

```bash
python3 benchmark/timekiller_vs_co/run_benchmark.py --list-traces
```

Run the default comparison (ten measured trials per tool and trace, with no
warm-up or smoke test):

```bash
python3 benchmark/timekiller_vs_co/run_benchmark.py
```

For an explicit run, set `CO_BUILD_DIR`, `TIMEKILLER_ROOT`, and
`BENCH_OUTPUT_ROOT` to environment-specific locations. Set `DENSE_TRACE_ROOT`
to the persistent dense-trace collection. Keep the output root outside the
source repository. Then run:

```bash
python3 benchmark/timekiller_vs_co/run_benchmark.py \
  --trace-glob "${DENSE_TRACE_ROOT}/tpcc/*.dense.trace" \
  --co-checker "${CO_BUILD_DIR}/co-checker" \
  --converter "${CO_BUILD_DIR}/cotrace-to-timekiller" \
  --timekiller-source "${TIMEKILLER_ROOT}" \
  --timekiller-jar "${TIMEKILLER_ROOT}/target/TimeKiller-jar-with-dependencies.jar" \
  --java-options='-Xms1g -Xmx48g -XX:+UseG1GC -XX:+ExitOnOutOfMemoryError' \
  --warmups 0 \
  --trials 10 \
  --no-smoke-test \
  --timeout-seconds 7200 \
  --result-dir "${BENCH_OUTPUT_ROOT}/results/run-01" \
  --work-dir "${BENCH_OUTPUT_ROOT}/work/run-01" \
  --converted-retention none
```

Use `--trace` for exact paths or `--trace-glob` for one or more selections.
Exact paths override globs. Use `--max-traces 1` for a short harness check.
The explicit trial options above equal the defaults and make the intended
paper run visible in the recorded command. See `--help` for all options.

Run the focused harness parser tests with:

```bash
python3 -m unittest discover -s benchmark/timekiller_vs_co -p 'test_*.py'
```

Set `READ_HEAVY_DIR` to a writable directory outside the repository, then
generate deterministic read-heavy histories with:

```bash
python3 benchmark/timekiller_vs_co/generate_read_heavy_trace.py \
  "${READ_HEAVY_DIR}/read-heavy.trace" \
  --keys 65536 --read-transactions 19200 \
  --reads-per-transaction 256 --key-bytes 8 --value-bytes 8 \
  --stats "${READ_HEAVY_DIR}/read-heavy.stats.json"
```

By default, one preparation transaction writes every key and all remaining
transactions read the latest prepared values. Add `--history-rounds N` to
insert `N` committed writer transactions between one empty anchor transaction
per reader and the corresponding stale-snapshot read. `--repeated-writes N`
controls how many times each history transaction writes a key; co-checker keeps
only the final write while TimeKiller retains each operation in its per-key
writer history. Benchmark runs default to the configurable operating-system
temporary-data area. Put retained raw fixtures in the configured raw-trace
collection, then pass them through `cotrace-densify` like other benchmark
inputs. Converted JSON and benchmark logs should remain outside the repository.

The converter invocation defaults to:

```text
cotrace-to-timekiller --stats <stats.json> <input.dense.trace> <output.json>
```

For a different compatible interface, set `--converter-command-template` with
the token placeholders `{converter}`, `{trace}`, `{json}`, and `{stats}`. The
template is tokenized but is never evaluated by a shell.

## Results

The result directory contains:

- `config.json`: complete options, hashes, versions, git commits, and captured
  environment details.
- `environment.txt`: concise human-readable environment and tool versions.
- `traces.csv`: validated header counts in benchmark order.
- `conversion.csv`: converter wall time, peak RSS in MiB, output size/count
  validation, and failure evidence.
- `trials.csv`: every measured run. Verdict and exit status are separate
  fields; TimeKiller checking and sorting milliseconds are recorded
  independently, and peak RSS is recorded in MiB.
- `summary.csv`: one row per trace and exactly these seven columns:

  ```text
  testcase,transactions,events,co_checker_ms,timekiller_ms,co_checker_peak_rss_mib,timekiller_peak_rss_mib
  ```

  The four measured values are arithmetic averages over successful measured
  trials only. Trial validity and diagnostic details remain in `trials.csv`
  and `report.md`, not in the compact summary.
- `report.md`: generated comparison tables and measurement caveats.
- `logs/`: command, stdout, stderr, and raw GNU time output for every process.

CSV fields ending in `_rss_mib` are GNU time's native KiB measurement divided
by 1024.

CSV files are flushed and synced after every row. Timeouts, OOM messages,
signals, malformed output, and skipped TimeKiller runs after conversion failure
are recorded instead of aborting the remaining trace set. A co-checker `fail`
verdict with exit status 1 and a TimeKiller `Do NOT satisfy SI` verdict with exit
status 0 are both successful benchmark executions.

Every timed process is wrapped with GNU `time -v` and GNU `timeout`; their
executable locations are configurable. `JAVA_TOOL_OPTIONS`, `JDK_JAVA_OPTIONS`,
and `_JAVA_OPTIONS` are recorded and then removed from child environments so
they cannot silently change the fixed JVM options.

For a dense YCSB trace collection, set `DENSE_TRACE_ROOT` to the persistent
root containing the `ycsb/event_sweep/` and `ycsb/session_sweep/`
subdirectories. Validate all selected inputs with:

```bash
python3 benchmark/timekiller_vs_co/run_benchmark.py \
  --trace-glob "${DENSE_TRACE_ROOT}/ycsb/event_sweep/*.dense.trace" \
  --trace-glob "${DENSE_TRACE_ROOT}/ycsb/session_sweep/*.dense.trace" \
  --list-traces
```

The header transaction/event counts are committed low-level checker records,
not requested YCSB attempts or logical operations. The session sweep is also
affected by optimistic aborts and TiDB auto-analyze pseudo-sessions, so use the
actual COTRACE1 header counts in every result.

`co_checker_ms` and `timekiller_ms` are checker timers and exclude input
loading. Peak RSS is GNU time's whole-process maximum: co-checker loads the
dense binary, while TimeKiller loads compact JSON projected from the same dense
IDs. Raw-to-dense preprocessing and the dense-to-JSON projection are outside
both measured process peaks. The RSS columns therefore compare compact-input
tool processes, including their loaders, in-memory models, checker structures,
language runtime, and JVM overhead; they are not checker-phase deltas or exact
checker-owned heap sizes.

`snapshot-so-ww` and Chronos's timestamp-based SI checker use different
decision procedures. Compare performance only on histories both accept, and
disclose that TimeKiller is run with `--enable_session false` while
`snapshot-so-ww` still uses session order.
