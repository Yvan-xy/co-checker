#include "co_checker/online/engine.h"
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
  CHECK(measured_metrics.repeated_reads.size == 0U);
  CHECK(measured_metrics.repeated_reads.probe_groups > 0U);

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
    ReferenceSOWWModel reference;
    std::array<std::uint64_t, session_count> session_sequences{};

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
  test_final_write_per_key();
  test_session_order_lower_bound();
  test_write_write_lower_bound();
  test_all_reads_must_match_one_prefix();
  test_writes_apply_after_violation_and_verdict_is_sticky();
  test_delete_and_present_empty_state();
  test_borrowed_event_storage_is_not_retained();
  test_protocol_errors_are_sticky();
  test_malformed_events_stop_the_engine();
  test_interned_state_metrics_and_hard_limit();
  test_randomized_differential_against_materialized_reference();
  failures += run_bounded_queue_tests();
  failures += run_byte_budget_tests();
  failures += run_c_api_tests();
  failures += run_order_gate_tests();
  failures += run_packed_batch_tests();
  failures += run_repeated_read_table_tests();
  failures += run_runtime_tests();

  if (failures != 0) {
    std::cerr << failures << " test failure(s)\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
