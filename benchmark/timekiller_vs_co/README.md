# TimeKiller/Chronos versus `snapshot-so-ww`

`run_benchmark.py` is a standard-library-only driver for the offline
performance comparison. It validates and event-sorts `COTRACE1` inputs,
converts each input once, runs a smallest-trace smoke test, performs warm-ups
and measured trials, and preserves the evidence needed to reproduce every
number.

## Prerequisites

Build a dedicated Release copy of this repository containing both
`co-checker` and `cotrace-to-timekiller`. Build TimeKiller commit
`092df846d739e088e026c58c3679a6eba8f4165c` as the assembly jar. The harness
checks that source commit before starting.

Apply the zero-context timing patch to that pinned checkout before building:

```bash
git apply --unidiff-zero /path/to/timekiller-post-parse-timing.patch
```

It leaves JSON decoding and transaction/operation materialization in `Loading
time`, but moves the reported `Checking time` boundary before checker-specific
GC, dense initial-state/per-key-index construction, sorting, and the SI checker.
This matches co-checker's policy of excluding input-format loading while
including checker-owned preparation and teardown.

The default expected paths are:

```text
<repo>/build-benchmark-release/co-checker
<repo>/build-benchmark-release/cotrace-to-timekiller
/home/dyf/code/db-consistency/TimeKiller/target/TimeKiller-jar-with-dependencies.jar
```

Paths are configurable. Generated JSON and raw results default to
`/tmp/co-checker-timekiller-benchmark`, outside the repository.

## Run

List and validate the trace order without requiring built tools:

```bash
python3 benchmark/timekiller_vs_co/run_benchmark.py --list-traces
```

Run the default comparison (one warm-up and three measured trials per tool and
trace):

```bash
python3 benchmark/timekiller_vs_co/run_benchmark.py
```

A typical explicit run is:

```bash
python3 benchmark/timekiller_vs_co/run_benchmark.py \
  --co-checker /tmp/co-timekiller-bench/co-checker-release/co-checker \
  --converter /tmp/co-timekiller-bench/co-checker-release/cotrace-to-timekiller \
  --timekiller-jar /home/dyf/code/db-consistency/TimeKiller/target/TimeKiller-jar-with-dependencies.jar \
  --java-options='-Xms1g -Xmx48g -XX:+UseG1GC -XX:+ExitOnOutOfMemoryError' \
  --warmups 1 \
  --trials 3 \
  --timeout-seconds 7200 \
  --result-dir /tmp/co-timekiller-bench/results/run-01 \
  --work-dir /tmp/co-timekiller-bench/work/run-01 \
  --converted-retention failed
```

Use `--trace` for exact paths or `--trace-glob` for one or more selections.
Exact paths override globs. Use `--max-traces 1` for a short harness check.
`--no-smoke-test` disables the separate smallest-trace smoke run. See `--help`
for all defaults.

Run the focused harness parser tests with:

```bash
python3 -m unittest discover -s benchmark/timekiller_vs_co -p 'test_*.py'
```

Generate deterministic read-heavy histories outside the repository with:

```bash
python3 benchmark/timekiller_vs_co/generate_read_heavy_trace.py \
  /tmp/read-heavy.trace --keys 65536 --read-transactions 19200 \
  --reads-per-transaction 256 --key-bytes 8 --value-bytes 8 \
  --stats /tmp/read-heavy.stats.json
```

By default, one preparation transaction writes every key and all remaining
transactions read the latest prepared values. Add `--history-rounds N` to
insert `N` committed writer transactions between one empty anchor transaction
per reader and the corresponding stale-snapshot read. `--repeated-writes N`
controls how many times each history transaction writes a key; co-checker keeps
only the final write while TimeKiller retains each operation in its per-key
writer history. Benchmark runs default to the configurable `/tmp` work area.
Deliberately retained input fixtures may be copied to the repository's ignored
`bin_traces/read-heavy/` directory; converted JSON and benchmark logs should
remain outside the repository.

The converter invocation defaults to:

```text
cotrace-to-timekiller --stats <stats.json> <input.trace> <output.json>
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
- `conversion.csv`: converter wall time, RSS, output size/count validation, and
  failure evidence.
- `trials.csv`: every smoke, warm-up, and measured run. Verdict and exit status
  are separate fields.
- `summary.csv`: per-trace/tool medians over valid measured trials only.
- `report.md`: generated comparison tables and measurement caveats.
- `logs/`: command, stdout, stderr, and raw GNU time output for every process.

CSV files are flushed and synced after every row. Timeouts, OOM messages,
signals, malformed output, and skipped TimeKiller runs after conversion failure
are recorded instead of aborting the remaining trace set. A co-checker `fail`
verdict with exit status 1 and a TimeKiller `Do NOT satisfy SI` verdict with exit
status 0 are both successful benchmark executions.

Every timed process is wrapped with GNU `/usr/bin/time -v` and GNU `timeout`.
`JAVA_TOOL_OPTIONS`, `JDK_JAVA_OPTIONS`, and `_JAVA_OPTIONS` are recorded and
then removed from child environments so they cannot silently change the fixed
JVM options.
