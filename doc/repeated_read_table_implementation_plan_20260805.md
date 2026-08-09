# RepeatedReadTable Optimization Implementation Plan

## Purpose

Implement and validate the `RepeatedReadTable` redesign identified by
`full_tpcc_profile_20260805.md`. The primary goal is to remove work proportional
to retained table capacity from ordinary transaction checking while preserving
all checker semantics and borrowed-view lifetime guarantees.

The work must be evidence driven. Implement the smallest correctness-preserving
core first, run focused tests and a retained-capacity benchmark immediately, and
stop adding optimizations if that core does not remove the measured bottleneck.

## Repositories and baseline

- Checker: `/home/dyf/code/db-consistency/co-checker`
- Instrumented TiDB: `/home/dyf/code/db-consistency/tidb-online`
- Pristine TiDB commit: `b94006d3abffd428634ecc5c499f4bcb7b4d06d1`
- Investigation report: `doc/full_tpcc_profile_20260805.md`

At the time this plan was written, both repositories contained important
unstashed profiling and diagnostic changes. They must be inspected and
preserved. Do not reset, clean, overwrite, or silently reformat either
worktree.

Create a new branch before editing:

- Checker: `perf/repeated-read-table-touched-clear`
- TiDB: create `perf/repeated-read-table-profiling-gate` only if TiDB profiling
  code must be changed for an unbiased measurement.

Dirty changes should carry onto the new branch unchanged. Record status and
HEAD before and after branch creation. Do not commit or push unless explicitly
requested.

## Measured failure to eliminate

A preparation transaction containing 2,400,331 unique snapshot reads grew the
table to 4,194,304 slots. The table never shrank. Every later transaction
visited the complete retained capacity twice:

1. `RepeatedReadTable::reset()` called `clear()`.
2. `BorrowedReadGuard` called `clear()` at transaction exit.

That produced 8,388,608 slot visits per checked transaction. Full-mix and
NewOrder-only checker service remained approximately 4.3--4.4 ms despite very
different event counts because `reset`, `clear`, and `memset` accounted for
approximately 97.85% of checker-consumer CPU.

The target is not to make the 2.4-million-read transaction O(1). Every unique
read in that transaction must still be processed and its borrowed views must be
invalidated. The target is to prevent that outlier from imposing
capacity-proportional work on all subsequent transactions.

## Scope

Implement:

1. Touched/occupied-slot tracking.
2. O(unique snapshot keys) physical cleanup.
3. O(1) cleanup when the table is already empty.
4. Snapshot-read-aware sizing and container reservation.
5. Strongly exception-safe growth/rebuild.
6. Conservative empty-boundary shrinking after sustained under-utilization.
7. Focused unit, sanitizer, microbenchmark, and deterministic TPC-C validation.
8. Compile-time control or removal of diagnostic-only hot-path profiling.

Do not implement in this work:

- Upstream duplicate-read filtering.
- Packed event format changes.
- MPSC queue changes.
- Publication or ordering protocol changes.
- Checker semantic changes.
- Unrelated history, capture, or TiDB optimizations.

## Fixed correctness invariants

The implementation is unacceptable unless all of these remain true:

- No borrowed key or value view survives transaction completion, reset, or an
  exception.
- Equal repeated reads represent one logical observation.
- A different value or presence state for the same key produces
  `InconsistentRepeatedRead` with the first and conflicting event sequences.
- Absent and present-empty values are distinct.
- Fingerprint collisions, full-hash collisions, and key-byte comparisons remain
  correct.
- Scalar, SSE2, AVX2, and NEON probing remain semantically equivalent.
- Every occupied slot is represented exactly once in the touched list.
- Every touched slot is physically reset before its source transaction storage
  can be recycled.
- Control-byte clearing updates the wraparound mirror used by unaligned SIMD
  loads.
- Cleanup is allocation-free and `noexcept`.
- Mid-transaction growth failures leave the old active table unchanged and
  clearable.
- Empty-boundary rebuild failures leave the old empty layout unchanged.
- Optional shrink failure cannot fail an otherwise valid transaction.

## Recommended data layout

Keep the existing control and slot layout so probing does not change:

```cpp
std::vector<std::uint8_t> control_;  // capacity + SIMD group width
std::vector<Slot> slots_;            // capacity
std::vector<std::size_t> touched_;   // one entry per occupied slot
std::size_t size_ = 0;
```

Use `std::size_t` indices for the first implementation. A narrower index type
should only be considered after proving and testing a hard capacity bound.

Maintain these debug invariants:

```text
touched_.size() == size_
control_[index] != kEmpty for every index in touched_
no occupied control byte exists outside touched_
```

The final invariant may be checked by an expensive debug/test helper rather
than in production.

## Core algorithms

### Cleanup

The end-of-transaction guard remains the authoritative lifetime boundary:

```cpp
void RepeatedReadTable::clear() noexcept {
    if (touched_.empty()) {
        assert(size_ == 0);
        return;
    }

    for (const std::size_t index : touched_) {
        slots_[index] = Slot{};       // remove borrowed key/value views
        set_control(index, kEmpty);   // update primary and mirrored byte
    }

    touched_.clear();
    size_ = 0;
}
```

Do not remove or defer `BorrowedReadGuard`. The runtime may recycle the packed
batch, queue node, chunk segments, and budget lease immediately after the
engine returns.

`reset()` may continue calling `clear()` to preserve its defensive and public
semantics. After guard cleanup, this call is O(1) because `touched_` is empty.

### Lookup and insertion

Probe for an existing key before deciding to grow. This avoids growing when a
duplicate arrives at the load threshold.

```cpp
InsertResult insert(const EventView& read) {
    const std::uint64_t hash = hash_(read.key);
    Probe probe = find_existing_or_empty(hash, read.key);

    if (probe.found) {
        const Slot& first = slots_[probe.index];
        return result_equal(first, read)
            ? SameResult{first.first_sequence}
            : ConflictingResult{first.first_sequence};
    }

    if (size_ == maximum_load()) {
        grow_strong();
        probe = find_existing_or_empty(hash, read.key);
    }

    // The touched entry must exist before a borrowed view becomes reachable.
    touched_.push_back(probe.empty_index);
    slots_[probe.empty_index] =
        Slot{hash, read.key, read.value_state, read.value, read.sequence};
    set_control(probe.empty_index, fingerprint(hash));
    ++size_;
    return Inserted{};
}
```

`touched_.push_back()` is the only potentially throwing step in the publication
sequence. Slot assignment, control publication, and size increment must remain
non-throwing.

Reserve touched capacity from the validated snapshot-read count before any
current-transaction view is borrowed. This should normally prevent insertion
from allocating, but append-before-publication remains required as a defensive
guarantee.

### Growth and rebuild

Do not mutate or move the current active arrays before all replacement storage
is ready.

For an active-table grow:

1. Compute the new capacity with checked arithmetic.
2. Allocate temporary control, slot, and touched arrays.
3. Initialize the complete temporary control array, including its mirror.
4. Reserve at least the current live size in the temporary touched list.
5. Rehash only indices in the current touched list.
6. Validate the replacement size and touched invariants.
7. Swap the complete replacement into the table.

If any allocation fails, the old table must retain every active borrowed view,
control byte, and touched entry so the guard can still clear it.

For an empty-boundary resize, failure must leave the existing empty layout
unchanged. Required growth failure may propagate. Optional shrink failure must
be caught locally and treated as a skipped optimization.

### Snapshot-read-aware reset

Production packed transactions already expose validated read and write counts.
Use snapshot-read count for:

- `RepeatedReadTable::reset()` capacity planning.
- `touched_` reservation.
- Logical read and pending-read container reservation.

Use write count for final-write reservation.

For any public span overload that does not carry counts, derive snapshot-read
and write counts during its existing validation pass. Local reads must not
inflate the table.

The known preparation outlier is entirely snapshot reads, so this change does
not prevent the known growth. It prevents unrelated write-heavy or local-read
transactions from producing the same problem.

### Shrink policy

Shrinking is a secondary memory and locality improvement after touched cleanup
removes the CPU dependence on retained capacity.

Only consider shrink during `reset()`, after cleanup and before borrowing the
new transaction's views. Use overflow-safe comparisons such as:

```cpp
required_capacity <= capacity_ / shrink_ratio
```

Initial policy to benchmark, not to accept without measurement:

- Shrink ratio: 8.
- Grace period: 64 consecutive under-utilized transactions.
- Target: the maximum required capacity observed during the grace window,
  rounded through the normal capacity function.
- Minimum: at least one SIMD group and the existing minimum capacity.

Reset the shrink streak when demand no longer satisfies the ratio. After a
successful grow or shrink, reset all policy state.

Build the smaller layout completely before swapping. A failed optional shrink
must retain the larger empty layout and continue the transaction. Record a
shrink-attempt/failure metric only in profiling-enabled builds.

The shrink operation should also release excessive touched-list capacity. Do
not call `shrink_to_fit()` as an exception or performance guarantee; construct
the desired replacement explicitly.

### Optional dense cleanup

Do not add a dense fallback in the first core change. First benchmark touched
cleanup at dense occupancy. Random touched-slot writes may lose to the existing
sequential scan for the 2.4-million-key preparation transaction.

Only add a fallback if the benchmark identifies a stable crossover. If added:

- Select it from occupancy, not retained capacity alone.
- Keep the threshold as a named policy constant.
- Preserve identical slot destruction and mirrored-control semantics.
- Test both sides of the threshold in every probe mode.

## Profiling instrumentation plan

The current dirty changes contain detailed timing and counter instrumentation
in checker and TiDB hot paths. An unbiased performance comparison requires a
production-like build where diagnostic-only work is compiled out.

### Inventory

Before changing the algorithm, classify each current diagnostic addition as:

1. Correctness or protocol validation: always enabled.
2. Stable low-cost operational metric: normally enabled.
3. Detailed profiling counter/timer/map: compile-time optional.
4. One-off diagnostic code with no continuing value: candidate for removal.

Review both repository diffs. Do not assume all new metrics have the same cost.
In particular, distinguish simple atomic totals from per-event timers, map
construction, provenance analysis, and table-visit accounting.

### Build control

Prefer one centralized checker build option, for example:

```text
CO_CHECKER_ENABLE_DETAILED_PROFILING=OFF
```

It should define a 0/1 compile-time macro. Use `#if` on that value so disabled
hot-path increments, timers, and diagnostic map construction generate no
runtime branch.

If exported metric structures or C API layouts are already consumed, keep
their layout stable where practical. Disabled detailed counters may report zero
or an explicit availability flag. Do not compile out correctness checks.

If TiDB producer diagnostics are active in the measured build, give them an
equivalent compile-time or build-tag gate on a separate TiDB branch. Do not
change TiDB merely because diagnostic code exists; first establish whether it
executes in the default benchmark configuration.

### Measurement builds

Maintain separate build directories/artifacts:

- Old algorithm, detailed profiling disabled: apples-to-apples baseline.
- New algorithm, detailed profiling disabled: performance result.
- New algorithm, detailed profiling enabled: slot-visit and attribution check.
- Sanitizer build: lifetime and exception validation.

Build and test both profiling configurations. Quantify enabled-versus-disabled
overhead rather than assuming it is negligible.

## Expected files to inspect or modify

Confirm the actual dependency graph before editing. Likely checker files are:

- `online/include/co_checker/online/repeated_read_table.h`
- `online/src/repeated_read_table.cpp`
- `online/src/engine.cpp`
- `online/include/co_checker/online/engine.h`
- relevant CMake/build-option files
- metric export code if detailed counters become optional
- `online/tests/test_repeated_read_table.cpp`
- engine, C API, and runtime tests
- `online/tools/microbench.cpp`

Do not modify TiDB checker-capture semantics. TiDB changes, if any, should be
limited to compiling diagnostic-only producer instrumentation out of the
performance build.

## Staged implementation workflow

### Phase 0: establish state

1. Read all applicable `AGENTS.md` files and the complete profile report.
2. Record both HEADs, branches, status, staged diffs, unstaged diffs, and
   relevant untracked files.
3. Identify which dirty diffs are profiling-only and which are functional.
4. Create the new branch or branches without stashing or discarding changes.
5. Confirm that branch creation changed no worktree content.

### Phase 1: gate profiling and capture the old-algorithm baseline

1. Add the smallest compile-time gate needed for detailed hot-path profiling.
2. Build profiling-enabled and profiling-disabled variants.
3. Run existing focused tests in both variants.
4. With profiling disabled and the old table algorithm intact, capture:
   - reset/clear microbenchmark time;
   - one 4,194,304-slot outlier followed by small transactions;
   - RSS and retained table capacity;
   - compiler flags and CPU/probe mode.
5. Preserve the exact baseline command and result artifact outside source
   directories or in the project's normal ignored results area.

Do not use the profile report's diagnostic-enabled numbers as the only
before-result.

### Phase 2: add or update focused tests before the core implementation

Add tests that express the new behavioral and complexity requirements without
depending on wall-clock timing:

- Outlier capacity followed by empty and small transactions.
- Actual cleanup visits proportional to touched slots when profiling is on.
- Second clear of an already empty table performs zero slot work.
- Collision, mirror-boundary, growth, and reuse cases.
- Exception paths and source-storage poisoning.
- Snapshot-read versus total-event sizing.

Confirm that the complexity assertions fail against the old algorithm for the
expected reason while existing semantic tests continue to pass.

### Phase 3: implement touched cleanup

1. Add touched storage and invariants.
2. Change insertion publication ordering.
3. Change cleanup to reset only touched slots.
4. Keep `BorrowedReadGuard` unchanged in purpose and placement.
5. Make growth/rebuild strongly exception safe with touched state.
6. Preserve the existing control layout and all probe masks.
7. Run focused tests immediately.
8. Run ASan/UBSan lifetime tests.
9. Run the retained-capacity benchmark immediately.

### Phase 4: add snapshot-read-aware sizing

1. Propagate validated read and write counts through engine entry points.
2. Count source classes during validation where counts are not already carried.
3. Change table and temporary-container reservations.
4. Test zero-read/many-write, local-read-heavy, duplicate-heavy, and all-read
   transactions.
5. Re-run the focused benchmark.

### Phase 5: early decision gate

Proceed only if all of the following hold:

- Semantic and randomized differential tests pass.
- No sanitizer reports a borrowed-view lifetime problem.
- A post-outlier small transaction performs O(U), not O(C), cleanup work.
- The profiling-disabled retained-capacity benchmark improves materially.
- Profiling shows that capacity-wide clearing is no longer dominant.

If any condition fails, stop adding shrink or dense fallback. Reproduce the
failure, collect `perf` or equivalent evidence, and explain whether the problem
is algorithmic, allocator-related, cache/TLB-related, instrumentation overhead,
or a correctness constraint. Fix the core only if the evidence supports it.

### Phase 6: implement and tune shrink

1. Add empty-boundary hysteresis state.
2. Implement best-effort full replacement and swap.
3. Test allocation failure and alternating demand.
4. Measure memory release, rebuild latency, and transaction tail latency.
5. Tune policy from results.
6. Benchmark dense cleanup and add a fallback only if justified.

### Phase 7: full validation

Run:

- All relevant checker tests.
- C API and runtime tests.
- Scalar/SSE2/AVX2 tests.
- NEON tests on supported hardware or CI.
- ASan and UBSan.
- Any existing randomized/reference differential tests.
- Profiling-enabled metric tests.
- Profiling-disabled production-like tests.

Do not silently skip a suite. Record any environment limitation.

### Phase 8: deterministic TPC-C comparison

Follow the exact workflow, seed, scale, warehouse count, concurrency, duration,
build mode, and validation procedure in the profile report. At minimum run:

1. Full TPC-C mix.
2. NewOrder-only.
3. Pristine TiDB baseline.
4. Instrumented capture-only.
5. Instrumented TiDB with the full checker.

Never switch the dirty TiDB worktree to the pristine commit. Use the existing
separate pristine build/worktree mechanism.

Use profiling-disabled binaries for performance conclusions. Run a smaller or
separate profiling-enabled repetition to validate table-work attribution.

## Focused test matrix

### Table semantics

- First insert followed by an equal repeat.
- Absent followed by absent.
- Present-empty followed by present-empty.
- Absent versus present-empty conflict.
- Present values with equal and unequal bytes.
- Exact first and conflicting event sequences.
- Multiple conflicts after the first observation.

### Hash and probe behavior

- Constant full hash for different keys.
- Equal fingerprints with different full hashes.
- Probe wraparound at the final group.
- Touched indices in the mirrored prefix.
- Growth at the load threshold.
- Duplicate at the load threshold must not grow.
- Scalar/SSE2/AVX2/NEON randomized differential.

### Lifetime and exceptions

- Normal return followed by immediate source-storage poisoning.
- Consistency-violation early return.
- Retained-state-limit return.
- Allocation failure while reserving touched storage.
- Allocation failure while building new control, slot, or touched arrays.
- Required growth failure leaves active state unchanged and clearable.
- Optional shrink failure leaves the old empty table reusable.
- Reset overflow leaves a valid empty table.

### Sizing and shrink

- Many writes with zero snapshot reads.
- Many local reads with zero snapshot reads.
- Duplicate-heavy reads where U is much smaller than R.
- The 2.4-million all-read outlier.
- Outlier followed by empty, one-key, and ordinary transactions.
- Sustained small demand triggers exactly one shrink.
- Alternating demand does not repeatedly grow and shrink.

## Focused benchmark matrix

Pre-grow the table to 4,194,304 slots, then run at least 100,000 transactions
for each live unique-key count:

```text
U = 0, 1, 23, 117, 4,096, dense
```

Measure:

- Nanoseconds per transaction.
- Reset time and guard-cleanup time separately when profiling is enabled.
- Touched slots and control writes.
- Retained table and touched capacity.
- RSS and peak retained memory.
- Grow/shrink count and maximum rebuild latency.
- Uniform versus constant/adversarial hashes.
- Available scalar and SIMD modes.

Compare:

1. Old algorithm, profiling disabled.
2. New touched algorithm, profiling disabled.
3. New touched algorithm, profiling enabled.
4. Touched-only versus dense fallback, if the fallback is considered.

## End-to-end metrics

For every relevant TPC-C run, report:

- Transactions and events accepted and checked.
- Mean and p50/p95/p99 checker service time.
- Checker throughput and workload arrival rate.
- Queue peak depth and bytes.
- Queue backpressure and waits.
- Drain time and end-to-verdict time.
- Table capacity, touched slots, clear calls, and actual cleanup work.
- RSS and peak retained state.
- Producer pack time.
- Native parse, reserve, build, handoff, and ordering time.
- Consumer `perf` attribution.
- Final checker verdict and all post-run validations.

Compare against both the new profiling-disabled old-algorithm baseline and the
preserved report results.

Expected direction, not a guaranteed acceptance number:

- Ordinary checker service near 0.1 ms rather than 4.3--4.4 ms.
- No capacity-proportional steady-state cleanup after the preparation outlier.
- Service comfortably below the approximately 1.17 ms full-mix and 1.26 ms
  NewOrder arrival budgets.
- No checker-driven 2 GiB queue saturation at the measured workload rate.
- Much shorter drain and end-to-verdict time.

If these do not occur, profile the new build before proposing more changes.

## Final deliverables

The implementation report must include:

1. Exact source and build-system changes.
2. Why each change was necessary.
3. How profiling code was classified, gated, or removed.
4. Tests and sanitizer runs with pass/fail counts.
5. Focused benchmark commands and before/after results.
6. Shrink-policy measurements and chosen values.
7. Full-mix and NewOrder TPC-C results.
8. Pristine, capture-only, and full-checker performance gaps.
9. `perf` findings for any remaining bottleneck.
10. Memory and retained-capacity results.
11. Risks, assumptions, and follow-up work.
12. Final status of both repositories, confirming unrelated dirty changes were
    preserved.

Do not declare success from unit tests alone. Completion requires the focused
post-outlier benchmark and the deterministic end-to-end comparison.
