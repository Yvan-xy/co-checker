#include "co_checker/online/runtime.h"

#include "co_checker/online/engine.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace co_checker::online;
using namespace std::chrono_literals;

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void check(const bool condition, const std::string_view expression,
           const char *file, const int line) {
  if (condition) {
    return;
  }
  std::ostringstream message;
  message << file << ':' << line << ": check failed: " << expression;
  throw TestFailure(message.str());
}

#define CHECK(expression)                                                      \
  check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text) {
  std::vector<std::byte> result;
  result.reserve(text.size());
  for (const auto character : text) {
    result.push_back(
        static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  return result;
}

[[nodiscard]] ByteView view(const std::vector<std::byte> &value) noexcept {
  return ByteView{value.data(), value.size()};
}

[[nodiscard]] EntityUid uid(const std::uint64_t value,
                            const std::uint64_t domain) {
  return EntityUid{{domain, value}};
}

[[nodiscard]] CommitOrderToken token(const std::uint64_t timestamp) {
  return CommitOrderToken{1, timestamp, std::nullopt};
}

[[nodiscard]] OrderedTxnMeta
metadata(const std::uint64_t transaction_id, const ProducerId producer_id,
         const ProducerSeq producer_sequence,
         const std::uint64_t commit_timestamp,
         const std::uint64_t session_counter  = 0,
         const std::uint64_t session_sequence = 1) {
  OrderedTxnMeta result;
  result.commit_seq     = kUnassignedCommitSeq;
  result.transaction_id = uid(transaction_id, 0x1000U);
  result.attempt_id     = uid(transaction_id, 0x2000U);
  result.session_id =
      SessionUid{RuntimeEpoch{{0xabcU, 0xdefU}}, producer_id,
                 session_counter == 0 ? transaction_id : session_counter};
  result.session_seq  = session_sequence;
  result.commit_token = token(commit_timestamp);
  result.producer_id  = producer_id;
  result.producer_seq = producer_sequence;
  return result;
}

void require_build(const BatchBuildStatus &status) { CHECK(status.ok()); }

[[nodiscard]] std::unique_ptr<VectorBatch>
empty_batch(const OrderedTxnMeta &meta) {
  VectorBatchBuilder builder(meta);
  auto sealed = builder.seal();
  CHECK(sealed.ok());
  return std::move(sealed.batch);
}

[[nodiscard]] std::unique_ptr<VectorBatch>
read_batch(const OrderedTxnMeta &meta, const std::string_view key_text,
           const std::optional<std::string_view> value_text) {
  const auto key = bytes(key_text);
  VectorBatchBuilder builder(meta);
  if (value_text.has_value()) {
    const auto value = bytes(*value_text);
    require_build(builder.append_read(1, view(key), PresentBytes{view(value)},
                                      ReadSource::Snapshot));
  } else {
    require_build(
        builder.append_read(1, view(key), Absent{}, ReadSource::Snapshot));
  }
  auto sealed = builder.seal();
  CHECK(sealed.ok());
  return std::move(sealed.batch);
}

[[nodiscard]] std::unique_ptr<VectorBatch>
write_batch(const OrderedTxnMeta &meta, const std::string_view key_text,
            const std::optional<std::string_view> value_text) {
  const auto key = bytes(key_text);
  VectorBatchBuilder builder(meta);
  if (value_text.has_value()) {
    const auto value = bytes(*value_text);
    require_build(
        builder.append_write(1, view(key), PresentBytes{view(value)}));
  } else {
    require_build(builder.append_write(1, view(key), Absent{}));
  }
  auto sealed = builder.seal();
  CHECK(sealed.ok());
  return std::move(sealed.batch);
}

[[nodiscard]] std::unique_ptr<ChunkBatch>
chunk_write_batch(const std::shared_ptr<ChunkPool> &pool,
                  const OrderedTxnMeta &meta, const std::string_view key_text,
                  const std::string_view value_text) {
  const auto key   = bytes(key_text);
  const auto value = bytes(value_text);
  ChunkBatchBuilder builder(pool, meta);
  require_build(builder.append_write(1, view(key), PresentBytes{view(value)}));
  auto sealed = builder.seal();
  CHECK(sealed.ok());
  return std::move(sealed.batch);
}

[[nodiscard]] RuntimeOptions options(const std::size_t queue_capacity,
                                     const std::size_t reorder_capacity,
                                     const bool checking_enabled = true) {
  RuntimeOptions result;
  result.queue.capacity_bytes   = queue_capacity;
  result.queue.push_timeout     = 30s;
  result.reorder_capacity_bytes = reorder_capacity;
  result.pipeline_mode =
      checking_enabled ? PipelineMode::Full : PipelineMode::OrderOnly;
  result.ordering_protocol = OrderingProtocol::FrontierReorder;
  return result;
}

[[nodiscard]] RuntimeOptions
serialized_options(const std::size_t queue_capacity,
                   const PipelineMode pipeline_mode = PipelineMode::Full) {
  RuntimeOptions result;
  result.queue.capacity_bytes      = queue_capacity;
  result.queue.push_timeout        = 30s;
  result.reorder_capacity_bytes    = 0;
  result.pipeline_mode             = pipeline_mode;
  result.ordering_protocol         =
      OrderingProtocol::SerializedGlobalLock;
  result.expected_runtime_epoch    = RuntimeEpoch{{0xabcU, 0xdefU}};
  result.expected_cluster_epoch    = 1;
  return result;
}

void require_ok(const RuntimeControlResult &result) {
  if (result.error.has_value()) {
    std::cerr << "unexpected runtime control error: " << result.error->message
              << '\n';
  }
  CHECK(result.ok());
}

void require_accepted(RuntimeBatchSubmitResult result) {
  if (result.error.has_value()) {
    std::cerr << "unexpected runtime submission error: "
              << result.error->message << '\n';
  }
  CHECK(result.accepted);
  CHECK(!result.returned_batch.valid());
}

void close_cleanly(OnlineRuntime &runtime) {
  require_ok(runtime.quiescentBarrier());
  require_ok(runtime.shutdown());
  CHECK(!runtime.error().has_value());
}

void test_reordered_arrival_matches_commit_sorted_packed_engine() {
  const auto later_meta   = metadata(1, 1, 1, 30);
  const auto earlier_meta = metadata(2, 1, 2, 20);
  auto later              = write_batch(later_meta, "key", "later");
  auto earlier            = write_batch(earlier_meta, "key", "earlier");
  auto direct_later       = write_batch(later_meta, "key", "later");
  auto direct_earlier     = write_batch(earlier_meta, "key", "earlier");

  SnapshotSOWWEngine direct;
  const auto direct_first =
      direct.consume(direct_earlier->view().with_commit_seq(0));
  const auto direct_second =
      direct.consume(direct_later->view().with_commit_seq(1));
  CHECK(direct_first.ok());
  CHECK(direct_second.ok());

  OnlineRuntime runtime(options(1U << 20U, 1U << 20U), {1});
  require_ok(runtime.registerActiveStart(later_meta.attempt_id, 1, token(10)));
  require_ok(
      runtime.registerActiveStart(earlier_meta.attempt_id, 1, token(11)));
  require_accepted(runtime.submitCommitted(std::move(later), false));
  require_accepted(runtime.submitCommitted(std::move(earlier), false));
  require_ok(runtime.updateDatabaseFrontier(token(30)));
  require_ok(runtime.updateProducerDeliveryFrontier(1, token(30), 2));
  require_ok(runtime.flush());

  CHECK(runtime.verdict() == direct.verdict());
  const auto metrics = runtime.metrics();
  CHECK(metrics.ordered_transactions == 2U);
  CHECK(metrics.checked_transactions == 2U);
  CHECK(metrics.checked_events == 2U);
  CHECK(metrics.gate.next_commit_seq == 2U);
  CHECK(metrics.gate.pending_transactions == 0U);
  CHECK(metrics.max_commit_token_lag >= 10U);
  CHECK(metrics.last_checked_token == later_meta.commit_token);
  close_cleanly(runtime);
}

void test_delayed_read_only_start_blocks_later_write_until_publication() {
  const auto write_meta = metadata(10, 1, 1, 20);
  const auto read_meta  = metadata(11, 1, 2, 10);
  auto write            = write_batch(write_meta, "key", "value");
  auto read             = read_batch(read_meta, "key", std::nullopt);

  OnlineRuntime runtime(options(1U << 20U, 1U << 20U), {1});
  require_ok(runtime.registerActiveStart(read_meta.attempt_id, 1, token(10)));
  require_ok(runtime.registerActiveStart(write_meta.attempt_id, 1, token(15)));
  require_accepted(runtime.submitCommitted(std::move(write), false));
  require_ok(runtime.updateDatabaseFrontier(token(30)));
  require_ok(runtime.updateProducerDeliveryFrontier(1, token(30), 1));
  require_ok(runtime.flush());

  auto blocked = runtime.metrics();
  CHECK(blocked.checked_transactions == 0U);
  CHECK(blocked.gate.pending_transactions == 1U);
  CHECK(blocked.gate.active_starts == 1U);

  require_accepted(runtime.submitCommitted(std::move(read), true));
  require_ok(runtime.flush());
  require_ok(runtime.updateProducerDeliveryFrontier(1, token(30), 2));
  const auto released = runtime.metrics();
  CHECK(released.checked_transactions == 2U);
  CHECK(released.gate.pending_transactions == 0U);
  CHECK(released.gate.active_starts == 0U);
  CHECK(runtime.verdict()->status == ConsistencyStatus::Pass);
  close_cleanly(runtime);
}

void test_violation_is_sticky_but_later_transactions_are_checked() {
  const auto violating_meta = metadata(20, 1, 1, 10);
  const auto later_meta     = metadata(21, 1, 2, 20);
  auto violating = read_batch(violating_meta, "missing", "impossible");
  auto later     = empty_batch(later_meta);

  OnlineRuntime runtime(options(1U << 20U, 1U << 20U), {1});
  require_ok(
      runtime.registerActiveStart(violating_meta.attempt_id, 1, token(10)));
  require_ok(runtime.registerActiveStart(later_meta.attempt_id, 1, token(20)));
  require_accepted(runtime.submitCommitted(std::move(violating), true));
  require_accepted(runtime.submitCommitted(std::move(later), true));
  require_ok(runtime.updateDatabaseFrontier(token(20)));
  require_ok(runtime.updateProducerDeliveryFrontier(1, token(20), 2));
  require_ok(runtime.flush());

  const auto verdict = runtime.verdict();
  CHECK(verdict.has_value());
  CHECK(verdict->status == ConsistencyStatus::Violation);
  CHECK(verdict->first_violation.has_value());
  CHECK(verdict->first_violation->transaction_id ==
        violating_meta.transaction_id);
  const auto metrics = runtime.metrics();
  CHECK(metrics.checked_transactions == 2U);
  CHECK(metrics.violating_transactions == 1U);
  close_cleanly(runtime);
}

void test_read_only_declaration_must_match_validated_write_count() {
  const auto meta = metadata(30, 1, 1, 20);
  auto batch      = write_batch(meta, "key", "value");
  OnlineRuntime runtime(options(1U << 20U, 1U << 20U), {1});
  require_ok(runtime.registerActiveStart(meta.attempt_id, 1, token(20)));

  const auto submitted = runtime.submitCommitted(std::move(batch), true);
  CHECK(submitted.accepted);
  const auto flushed = runtime.flush();
  CHECK(!flushed.ok());
  CHECK(flushed.error.has_value());
  CHECK(flushed.error->code == RuntimeErrorCode::ProtocolFailure);
  CHECK(!runtime.verdict().has_value());
  const auto shutdown = runtime.shutdown();
  CHECK(!shutdown.ok());
  CHECK(shutdown.error == flushed.error);
}

void test_write_token_kind_allows_zero_normalized_writes() {
  const auto meta = metadata(31, 1, 1, 20);
  auto batch      = empty_batch(meta);
  OnlineRuntime runtime(options(1U << 20U, 1U << 20U), {1});
  require_ok(runtime.registerActiveStart(meta.attempt_id, 1, token(10)));
  require_accepted(runtime.submitCommitted(std::move(batch), false));
  require_ok(runtime.updateDatabaseFrontier(token(20)));
  require_ok(runtime.updateProducerDeliveryFrontier(1, token(20), 1));
  require_ok(runtime.flush());
  CHECK(runtime.metrics().checked_transactions == 1U);
  CHECK(runtime.verdict()->status == ConsistencyStatus::Pass);
  close_cleanly(runtime);
}

void test_orderer_only_recycles_owner_and_budget_without_a_pass_verdict() {
  const auto meta     = metadata(40, 1, 1, 20);
  auto pool           = std::make_shared<ChunkPool>(64);
  auto batch          = chunk_write_batch(pool, meta, "key", "value");
  const auto reserved = batch->reserved_bytes();
  ByteBudget budget(
      ByteBudgetOptions{.capacity_bytes = reserved, .reservation_timeout = 1s});
  auto reservation = budget.reserve(reserved);
  CHECK(reservation.ok());

  OnlineRuntime runtime(options(reserved, reserved, false), {1});
  require_ok(runtime.registerActiveStart(meta.attempt_id, 1, token(10)));
  require_accepted(runtime.submitCommitted(
      PackedBatchEnvelope(std::move(batch), std::move(reservation.lease)),
      false));
  CHECK(budget.metrics().active_leases == 1U);
  require_ok(runtime.updateDatabaseFrontier(token(20)));
  require_ok(runtime.updateProducerDeliveryFrontier(1, token(20), 1));

  const auto metrics = runtime.metrics();
  CHECK(!metrics.checking_enabled);
  CHECK(metrics.ordered_transactions == 1U);
  CHECK(metrics.checked_transactions == 0U);
  CHECK(metrics.last_ordered_token == meta.commit_token);
  CHECK(!metrics.last_checked_token.has_value());
  CHECK(!runtime.verdict().has_value());
  CHECK(!runtime.error().has_value());
  CHECK(budget.metrics().reserved_bytes == 0U);
  CHECK(budget.metrics().active_leases == 0U);
  CHECK(pool->stats().recycles >= 1U);
  close_cleanly(runtime);
}

void test_budgeted_envelope_move_assignment_retires_old_pair() {
  auto first_pool  = std::make_shared<ChunkPool>(64);
  auto second_pool = std::make_shared<ChunkPool>(64);
  auto first = chunk_write_batch(first_pool, metadata(50, 1, 1, 20), "a", "1");
  auto second =
      chunk_write_batch(second_pool, metadata(51, 1, 2, 30), "b", "2");
  const auto first_reserved  = first->reserved_bytes();
  const auto second_reserved = second->reserved_bytes();
  ByteBudget budget(ByteBudgetOptions{
      .capacity_bytes      = first_reserved + second_reserved,
      .reservation_timeout = 1s,
  });
  auto first_lease  = budget.reserve(first_reserved);
  auto second_lease = budget.reserve(second_reserved);
  CHECK(first_lease.ok());
  CHECK(second_lease.ok());

  {
    PackedBatchEnvelope target(std::move(first), std::move(first_lease.lease));
    PackedBatchEnvelope source(std::move(second),
                               std::move(second_lease.lease));
    CHECK(budget.metrics().active_leases == 2U);
    target = std::move(source);
    CHECK(!source.valid());
    CHECK(target.valid());
    CHECK(target.budget_lease_bytes() == second_reserved);
    CHECK(budget.metrics().active_leases == 1U);
    CHECK(budget.metrics().reserved_bytes == second_reserved);
    CHECK(first_pool->stats().recycles >= 1U);
    CHECK(second_pool->stats().recycles == 0U);
  }
  CHECK(budget.metrics().active_leases == 0U);
  CHECK(budget.metrics().reserved_bytes == 0U);
  CHECK(second_pool->stats().recycles >= 1U);
}

void test_multi_producer_saturation_is_lossless() {
  constexpr std::size_t producer_count = 4;
  constexpr std::size_t per_producer   = 48;
  const auto transaction_count         = producer_count * per_producer;

  std::vector<std::vector<OrderedTxnMeta>> all_meta(producer_count);
  std::vector<std::vector<std::unique_ptr<VectorBatch>>> batches(
      producer_count);
  std::size_t batch_charge = 0;
  std::size_t total_charge = 0;
  for (std::size_t producer_index = 0; producer_index < producer_count;
       ++producer_index) {
    const auto producer = static_cast<ProducerId>(producer_index + 1U);
    all_meta[producer_index].reserve(per_producer);
    batches[producer_index].reserve(per_producer);
    for (std::size_t sequence_index = 0; sequence_index < per_producer;
         ++sequence_index) {
      const auto id = static_cast<std::uint64_t>(
          1000U + producer_index * per_producer + sequence_index);
      const auto producer_sequence =
          static_cast<ProducerSeq>(sequence_index + 1U);
      const auto commit_timestamp = id + 1000U;
      auto meta  = metadata(id, producer, producer_sequence, commit_timestamp);
      auto batch = empty_batch(meta);
      if (batch_charge == 0U) {
        batch_charge = batch->reserved_bytes();
      }
      CHECK(batch->reserved_bytes() == batch_charge);
      total_charge += batch->reserved_bytes();
      all_meta[producer_index].push_back(meta);
      batches[producer_index].push_back(std::move(batch));
    }
  }

  std::vector<ProducerId> producers;
  producers.reserve(producer_count);
  for (std::size_t index = 0; index < producer_count; ++index) {
    producers.push_back(static_cast<ProducerId>(index + 1U));
  }
  OnlineRuntime runtime(options(batch_charge, total_charge), producers);
  for (const auto &producer_meta : all_meta) {
    for (const auto &meta : producer_meta) {
      require_ok(runtime.registerActiveStart(meta.attempt_id, meta.producer_id,
                                             meta.commit_token));
    }
  }

  std::atomic<std::size_t> submission_failures{0};
  std::vector<std::thread> threads;
  threads.reserve(producer_count);
  for (std::size_t producer_index = 0; producer_index < producer_count;
       ++producer_index) {
    threads.emplace_back([&, producer_index] {
      for (auto &batch : batches[producer_index]) {
        auto result = runtime.submitCommitted(std::move(batch), true);
        if (!result.accepted || result.returned_batch.valid()) {
          submission_failures.fetch_add(1U, std::memory_order_relaxed);
          return;
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  if (const auto error = runtime.error(); error.has_value()) {
    std::cerr << "saturation runtime error: " << error->message << '\n';
  }
  CHECK(submission_failures.load(std::memory_order_relaxed) == 0U);
  require_ok(runtime.flush());
  auto metrics = runtime.metrics();
  CHECK(metrics.published_transactions == transaction_count);
  CHECK(metrics.gate.pending_transactions == transaction_count);
  CHECK(metrics.queue.high_water_queued_bytes == batch_charge);
  CHECK(metrics.queue.high_water_queued_bytes <= metrics.queue.capacity_bytes);

  std::atomic<bool> stop_metrics{false};
  std::atomic<std::size_t> metrics_failures{0};
  std::thread metrics_reader([&] {
    while (!stop_metrics.load(std::memory_order_acquire)) {
      const auto snapshot = runtime.metrics();
      if (snapshot.queue.queued_bytes > snapshot.queue.capacity_bytes ||
          snapshot.gate.pending_reserved_bytes >
              snapshot.reorder_capacity_bytes ||
          snapshot.engine.retained_state_bytes >
              snapshot.engine.retained_state_capacity_bytes) {
        metrics_failures.fetch_add(1U, std::memory_order_relaxed);
      }
    }
  });
  const auto final_token = token(1000U + 1000U + transaction_count - 1U);
  require_ok(runtime.updateDatabaseFrontier(final_token));
  for (std::size_t producer_index = 0; producer_index < producer_count;
       ++producer_index) {
    require_ok(runtime.updateProducerDeliveryFrontier(
        static_cast<ProducerId>(producer_index + 1U), final_token,
        static_cast<ProducerSeq>(per_producer)));
  }
  require_ok(runtime.flush());
  stop_metrics.store(true, std::memory_order_release);
  metrics_reader.join();
  CHECK(metrics_failures.load(std::memory_order_relaxed) == 0U);
  metrics = runtime.metrics();
  CHECK(metrics.ordered_transactions == transaction_count);
  CHECK(metrics.checked_transactions == transaction_count);
  CHECK(metrics.gate.pending_transactions == 0U);
  CHECK(metrics.gate.active_starts == 0U);
  CHECK(metrics.queue.total_enqueued == metrics.queue.total_dequeued);
  CHECK(runtime.verdict()->status == ConsistencyStatus::Pass);
  close_cleanly(runtime);
}

void test_shutdown_refuses_incomplete_evidence() {
  {
    const auto meta = metadata(60, 1, 1, 10);
    OnlineRuntime runtime(options(1U << 20U, 1U << 20U), {1});
    require_ok(runtime.registerActiveStart(meta.attempt_id, 1, token(10)));
    const auto shutdown = runtime.shutdown();
    CHECK(!shutdown.ok());
    CHECK(shutdown.error.has_value());
    CHECK(shutdown.error->code == RuntimeErrorCode::IncompletePrefix);
    CHECK(!runtime.verdict().has_value());
  }

  {
    const auto meta = metadata(61, 1, 1, 10);
    auto batch      = empty_batch(meta);
    OnlineRuntime runtime(options(1U << 20U, 1U << 20U), {1});
    require_ok(runtime.registerActiveStart(meta.attempt_id, 1, token(10)));
    require_accepted(runtime.submitCommitted(std::move(batch), true));
    require_ok(runtime.flush());
    CHECK(runtime.metrics().gate.pending_transactions == 1U);
    const auto shutdown = runtime.shutdown();
    CHECK(!shutdown.ok());
    CHECK(shutdown.error.has_value());
    CHECK(shutdown.error->code == RuntimeErrorCode::IncompletePrefix);
    CHECK(!runtime.verdict().has_value());
  }
}

void test_reorder_limit_fails_closed() {
  const auto meta     = metadata(70, 1, 1, 10);
  auto batch          = empty_batch(meta);
  const auto reserved = batch->reserved_bytes();
  CHECK(reserved > 1U);
  OnlineRuntime runtime(options(reserved, reserved - 1U), {1});
  require_ok(runtime.registerActiveStart(meta.attempt_id, 1, token(10)));
  CHECK(runtime.submitCommitted(std::move(batch), true).accepted);
  const auto flushed = runtime.flush();
  CHECK(!flushed.ok());
  CHECK(flushed.error.has_value());
  CHECK(flushed.error->code == RuntimeErrorCode::ReorderLimitExceeded);
  CHECK(!runtime.verdict().has_value());
  CHECK(!runtime.shutdown().ok());
}

void test_invalid_configuration_never_exposes_pass() {
  OnlineRuntime runtime(options(0, 1024), {1});
  const auto error = runtime.error();
  CHECK(error.has_value());
  CHECK(error->code == RuntimeErrorCode::ResourceExhausted);
  CHECK(error->queue_error == QueueErrorCode::InvalidCapacity);
  CHECK(!runtime.verdict().has_value());
  CHECK(!runtime.shutdown().ok());
}


void test_serialized_prepare_submit_orders_directly() {
  const auto write_meta = metadata(80, 1, 0, 20, 42, 1);
  auto write = write_batch(write_meta, "key", "value");
  const auto read_meta = metadata(81, 1, 0, 30, 42, 2);
  auto read = read_batch(read_meta, "key",
                         std::optional<std::string_view>{"value"});

  OnlineRuntime runtime(serialized_options(1U << 20U), {1});
  auto prepared_write =
      runtime.prepareCommitted(PackedBatchEnvelope(std::move(write)));
  CHECK(prepared_write.ok());
  CHECK(runtime.metrics().queue.reserved_depth == 1U);
  auto write_final                = write_meta;
  write_final.order_token_kind    = OrderTokenKind::WriteCommit;
  write_final.publication_seq     = 1;
  write_final.producer_seq        = 1;
  require_accepted(runtime.submitPrepared(
      std::move(prepared_write.prepared), write_final));

  auto prepared_read =
      runtime.prepareCommitted(PackedBatchEnvelope(std::move(read)));
  CHECK(prepared_read.ok());
  auto read_final                 = read_meta;
  read_final.order_token_kind     = OrderTokenKind::ReadOnlyCompletion;
  read_final.publication_seq      = 2;
  read_final.producer_seq         = 2;
  require_accepted(runtime.submitPrepared(
      std::move(prepared_read.prepared), read_final));

  require_ok(runtime.flush());
  const auto metrics = runtime.metrics();
  CHECK(metrics.ordering_protocol ==
        OrderingProtocol::SerializedGlobalLock);
  CHECK(metrics.published_transactions == 2U);
  CHECK(metrics.ordered_transactions == 2U);
  CHECK(metrics.checked_transactions == 2U);
  CHECK(metrics.serialized.accepted_transactions == 2U);
  CHECK(metrics.serialized.next_publication_seq == 3U);
  CHECK(metrics.queue.reserved_depth == 0U);
  CHECK(runtime.verdict()->status == ConsistencyStatus::Pass);
  close_cleanly(runtime);
}

void test_serialized_outstanding_reservation_fails_shutdown() {
  const auto meta = metadata(82, 1, 0, 20);
  auto batch      = write_batch(meta, "key", "value");
  OnlineRuntime runtime(serialized_options(1U << 20U), {1});
  auto prepared =
      runtime.prepareCommitted(PackedBatchEnvelope(std::move(batch)));
  CHECK(prepared.ok());
  CHECK(runtime.metrics().queue.reserved_depth == 1U);
  const auto shutdown = runtime.shutdown();
  CHECK(!shutdown.ok());
  CHECK(shutdown.error.has_value());
  CHECK(shutdown.error->code == RuntimeErrorCode::IncompletePrefix);
  prepared.prepared = RuntimePreparedBatch{};
}

} // namespace

int run_runtime_tests() {
  try {
    test_reordered_arrival_matches_commit_sorted_packed_engine();
    test_delayed_read_only_start_blocks_later_write_until_publication();
    test_violation_is_sticky_but_later_transactions_are_checked();
    test_read_only_declaration_must_match_validated_write_count();
    test_write_token_kind_allows_zero_normalized_writes();
    test_orderer_only_recycles_owner_and_budget_without_a_pass_verdict();
    test_budgeted_envelope_move_assignment_retires_old_pair();
    test_multi_producer_saturation_is_lossless();
    test_shutdown_refuses_incomplete_evidence();
    test_reorder_limit_fails_closed();
    test_invalid_configuration_never_exposes_pass();
    test_serialized_prepare_submit_orders_directly();
    test_serialized_outstanding_reservation_fails_shutdown();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "runtime tests failed: " << error.what() << '\n';
    return 1;
  }
}

#ifdef CO_CHECKER_RUNTIME_STANDALONE_TEST_MAIN
int main() { return run_runtime_tests(); }
#endif
