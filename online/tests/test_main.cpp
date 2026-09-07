#include "co_checker/online/engine.h"
#include "co_checker/online/packed_batch.h"
#include "reference_model.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

int run_bounded_queue_tests();
int run_byte_budget_tests();
int run_c_api_tests();
int run_order_gate_tests();
int run_packed_batch_tests();
int run_repeated_read_table_tests();
int run_runtime_tests();
int run_serialized_order_tests();

namespace {

using namespace co_checker::online;
using co_checker::online::test::ReferenceSOWWModel;

int failures = 0;

void check(const bool condition, const std::string_view expression,
           const std::string_view file, const int line) {
  if (condition) {
    return;
  }
  std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
  ++failures;
}

#define CHECK(expression)                                                      \
  check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

[[nodiscard]] OwnedBytes
bytes(const std::initializer_list<unsigned int> input) {
  OwnedBytes result;
  result.reserve(input.size());
  for (const auto value : input) {
    result.push_back(std::byte{static_cast<unsigned char>(value)});
  }
  return result;
}

struct TaggedInput {
  ValueState state = ValueState::Absent;
  OwnedBytes value;
};

[[nodiscard]] TaggedInput absent() { return {}; }

[[nodiscard]] TaggedInput present(OwnedBytes value) {
  return TaggedInput{.state = ValueState::Present, .value = std::move(value)};
}

struct OwnedEvent {
  EventSeq sequence = 0;
  EventOp op        = EventOp::Read;
  ValueState state  = ValueState::Absent;
  ReadSource source = ReadSource::Snapshot;
  OwnedBytes key;
  OwnedBytes value;

  [[nodiscard]] EventView view() const {
    return EventView{
        .sequence    = sequence,
        .op          = op,
        .value_state = state,
        .read_source = source,
        .key         = ByteView{key.data(), key.size()},
        .value       = ByteView{value.data(), value.size()},
    };
  }
};

[[nodiscard]] OwnedEvent read(const EventSeq sequence, OwnedBytes key,
                              TaggedInput result,
                              const ReadSource source = ReadSource::Snapshot) {
  return OwnedEvent{
      .sequence = sequence,
      .op       = EventOp::Read,
      .state    = result.state,
      .source   = source,
      .key      = std::move(key),
      .value    = std::move(result.value),
  };
}

[[nodiscard]] OwnedEvent write(const EventSeq sequence, OwnedBytes key,
                               TaggedInput result) {
  return OwnedEvent{
      .sequence = sequence,
      .op       = EventOp::Write,
      .state    = result.state,
      .source   = ReadSource::Snapshot,
      .key      = std::move(key),
      .value    = std::move(result.value),
  };
}

[[nodiscard]] SessionUid session(const std::uint64_t id) {
  return SessionUid{
      .runtime_epoch         = RuntimeEpoch{.words = {0x1234U, 0x5678U}},
      .producer_id           = 7,
      .local_session_counter = id,
  };
}

[[nodiscard]] OrderedTxnMeta meta(const CommitSeq commit_seq,
                                  const SessionUid session_id,
                                  const std::uint64_t session_seq) {
  OrderedTxnMeta result;
  result.commit_seq             = commit_seq;
  result.transaction_id.words   = {commit_seq + 1U, 0x100U};
  result.attempt_id.words       = {commit_seq + 1U, 0x200U};
  result.session_id             = session_id;
  result.session_seq            = session_seq;
  result.commit_token.timestamp = commit_seq + 100U;
  result.producer_id            = session_id.producer_id;
  result.producer_seq           = commit_seq + 1U;
  return result;
}

[[nodiscard]] std::vector<EventView>
make_views(const std::vector<OwnedEvent> &events) {
  std::vector<EventView> views;
  views.reserve(events.size());
  for (const auto &event : events) {
    views.push_back(event.view());
  }
  return views;
}

[[nodiscard]] ConsumeResult consume(SnapshotSOWWEngine &engine,
                                    const OrderedTxnMeta &metadata,
                                    const std::vector<OwnedEvent> &events) {
  const auto views = make_views(events);
  return engine.consume(metadata, views);
}

void expect_transaction_pass(const ConsumeResult &result) {
  CHECK(result.ok());
  CHECK(result.verdict.has_value());
  CHECK(!result.transaction_violation.has_value());
}

void expect_transaction_violation(const ConsumeResult &result,
                                  const ViolationKind kind) {
  CHECK(result.ok());
  CHECK(result.verdict.has_value());
  CHECK(result.transaction_violation.has_value());
  if (result.transaction_violation.has_value()) {
    CHECK(result.transaction_violation->kind == kind);
  }
  if (result.verdict.has_value()) {
    CHECK(result.verdict->status == ConsistencyStatus::Violation);
    CHECK(result.verdict->first_violation.has_value());
  }
}

void expect_error(const ConsumeResult &result, const EngineErrorCode code) {
  CHECK(!result.ok());
  CHECK(!result.verdict.has_value());
  CHECK(!result.transaction_violation.has_value());
  CHECK(result.error.has_value());
  if (result.error.has_value()) {
    CHECK(result.error->code == code);
  }
}

void test_empty_initial_state_and_first_read_only_checked() {
  {
    SnapshotSOWWEngine engine;
    const std::vector<OwnedEvent> events{
        read(1, bytes({1}), absent()),
    };
    const auto result = consume(engine, meta(0, session(1), 1), events);
    expect_transaction_pass(result);
    CHECK(result.verdict->status == ConsistencyStatus::Pass);
  }

  {
    SnapshotSOWWEngine engine;
    const std::vector<OwnedEvent> events{
        read(1, bytes({1}), present(bytes({9}))),
    };
    const auto result = consume(engine, meta(0, session(1), 1), events);
    expect_transaction_violation(result, ViolationKind::NoCompatibleSnapshot);
  }
}

void test_present_empty_is_distinct_from_absent() {
  SnapshotSOWWEngine engine;
  const std::vector<OwnedEvent> events{
      read(1, bytes({1}), absent()),
      read(2, bytes({1}), present(bytes({}))),
  };
  const auto result = consume(engine, meta(0, session(1), 1), events);
  expect_transaction_violation(result, ViolationKind::InconsistentRepeatedRead);
  CHECK(result.transaction_violation->keys ==
        std::vector<OwnedBytes>{bytes({1})});
  CHECK(result.transaction_violation->first_event_sequence == 1);
  CHECK(result.transaction_violation->conflicting_event_sequence == 2);
  const auto metrics             = engine.metrics();
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
  const auto marginal_read_bytes = sizeof(EventDescriptorV1) + 1U;
  CHECK(metrics.snapshot_read_events == 2U);
  CHECK(metrics.unique_snapshot_reads == 1U);
  CHECK(metrics.identical_snapshot_repeats == 0U);
  CHECK(metrics.conflicting_snapshot_repeats == 1U);
  CHECK(metrics.snapshot_read_packed_bytes == marginal_read_bytes * 2U);
  CHECK(metrics.duplicate_snapshot_read_packed_bytes == marginal_read_bytes);
  CHECK(metrics.identical_snapshot_repeat_packed_bytes == 0U);
  CHECK(metrics.conflicting_snapshot_repeat_packed_bytes ==
        marginal_read_bytes);
#else
  CHECK(metrics.snapshot_read_events == 0U);
  CHECK(metrics.conflicting_snapshot_repeats == 0U);
#endif

  engine.reset();
  const std::vector<OwnedEvent> present_empty{
      read(1, bytes({1}), present(bytes({}))),
  };
  expect_transaction_violation(
      consume(engine, meta(0, session(1), 1), present_empty),
      ViolationKind::NoCompatibleSnapshot);
}

void test_local_reads_are_ignored() {
  SnapshotSOWWEngine engine;
  const std::vector<OwnedEvent> events{
      read(1, bytes({1}), absent(), ReadSource::Local),
      read(2, bytes({1}), present(bytes({7})), ReadSource::Local),
      read(3, bytes({2}), absent(), ReadSource::Snapshot),
  };
  const auto result = consume(engine, meta(0, session(1), 1), events);
  expect_transaction_pass(result);
  CHECK(result.verdict->status == ConsistencyStatus::Pass);
}

void test_scratch_sizing_uses_snapshot_reads_and_writes() {
  constexpr std::size_t event_count = 100U;

  SnapshotSOWWEngine local_read_engine;
  std::vector<OwnedEvent> local_reads;
  local_reads.reserve(event_count);
  for (std::size_t index = 0; index < event_count; ++index) {
    local_reads.push_back(read(
        static_cast<EventSeq>(index + 1U),
        bytes({static_cast<unsigned int>(index)}), absent(),
        ReadSource::Local));
  }
  expect_transaction_pass(
      consume(local_read_engine, meta(0, session(1), 1), local_reads));
  CHECK(local_read_engine.metrics().repeated_reads.capacity == 16U);

  SnapshotSOWWEngine write_engine;
  std::vector<OwnedEvent> writes;
  writes.reserve(event_count);
  for (std::size_t index = 0; index < event_count; ++index) {
    writes.push_back(write(
        static_cast<EventSeq>(index + 1U),
        bytes({static_cast<unsigned int>(index)}), present(bytes({1U}))));
  }
  expect_transaction_pass(
      consume(write_engine, meta(0, session(2), 1), writes));
  CHECK(write_engine.metrics().repeated_reads.capacity == 16U);

  SnapshotSOWWEngine snapshot_read_engine;
  std::vector<OwnedEvent> snapshot_reads;
  snapshot_reads.reserve(event_count);
  for (std::size_t index = 0; index < event_count; ++index) {
    snapshot_reads.push_back(read(
        static_cast<EventSeq>(index + 1U),
        bytes({static_cast<unsigned int>(index)}), absent()));
  }
  expect_transaction_pass(
      consume(snapshot_read_engine, meta(0, session(3), 1), snapshot_reads));
  CHECK(snapshot_read_engine.metrics().repeated_reads.capacity == 128U);

  SnapshotSOWWEngine duplicate_read_engine;
  std::vector<OwnedEvent> duplicate_reads;
  duplicate_reads.reserve(event_count);
  for (std::size_t index = 0; index < event_count; ++index) {
    duplicate_reads.push_back(
        read(static_cast<EventSeq>(index + 1U), bytes({42U}), absent()));
  }
  expect_transaction_pass(
      consume(duplicate_read_engine, meta(0, session(4), 1), duplicate_reads));
  CHECK(duplicate_read_engine.metrics().repeated_reads.capacity == 128U);
}
void test_final_write_per_key() {
  SnapshotSOWWEngine engine;
  const auto key = bytes({1});
  const std::vector<OwnedEvent> first{
      write(1, key, present(bytes({10}))),
      write(2, key, present(bytes({20}))),
  };
  expect_transaction_pass(consume(engine, meta(0, session(1), 1), first));

  const std::vector<OwnedEvent> final_value{
      read(1, key, present(bytes({20}))),
  };
  expect_transaction_pass(consume(engine, meta(1, session(2), 1), final_value));

  const std::vector<OwnedEvent> intermediate_value{
      read(1, key, present(bytes({10}))),
  };
  expect_transaction_violation(
      consume(engine, meta(2, session(3), 1), intermediate_value),
      ViolationKind::NoCompatibleSnapshot);
}

void test_session_order_lower_bound() {
  SnapshotSOWWEngine engine;
  const auto key = bytes({1});
  expect_transaction_pass(
      consume(engine, meta(0, session(1), 10),
              std::vector<OwnedEvent>{write(1, key, present(bytes({8})))}));
  expect_transaction_pass(
      consume(engine, meta(1, session(2), 1), std::vector<OwnedEvent>{}));

  const auto result = consume(engine, meta(2, session(1), 11),
                              std::vector<OwnedEvent>{read(1, key, absent())});
  expect_transaction_violation(result, ViolationKind::NoCompatibleSnapshot);
  CHECK(result.transaction_violation->session_lower_bound == 0);
  CHECK(result.transaction_violation->effective_lower_bound == 0);
}

void test_write_write_lower_bound() {
  SnapshotSOWWEngine engine;
  const auto key = bytes({1});
  expect_transaction_pass(
      consume(engine, meta(0, session(1), 1),
              std::vector<OwnedEvent>{write(1, key, present(bytes({8})))}));

  const std::vector<OwnedEvent> second{
      read(1, key, absent()),
      write(2, key, present(bytes({9}))),
  };
  const auto result = consume(engine, meta(1, session(2), 1), second);
  expect_transaction_violation(result, ViolationKind::NoCompatibleSnapshot);
  CHECK(result.transaction_violation->write_write_lower_bound == 0);
  CHECK(result.transaction_violation->effective_lower_bound == 0);
}

void test_all_reads_must_match_one_prefix() {
  SnapshotSOWWEngine engine;
  const auto a = bytes({1});
  const auto b = bytes({2});
  expect_transaction_pass(
      consume(engine, meta(0, session(1), 1),
              std::vector<OwnedEvent>{write(1, a, present(bytes({1})))}));
  expect_transaction_pass(
      consume(engine, meta(1, session(2), 1),
              std::vector<OwnedEvent>{write(1, b, present(bytes({1})))}));
  expect_transaction_pass(
      consume(engine, meta(2, session(3), 1),
              std::vector<OwnedEvent>{write(1, a, present(bytes({2})))}));

  const std::vector<OwnedEvent> impossible{
      read(1, a, present(bytes({2}))),
      read(2, b, absent()),
  };
  expect_transaction_violation(
      consume(engine, meta(3, session(4), 1), impossible),
      ViolationKind::NoCompatibleSnapshot);
}

void test_compatible_snapshot_history_traversal_metrics() {
  SnapshotSOWWEngine engine;
  const auto a = bytes({1});
  const auto b = bytes({2});
  const auto c = bytes({3});
  expect_transaction_pass(consume(engine, meta(0, session(1), 1),
                                  std::vector<OwnedEvent>{
                                      write(1, a, present(bytes({10}))),
                                      write(2, b, present(bytes({20}))),
                                  }));
  expect_transaction_pass(
      consume(engine, meta(1, session(2), 1),
              std::vector<OwnedEvent>{write(1, c, present(bytes({30})))}));
  expect_transaction_pass(
      consume(engine, meta(2, session(3), 1),
              std::vector<OwnedEvent>{read(1, a, absent())}));

  const auto metrics = engine.metrics();
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
  CHECK(metrics.compatible_snapshot_checks == 3U);
  CHECK(metrics.history_entries_visited == 2U);
  CHECK(metrics.history_deltas_visited == 3U);
  CHECK(metrics.history_relevant_deltas_visited == 1U);
  CHECK(metrics.max_history_entries_visited == 2U);
  CHECK(metrics.max_history_deltas_visited == 3U);
#else
  CHECK(metrics.compatible_snapshot_checks == 0U);
  CHECK(metrics.history_entries_visited == 0U);
#endif
}

void test_writes_apply_after_violation_and_verdict_is_sticky() {
  SnapshotSOWWEngine engine;
  const auto key = bytes({1});
  const std::vector<OwnedEvent> violating_writer{
      read(1, bytes({9}), present(bytes({7}))),
      write(2, key, present(bytes({3}))),
  };
  const auto first = consume(engine, meta(0, session(1), 1), violating_writer);
  expect_transaction_violation(first, ViolationKind::NoCompatibleSnapshot);

  const std::vector<OwnedEvent> observes_committed_write{
      read(1, key, present(bytes({3}))),
  };
  const auto second =
      consume(engine, meta(1, session(2), 1), observes_committed_write);
  expect_transaction_pass(second);
  CHECK(second.verdict->status == ConsistencyStatus::Violation);
  CHECK(second.verdict->first_violation == first.transaction_violation);
}

void test_delete_and_present_empty_state() {
  SnapshotSOWWEngine engine;
  const auto key = bytes({1});
  expect_transaction_pass(
      consume(engine, meta(0, session(1), 1),
              std::vector<OwnedEvent>{write(1, key, present(bytes({})))}));

  const std::vector<OwnedEvent> read_then_delete{
      read(1, key, present(bytes({}))),
      write(2, key, absent()),
  };
  expect_transaction_pass(
      consume(engine, meta(1, session(1), 2), read_then_delete));
  expect_transaction_pass(
      consume(engine, meta(2, session(1), 3),
              std::vector<OwnedEvent>{read(1, key, absent())}));
}

void test_borrowed_event_storage_is_not_retained() {
  SnapshotSOWWEngine engine;
  std::vector<OwnedEvent> first{
      write(1, bytes({1}), present(bytes({2}))),
  };
  auto views = make_views(first);
  expect_transaction_pass(engine.consume(meta(0, session(1), 1), views));

  first[0].key[0]   = std::byte{9};
  first[0].value[0] = std::byte{9};
  std::fill(views.begin(), views.end(), EventView{});

  const std::vector<OwnedEvent> read_original{
      read(1, bytes({1}), present(bytes({2}))),
  };
  expect_transaction_pass(
      consume(engine, meta(1, session(2), 1), read_original));

  SnapshotSOWWEngine diagnostic_engine;
  std::vector<OwnedEvent> bad_read{
      read(1, bytes({4}), present(bytes({5}))),
  };
  const auto diagnostic =
      consume(diagnostic_engine, meta(0, session(1), 1), bad_read);
  expect_transaction_violation(diagnostic, ViolationKind::NoCompatibleSnapshot);
  bad_read[0].key[0] = std::byte{8};
  CHECK(diagnostic.transaction_violation->keys ==
        std::vector<OwnedBytes>{bytes({4})});
}

void test_protocol_errors_are_sticky() {
  SnapshotSOWWEngine engine;
  const auto unexpected =
      consume(engine, meta(1, session(1), 1), std::vector<OwnedEvent>{});
  expect_error(unexpected, EngineErrorCode::UnexpectedCommitSequence);
  CHECK(engine.next_expected_commit_seq() == 0);
  CHECK(!engine.verdict().has_value());

  const auto after_error =
      consume(engine, meta(0, session(1), 1), std::vector<OwnedEvent>{});
  expect_error(after_error, EngineErrorCode::UnexpectedCommitSequence);
  CHECK(after_error.error == unexpected.error);

  engine.reset();
  CHECK(engine.next_expected_commit_seq() == 0);
  CHECK(!engine.error().has_value());
  CHECK(engine.verdict()->status == ConsistencyStatus::Pass);
  expect_transaction_pass(
      consume(engine, meta(0, session(1), 5), std::vector<OwnedEvent>{}));

  const auto duplicate_session_seq =
      consume(engine, meta(1, session(1), 5), std::vector<OwnedEvent>{});
  expect_error(duplicate_session_seq,
               EngineErrorCode::NonIncreasingSessionSequence);
  CHECK(engine.next_expected_commit_seq() == 1);
}

void test_malformed_events_stop_the_engine() {
  SnapshotSOWWEngine engine;
  const auto metadata = meta(0, session(1), 1);

  {
    const std::vector<OwnedEvent> events{
        read(2, bytes({1}), absent()),
        read(2, bytes({2}), absent()),
    };
    expect_error(consume(engine, metadata, events),
                 EngineErrorCode::NonIncreasingEventSequence);
  }

  engine.reset();
  {
    auto invalid = read(1, bytes({1}), absent());
    invalid.op   = static_cast<EventOp>(0);
    expect_error(consume(engine, metadata, std::vector<OwnedEvent>{invalid}),
                 EngineErrorCode::InvalidEventOperation);
  }

  engine.reset();
  {
    auto invalid  = read(1, bytes({1}), absent());
    invalid.state = static_cast<ValueState>(0);
    expect_error(consume(engine, metadata, std::vector<OwnedEvent>{invalid}),
                 EngineErrorCode::InvalidValueState);
  }

  engine.reset();
  {
    auto invalid   = read(1, bytes({1}), absent());
    invalid.source = static_cast<ReadSource>(0);
    expect_error(consume(engine, metadata, std::vector<OwnedEvent>{invalid}),
                 EngineErrorCode::InvalidReadSource);
  }

  engine.reset();
  {
    const auto invalid =
        read(1, bytes({1}),
             TaggedInput{.state = ValueState::Absent, .value = bytes({9})});
    expect_error(consume(engine, metadata, std::vector<OwnedEvent>{invalid}),
                 EngineErrorCode::AbsentValueHasPayload);
  }
  CHECK(engine.next_expected_commit_seq() == 0);
}

void test_interned_state_metrics_and_hard_limit() {
  const std::vector<OwnedEvent> events{
      read(1, bytes({1, 2}), absent()),
      read(2, bytes({1, 2}), absent()),
      write(3, bytes({1, 2}), present(bytes({7, 8, 9}))),
  };

  SnapshotSOWWEngine measured;
  expect_transaction_pass(consume(measured, meta(0, session(1), 1), events));
  const auto measured_metrics = measured.metrics();
  CHECK(measured_metrics.interned_key_count == 1U);
  CHECK(measured_metrics.interned_key_bytes == 2U);
  CHECK(measured_metrics.interned_value_count == 1U);
  CHECK(measured_metrics.interned_value_bytes == 3U);
  CHECK(measured_metrics.current_snapshot_keys == 1U);
  CHECK(measured_metrics.history_entries == 1U);
  CHECK(measured_metrics.history_deltas == 1U);
  CHECK(measured_metrics.history_logical_bytes > 0U);
  CHECK(measured_metrics.retained_state_bytes ==
        measured_metrics.interned_key_bytes +
            measured_metrics.interned_value_bytes +
            measured_metrics.history_logical_bytes);
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
  CHECK(measured_metrics.repeated_reads.size == 0U);
  CHECK(measured_metrics.repeated_reads.probe_groups > 0U);
  CHECK(measured_metrics.snapshot_read_events == 2U);
  CHECK(measured_metrics.unique_snapshot_reads == 1U);
  CHECK(measured_metrics.identical_snapshot_repeats == 1U);
  CHECK(measured_metrics.conflicting_snapshot_repeats == 0U);
  const auto marginal_read_bytes = sizeof(EventDescriptorV1) + 2U;
  CHECK(measured_metrics.snapshot_read_packed_bytes ==
        marginal_read_bytes * 2U);
  CHECK(measured_metrics.duplicate_snapshot_read_packed_bytes ==
        marginal_read_bytes);
  CHECK(measured_metrics.identical_snapshot_repeat_packed_bytes ==
        marginal_read_bytes);
#else
  CHECK(measured_metrics.repeated_reads.probe_groups == 0U);
  CHECK(measured_metrics.snapshot_read_events == 0U);
#endif
  CHECK(measured_metrics.conflicting_snapshot_repeat_packed_bytes == 0U);

  SnapshotSOWWEngine exact_limit(SnapshotSOWWOptions{
      .retained_state_capacity_bytes = measured_metrics.retained_state_bytes,
  });
  expect_transaction_pass(consume(exact_limit, meta(0, session(1), 1), events));
  CHECK(exact_limit.metrics().retained_state_bytes ==
        measured_metrics.retained_state_bytes);

  SnapshotSOWWEngine too_small(SnapshotSOWWOptions{
      .retained_state_capacity_bytes =
          measured_metrics.retained_state_bytes - 1U,
  });
  expect_error(consume(too_small, meta(0, session(1), 1), events),
               EngineErrorCode::RetainedStateLimitExceeded);
  CHECK(!too_small.verdict().has_value());
  CHECK(too_small.next_expected_commit_seq() == 0U);

  measured.reset();
  const auto reset_metrics = measured.metrics();
  CHECK(reset_metrics.retained_state_bytes == 0U);
  CHECK(reset_metrics.interned_key_count == 0U);
  CHECK(reset_metrics.interned_value_count == 0U);
  CHECK(reset_metrics.history_entries == 0U);
}

void test_history_pruning_preserves_boundary_snapshot() {
  SnapshotSOWWEngine engine;
  const auto x = bytes({1});
  const auto y = bytes({2});
  expect_transaction_pass(consume(engine, meta(0, session(9), 1),
                                  {write(1, x, present(bytes({})))}));
  expect_transaction_pass(consume(engine, meta(1, session(1), 0), {}));
  expect_transaction_pass(consume(engine, meta(2, session(2), 0), {}));
  const auto before = engine.metrics();
  CHECK(!engine.pruneHistoryThrough(1).has_value());
  auto metrics = engine.metrics();
  CHECK(metrics.retained_floor == 1U);
  CHECK(metrics.history_entries == 0U);
  CHECK(metrics.reclaimed_history_entries == 1U);
  CHECK(metrics.reclaimed_history_deltas == 1U);
  CHECK(metrics.reclaimed_history_bytes == before.history_logical_bytes);
  CHECK(metrics.retained_state_bytes + metrics.reclaimed_history_bytes ==
        before.retained_state_bytes);
  CHECK(metrics.peak_retained_state_bytes == before.peak_retained_state_bytes);
  CHECK(metrics.interned_value_bytes == before.interned_value_bytes);
  CHECK(metrics.current_snapshot_keys == 1U);

  expect_transaction_pass(consume(
      engine, meta(3, session(2), 1),
      {write(1, x, present(bytes({7}))), write(2, y, present(bytes({8})))}));
  expect_transaction_pass(consume(engine, meta(4, session(2), 2), {}));
  CHECK(!engine.pruneHistoryThrough(1).has_value());
  CHECK(engine.metrics().history_entries == 1U);
  // The slow read-only session still needs the snapshot before commit 3.
  expect_transaction_pass(
      consume(engine, meta(5, session(1), 1),
              {read(1, x, present(bytes({}))), read(2, y, absent())}));
  CHECK(!engine.pruneHistoryThrough(4).has_value());
  CHECK(engine.metrics().history_entries == 0U);

  expect_transaction_pass(
      consume(engine, meta(6, session(2), 3), {write(1, x, absent())}));
  CHECK(!engine.pruneHistoryThrough(5).has_value());
  expect_transaction_pass(consume(
      engine, meta(7, session(1), 2),
      {read(1, x, present(bytes({7}))), read(2, y, present(bytes({8})))}));
  CHECK(!engine.pruneHistoryThrough(6).has_value());
  CHECK(engine.metrics().history_entries == 0U);
  CHECK(!engine.pruneHistoryThrough(6).has_value());
  CHECK(engine.metrics().reclaimed_history_entries == 3U);
  CHECK(engine.next_expected_commit_seq() == 8U);
  engine.reset();
  metrics = engine.metrics();
  CHECK(!metrics.retained_floor.has_value());
  CHECK(metrics.reclaimed_history_entries == 0U);
  CHECK(metrics.reclaimed_history_deltas == 0U);
  CHECK(metrics.reclaimed_history_bytes == 0U);
}

void test_history_pruning_preserves_ww_and_violation_continuation() {
  SnapshotSOWWEngine engine;
  const auto x = bytes({1});
  expect_transaction_pass(consume(engine, meta(0, session(1), 1),
                                  {write(1, x, present(bytes({7})))}));
  CHECK(!engine.pruneHistoryThrough(0).has_value());
  const auto result =
      consume(engine, meta(1, session(2), 1),
              {read(1, x, absent()), write(2, x, present(bytes({8})))});
  expect_transaction_violation(result, ViolationKind::NoCompatibleSnapshot);
  CHECK(result.transaction_violation->write_write_lower_bound == 0U);
  CHECK(!engine.pruneHistoryThrough(1).has_value());
  const auto next = consume(engine, meta(2, session(2), 2),
                            {read(1, x, present(bytes({8})))});
  expect_transaction_pass(next);
  CHECK(next.verdict->status == ConsistencyStatus::Violation);
}

void test_history_pruning_rejects_invalid_frontiers_and_old_bounds() {
  SnapshotSOWWEngine engine;
  CHECK(engine.pruneHistoryThrough(0)->code ==
        EngineErrorCode::InvalidHistoryFrontier);
  CHECK(!engine.verdict().has_value());
  expect_error(consume(engine, meta(0, session(1), 1), {}),
               EngineErrorCode::InvalidHistoryFrontier);
  engine.reset();
  expect_transaction_pass(consume(engine, meta(0, session(1), 1), {}));
  CHECK(engine.pruneHistoryThrough(1)->code ==
        EngineErrorCode::InvalidHistoryFrontier);
  engine.reset();
  expect_transaction_pass(consume(engine, meta(0, session(1), 1), {}));
  expect_transaction_pass(consume(engine, meta(1, session(2), 1), {}));
  CHECK(!engine.pruneHistoryThrough(1).has_value());
  CHECK(engine.pruneHistoryThrough(0)->code ==
        EngineErrorCode::InvalidHistoryFrontier);

  for (const bool known_session : {false, true}) {
    engine.reset();
    expect_transaction_pass(consume(engine, meta(0, session(1), 1), {}));
    expect_transaction_pass(consume(engine, meta(1, session(2), 1), {}));
    CHECK(!engine.pruneHistoryThrough(1).has_value());
    // Even if the latest snapshot matches, missing history is a protocol error.
    expect_error(
        consume(engine, meta(2, session(known_session ? 1 : 3), 2), {}),
        EngineErrorCode::HistoryUnavailable);
    CHECK(!engine.verdict().has_value());
    CHECK(engine.next_expected_commit_seq() == 2U);
    CHECK(engine.pruneHistoryThrough(1)->code ==
          EngineErrorCode::HistoryUnavailable);
  }
}

void test_history_pruning_reuses_byte_budget() {
  const std::vector<OwnedEvent> writes{
      write(1, bytes({1}), present(bytes({2})))};
  SnapshotSOWWEngine measured;
  expect_transaction_pass(consume(measured, meta(0, session(1), 1), writes));
  const auto capacity = measured.metrics().retained_state_bytes;
  SnapshotSOWWEngine bounded(
      SnapshotSOWWOptions{.retained_state_capacity_bytes = capacity});
  for (CommitSeq seq = 0; seq < 10000; ++seq) {
    expect_transaction_pass(
        consume(bounded, meta(seq, session(1), seq + 1), writes));
    CHECK(!bounded.pruneHistoryThrough(seq).has_value());
    CHECK(bounded.metrics().history_logical_bytes == 0U);
  }
  CHECK(bounded.metrics().peak_retained_state_bytes == capacity);
  CHECK(bounded.metrics().retained_state_bytes == 2U);
  CHECK(bounded.metrics().reclaimed_history_entries == 10000U);
}

void test_value_gc_shared_empty_and_bootstrap_values() {
  SnapshotSOWWEngine engine;
  const auto x = bytes({1});
  const auto y = bytes({2});
  const auto a = bytes({7, 8, 9});
  const auto b = bytes({4, 5});
  expect_transaction_pass(consume(engine, meta(0, session(1), 1),
      {write(1, x, present(a)), write(2, y, present(a))}));
  CHECK(!engine.pruneHistoryThrough(0).has_value());
  CHECK(engine.metrics().interned_value_count == 1U);
  CHECK(engine.metrics().reclaimed_value_count == 0U);

  expect_transaction_pass(consume(engine, meta(1, session(1), 2),
      {write(1, x, present(b))}));
  CHECK(!engine.pruneHistoryThrough(1).has_value());
  CHECK(engine.metrics().interned_value_count == 2U);
  CHECK(engine.metrics().reclaimed_value_count == 0U); // y still owns a.

  expect_transaction_pass(consume(engine, meta(2, session(1), 3),
      {write(1, y, present(a))})); // Same-value overwrite adds a delta reference.
  CHECK(!engine.pruneHistoryThrough(2).has_value());
  CHECK(engine.metrics().interned_value_count == 2U);
  expect_transaction_pass(consume(engine, meta(3, session(1), 4),
      {read(1, y, present(a)), write(2, y, absent())}));
  CHECK(engine.metrics(true).memory_profile->history_only_value_bytes == 3U);
  CHECK(!engine.pruneHistoryThrough(3).has_value());
  CHECK(engine.metrics().interned_value_count == 1U);
  CHECK(engine.metrics().reclaimed_value_count == 1U);
  CHECK(engine.metrics().reclaimed_value_bytes == 3U);

  expect_transaction_pass(consume(engine, meta(4, session(1), 5),
      {read(1, x, present(b)), write(2, x, present(bytes({})))}));
  CHECK(!engine.pruneHistoryThrough(4).has_value());
  CHECK(engine.metrics().interned_value_count == 1U); // Present empty value.
  CHECK(engine.metrics().interned_value_bytes == 0U);
  expect_transaction_pass(consume(engine, meta(5, session(1), 6),
      {read(1, x, present(bytes({}))), write(2, x, absent())}));
  CHECK(!engine.pruneHistoryThrough(5).has_value());
  CHECK(engine.metrics().interned_value_count == 0U);
  CHECK(engine.metrics().reclaimed_value_count == 3U);
  CHECK(engine.metrics().reclaimed_value_bytes == 5U);
  CHECK(engine.metrics(true).memory_profile->unreferenced_value_count == 0U);

  // A reclaimed bootstrap value can be interned again, including ID reuse.
  expect_transaction_pass(consume(engine, meta(6, session(1), 7),
      {read(1, x, absent()), write(2, x, present(a))}));
  CHECK(!engine.pruneHistoryThrough(6).has_value());
  expect_transaction_pass(consume(engine, meta(7, session(1), 8),
      {read(1, x, present(a))}));
  engine.reset();
  CHECK(engine.metrics().reclaimed_value_count == 0U);
  CHECK(engine.metrics().reclaimed_value_bytes == 0U);
  CHECK(engine.metrics().interned_value_count == 0U);
  expect_transaction_pass(consume(engine, meta(0, session(1), 1),
      {read(1, x, absent()), write(2, x, present(bytes({})))}));
  CHECK(!engine.pruneHistoryThrough(0).has_value());
  CHECK(engine.metrics().interned_value_count == 1U);
}

void test_value_gc_bounds_slots_and_reuses_payload_budget() {
  const auto x = bytes({1});
  SnapshotSOWWEngine measured;
  expect_transaction_pass(consume(measured, meta(0, session(1), 1),
      {write(1, x, present(bytes({0, 0})))}));
  CHECK(!measured.pruneHistoryThrough(0).has_value());
  expect_transaction_pass(consume(measured, meta(1, session(1), 2),
      {write(1, x, present(bytes({1, 0})))}));
  const auto budget = measured.metrics().peak_retained_state_bytes;
  SnapshotSOWWEngine engine(SnapshotSOWWOptions{.retained_state_capacity_bytes = budget});
  std::optional<SnapshotSOWWMemoryProfile> initial_capacity;
  for (CommitSeq seq = 0; seq < 2048; ++seq) {
    const auto value = bytes({static_cast<unsigned int>(seq & 255U),
                              static_cast<unsigned int>((seq >> 8U) & 255U)});
    expect_transaction_pass(consume(engine, meta(seq, session(1), seq + 1),
        {write(1, x, present(value))}));
    CHECK(!engine.pruneHistoryThrough(seq).has_value());
    CHECK(engine.metrics().interned_value_count == 1U);
    CHECK(engine.metrics().retained_state_bytes == 3U);
    if (seq == 1U) initial_capacity = engine.metrics(true).memory_profile;
  }
  const auto metrics = engine.metrics(true);
  CHECK(metrics.reclaimed_value_count == 2047U);
  CHECK(metrics.reclaimed_value_bytes == 4094U);
  CHECK(metrics.memory_profile->unreferenced_value_count == 0U);
  CHECK(metrics.memory_profile->value_descriptor_capacity_bytes ==
        initial_capacity->value_descriptor_capacity_bytes);
  CHECK(metrics.memory_profile->value_index_capacity_bytes ==
        initial_capacity->value_index_capacity_bytes);
  CHECK(metrics.memory_profile->value_reference_capacity_bytes ==
        initial_capacity->value_reference_capacity_bytes);
}

void test_value_gc_collision_chain_wraparound_and_id_reuse() {
  // All payloads hash to the final bucket for tables of up to 128 slots.
  // Deleting arbitrary members must preserve chains that wrap through slot 0.
  std::vector<OwnedBytes> colliding;
  for (std::uint32_t candidate = 0; candidate < 100000U && colliding.size() < 48U;
       ++candidate) {
    auto value = bytes({candidate & 255U, (candidate >> 8U) & 255U,
                        (candidate >> 16U) & 255U});
    if ((repeated_read_hash(ByteView(value)) & 127U) == 127U) {
      colliding.push_back(std::move(value));
    }
  }
  CHECK(colliding.size() == 48U);
  if (colliding.size() != 48U) return;
  std::array<std::optional<OwnedBytes>, 24> state;
  SnapshotSOWWEngine engine;
  CommitSeq seq = 0;
  std::vector<OwnedEvent> events;
  for (std::size_t key = 0; key < state.size(); ++key) {
    state[key] = colliding[key];
    events.push_back(write(events.size() + 1U,
        bytes({static_cast<unsigned int>(key)}), present(*state[key])));
  }
  expect_transaction_pass(consume(engine, meta(seq, session(1), seq + 1), events));
  CHECK(!engine.pruneHistoryThrough(seq++).has_value());
  const auto capacity = engine.metrics(true).memory_profile;
  const auto read_all = [&] {
    std::vector<OwnedEvent> reads;
    for (std::size_t key = 0; key < state.size(); ++key) {
      reads.push_back(read(reads.size() + 1U,
          bytes({static_cast<unsigned int>(key)}),
          state[key] ? present(*state[key]) : absent()));
    }
    return reads;
  };
  for (std::size_t round = 0; round < 24U; ++round) {
    events = read_all();
    for (std::size_t key = round % 3U; key < state.size(); key += 3U) {
      events.push_back(write(events.size() + 1U,
          bytes({static_cast<unsigned int>(key)}), absent()));
      state[key].reset();
    }
    expect_transaction_pass(consume(engine, meta(seq, session(1), seq + 1), events));
    CHECK(!engine.pruneHistoryThrough(seq++).has_value());
    CHECK(engine.metrics().interned_value_count == 16U);
    events = read_all();
    for (std::size_t key = round % 3U; key < state.size(); key += 3U) {
      state[key] = colliding[key + (round % 2U == 0U ? 24U : 0U)];
      events.push_back(write(events.size() + 1U,
          bytes({static_cast<unsigned int>(key)}), present(*state[key])));
    }
    expect_transaction_pass(consume(engine, meta(seq, session(1), seq + 1), events));
    CHECK(!engine.pruneHistoryThrough(seq++).has_value());
    CHECK(engine.metrics().interned_value_count == 24U);
  }
  expect_transaction_pass(consume(engine, meta(seq, session(1), seq + 1), read_all()));
  const auto metrics = engine.metrics(true);
  CHECK(metrics.reclaimed_value_count == 192U);
  CHECK(metrics.memory_profile->snapshot_value_count == 24U);
  CHECK(metrics.memory_profile->history_only_value_count == 0U);
  CHECK(metrics.memory_profile->unreferenced_value_count == 0U);
  CHECK(metrics.memory_profile->value_descriptor_capacity_bytes ==
        capacity->value_descriptor_capacity_bytes);
  CHECK(metrics.memory_profile->value_index_capacity_bytes ==
        capacity->value_index_capacity_bytes);
}

[[nodiscard]] TaggedInput random_tagged_value(std::mt19937_64 &random) {
  switch (random() % 5U) {
  case 0:
    return absent();
  case 1:
    return present(bytes({}));
  case 2:
    return present(bytes({0}));
  case 3:
    return present(bytes({1}));
  default:
    return present(bytes({2}));
  }
}

void test_randomized_differential_against_materialized_reference() {
  constexpr std::size_t history_count            = 80;
  constexpr std::size_t transactions_per_history = 80;
  constexpr std::size_t session_count            = 4;
  const std::array<OwnedBytes, 5> key_space{bytes({}), bytes({0}), bytes({1}),
                                            bytes({2}), bytes({3})};

  std::mt19937_64 random(0x5a17d1ffU);
  for (std::size_t history_index = 0; history_index < history_count;
       ++history_index) {
    SnapshotSOWWEngine engine;
    SnapshotSOWWEngine gc_engine;
    ReferenceSOWWModel reference;
    std::array<std::uint64_t, session_count> session_sequences{};
    std::array<std::optional<CommitSeq>, session_count> latest{};

    for (std::size_t txn_index = 0; txn_index < transactions_per_history;
         ++txn_index) {
      const auto session_index =
          static_cast<std::size_t>(random() % session_count);
      ++session_sequences[session_index];
      const auto metadata =
          meta(static_cast<CommitSeq>(txn_index),
               session(static_cast<std::uint64_t>(session_index + 1U)),
               session_sequences[session_index]);

      std::vector<OwnedEvent> events;
      const auto event_count = static_cast<std::size_t>(random() % 11U);
      events.reserve(event_count);
      EventSeq sequence = 0;
      for (std::size_t event_index = 0; event_index < event_count;
           ++event_index) {
        sequence += static_cast<EventSeq>(1U + (random() % 3U));
        const auto key_index =
            static_cast<std::size_t>(random() % key_space.size());
        const auto operation = random() % 3U;
        if (operation == 0U) {
          events.push_back(read(sequence, key_space[key_index],
                                random_tagged_value(random),
                                ReadSource::Snapshot));
        } else if (operation == 1U) {
          events.push_back(read(sequence, key_space[key_index],
                                random_tagged_value(random),
                                ReadSource::Local));
        } else {
          events.push_back(write(sequence, key_space[key_index],
                                 random_tagged_value(random)));
        }
      }

      const auto views    = make_views(events);
      const auto expected = reference.consume(metadata, views);
      const auto actual   = engine.consume(metadata, views);
      const auto pruned   = gc_engine.consume(metadata, views);
      CHECK(pruned.error == actual.error);
      CHECK(pruned.verdict == actual.verdict);
      CHECK(pruned.transaction_violation == actual.transaction_violation);
      latest[session_index] = metadata.commit_seq;
      if (std::all_of(latest.begin(), latest.end(),
                      [](const auto &seq) { return seq.has_value(); })) {
        const auto frontier = **std::min_element(latest.begin(), latest.end());
        CHECK(!gc_engine.pruneHistoryThrough(frontier).has_value());
      }
      CHECK(actual.ok());
      CHECK(actual.transaction_violation.has_value() ==
            expected.transaction_violation.has_value());
      if (actual.transaction_violation.has_value() &&
          expected.transaction_violation.has_value()) {
        CHECK(actual.transaction_violation->kind ==
              *expected.transaction_violation);
      }
      CHECK(actual.verdict.has_value());
      if (actual.verdict.has_value()) {
        CHECK((actual.verdict->status == ConsistencyStatus::Violation) ==
              expected.aggregate_violation);
      }

      // Poison producer-owned storage after each consume. Subsequent
      // differential steps also exercise the no-borrowed-retention contract.
      for (auto &event : events) {
        std::fill(event.key.begin(), event.key.end(), std::byte{0xff});
        std::fill(event.value.begin(), event.value.end(), std::byte{0xee});
      }
    }
    CHECK(engine.next_expected_commit_seq() == transactions_per_history);
  }
}

} // namespace

int main() {
  test_empty_initial_state_and_first_read_only_checked();
  test_present_empty_is_distinct_from_absent();
  test_local_reads_are_ignored();
  test_scratch_sizing_uses_snapshot_reads_and_writes();
  test_final_write_per_key();
  test_session_order_lower_bound();
  test_write_write_lower_bound();
  test_all_reads_must_match_one_prefix();
  test_compatible_snapshot_history_traversal_metrics();
  test_writes_apply_after_violation_and_verdict_is_sticky();
  test_delete_and_present_empty_state();
  test_borrowed_event_storage_is_not_retained();
  test_protocol_errors_are_sticky();
  test_malformed_events_stop_the_engine();
  test_interned_state_metrics_and_hard_limit();
  test_history_pruning_preserves_boundary_snapshot();
  test_history_pruning_preserves_ww_and_violation_continuation();
  test_history_pruning_rejects_invalid_frontiers_and_old_bounds();
  test_history_pruning_reuses_byte_budget();
  test_value_gc_shared_empty_and_bootstrap_values();
  test_value_gc_bounds_slots_and_reuses_payload_budget();
  test_value_gc_collision_chain_wraparound_and_id_reuse();
  test_randomized_differential_against_materialized_reference();
  failures += run_bounded_queue_tests();
  failures += run_byte_budget_tests();
  failures += run_c_api_tests();
  failures += run_order_gate_tests();
  failures += run_packed_batch_tests();
  failures += run_repeated_read_table_tests();
  failures += run_runtime_tests();
  failures += run_serialized_order_tests();

  if (failures != 0) {
    std::cerr << failures << " test failure(s)\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
