#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "co_checker/online/bounded_mpsc_queue.h"
#include "co_checker/online/byte_budget.h"
#include "co_checker/online/engine.h"
#include "co_checker/online/order_gate.h"
#include "co_checker/online/packed_batch.h"
#include "co_checker/online/result.h"
#include "co_checker/online/serialized_order.h"

namespace co_checker::online {

enum class RuntimeErrorCode : std::uint8_t {
  QueueFailure,
  OrderGateFailure,
  EngineFailure,
  ReorderLimitExceeded,
  ResourceExhausted,
  IncompletePrefix,
  ProtocolFailure,
  Stopped,
};

struct RuntimeError {
  RuntimeErrorCode code = RuntimeErrorCode::ProtocolFailure;
  std::string message;
  std::optional<QueueErrorCode> queue_error;
  std::optional<OrderGateErrorCode> order_error;
  std::optional<EngineErrorCode> engine_error;

  friend bool operator==(const RuntimeError &, const RuntimeError &) = default;
};

enum class PipelineMode : std::uint8_t {
  CaptureOnly = 1,
  OrderOnly,
  Full,
};

enum class OrderingProtocol : std::uint8_t {
  SerializedGlobalLock = 1,
  FrontierReorder,
};

struct RuntimeOptions {
  BoundedQueueOptions queue{
      .capacity_bytes = 64U * 1024U * 1024U,
      .push_timeout   = std::chrono::seconds{30},
  };
  std::size_t reorder_capacity_bytes = 0;
  std::size_t retained_state_capacity_bytes =
      std::numeric_limits<std::size_t>::max();
  PipelineMode pipeline_mode       = PipelineMode::Full;
  OrderingProtocol ordering_protocol =
      OrderingProtocol::SerializedGlobalLock;
  RuntimeEpoch expected_runtime_epoch;
  std::uint64_t expected_cluster_epoch = 0;
};

enum class PackedBatchBackend : std::uint8_t {
  None,
  Vector,
  Chunk,
};

// Move-only ownership envelope used at the producer/consumer handoff. A
// rejected submission returns the same envelope to the caller. An accepted
// submission leaves the caller's result envelope empty.
class PackedBatchEnvelope final {
public:
  PackedBatchEnvelope() = default;
  explicit PackedBatchEnvelope(std::unique_ptr<VectorBatch> batch) noexcept;
  explicit PackedBatchEnvelope(std::unique_ptr<ChunkBatch> batch) noexcept;
  PackedBatchEnvelope(std::unique_ptr<VectorBatch> batch,
                      ByteBudgetLease budget_lease) noexcept;
  PackedBatchEnvelope(std::unique_ptr<ChunkBatch> batch,
                      ByteBudgetLease budget_lease) noexcept;

  PackedBatchEnvelope(PackedBatchEnvelope &&other) noexcept;
  PackedBatchEnvelope &operator=(PackedBatchEnvelope &&other) noexcept;
  PackedBatchEnvelope(const PackedBatchEnvelope &)            = delete;
  PackedBatchEnvelope &operator=(const PackedBatchEnvelope &) = delete;
  ~PackedBatchEnvelope()                                      = default;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] PackedBatchBackend backend() const noexcept;
  [[nodiscard]] const ValidatedTxnView &view() const noexcept;
  [[nodiscard]] std::size_t used_bytes() const noexcept;
  [[nodiscard]] std::size_t reserved_bytes() const noexcept;
  [[nodiscard]] std::size_t budget_lease_bytes() const noexcept;

private:
  using Storage = std::variant<std::monostate, std::unique_ptr<VectorBatch>,
                               std::unique_ptr<ChunkBatch>>;
  // storage_ is destroyed before budget_lease_, ensuring the reservation is
  // released only after the packed allocation has been recycled.
  ByteBudgetLease budget_lease_;
  Storage storage_;
};

static_assert(std::is_nothrow_move_constructible_v<PackedBatchEnvelope>);
static_assert(std::is_nothrow_move_assignable_v<PackedBatchEnvelope>);

class RuntimePreparedBatchAccess;

class RuntimePreparedBatch final {
public:
  RuntimePreparedBatch() noexcept;
  RuntimePreparedBatch(RuntimePreparedBatch &&other) noexcept;
  RuntimePreparedBatch &operator=(RuntimePreparedBatch &&other) noexcept;
  RuntimePreparedBatch(const RuntimePreparedBatch &)            = delete;
  RuntimePreparedBatch &operator=(const RuntimePreparedBatch &) = delete;
  ~RuntimePreparedBatch();

  [[nodiscard]] bool valid() const noexcept;

private:
  friend class OnlineRuntime;
  friend class RuntimePreparedBatchAccess;
  struct State;
  explicit RuntimePreparedBatch(std::unique_ptr<State> state) noexcept;

  std::unique_ptr<State> state_;
};

struct RuntimePrepareResult {
  RuntimePreparedBatch prepared;
  std::optional<RuntimeError> error;
  PackedBatchEnvelope returned_batch;

  [[nodiscard]] bool ok() const noexcept { return prepared.valid(); }
};

struct RuntimeBatchSubmitResult {
  bool accepted = false;
  std::optional<RuntimeError> error;
  PackedBatchEnvelope returned_batch;

  [[nodiscard]] bool ok() const noexcept { return accepted; }
};

struct RuntimeControlResult {
  std::optional<RuntimeError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct RuntimeMetrics {
  BoundedQueueMetrics queue;
  OrderGateMetrics gate;
  SnapshotSOWWMetrics engine;
  std::size_t reorder_capacity_bytes   = 0;
  PipelineMode pipeline_mode             = PipelineMode::Full;
  OrderingProtocol ordering_protocol     =
      OrderingProtocol::SerializedGlobalLock;
  SerializedOrderMetrics serialized;
  std::uint64_t published_transactions = 0;
  std::uint64_t processed_messages     = 0;
  std::uint64_t processed_controls     = 0;
  std::uint64_t ordered_transactions   = 0;
  std::uint64_t checked_transactions   = 0;
  std::uint64_t checked_events         = 0;
  std::uint64_t violating_transactions = 0;
  std::chrono::nanoseconds total_queue_lag{};
  std::chrono::nanoseconds max_queue_lag{};
  std::chrono::nanoseconds total_ordering_time{};
  std::chrono::nanoseconds total_checking_time{};
  std::chrono::nanoseconds max_checking_time{};
  std::uint64_t current_commit_token_lag = 0;
  std::uint64_t max_commit_token_lag     = 0;
  std::optional<CommitOrderToken> highest_observed_token;
  std::optional<CommitOrderToken> last_ordered_token;
  std::optional<CommitOrderToken> last_checked_token;
  bool consumer_running = false;
  bool checking_enabled = true;
};

class OnlineRuntime final {
public:
  OnlineRuntime(RuntimeOptions options,
                std::vector<ProducerId> required_producers);
  ~OnlineRuntime();

  OnlineRuntime(const OnlineRuntime &)            = delete;
  OnlineRuntime &operator=(const OnlineRuntime &) = delete;
  OnlineRuntime(OnlineRuntime &&)                 = delete;
  OnlineRuntime &operator=(OnlineRuntime &&)      = delete;

  // Serialized publication is explicitly two-stage. prepareCommitted()
  // reserves queue bytes and a preallocated queue node; submitPrepared() is
  // allocation-free and cannot wait for capacity.
  [[nodiscard]] RuntimePrepareResult
  prepareCommitted(PackedBatchEnvelope batch);
  [[nodiscard]] RuntimeBatchSubmitResult
  submitPrepared(RuntimePreparedBatch prepared, OrderedTxnMeta final_meta);

  // Frontier publication remains a one-stage producer operation.
  [[nodiscard]] RuntimeBatchSubmitResult
  submitCommitted(PackedBatchEnvelope batch, bool read_only);
  [[nodiscard]] RuntimeBatchSubmitResult
  submitCommitted(std::unique_ptr<VectorBatch> batch, bool read_only);
  [[nodiscard]] RuntimeBatchSubmitResult
  submitCommitted(std::unique_ptr<ChunkBatch> batch, bool read_only);

  // Registration is acknowledged only after the consumer has applied it to
  // the gate. It therefore linearizes before this call returns.
  [[nodiscard]] RuntimeControlResult
  registerActiveStart(const TransactionAttemptUid &attempt_id,
                      ProducerId producer_id,
                      const CommitOrderToken &start_token);
  [[nodiscard]] RuntimeControlResult
  abortActiveStart(const TransactionAttemptUid &attempt_id);
  [[nodiscard]] RuntimeControlResult
  updateDatabaseFrontier(const CommitOrderToken &frontier);
  [[nodiscard]] RuntimeControlResult
  updateProducerDeliveryFrontier(ProducerId producer_id,
                                 const CommitOrderToken &frontier,
                                 ProducerSeq delivered_sequence);

  // flush() acknowledges that all earlier queue entries have been processed.
  // quiescentBarrier() additionally requires no pending batch or active start.
  [[nodiscard]] RuntimeControlResult flush();
  [[nodiscard]] RuntimeControlResult quiescentBarrier();
  [[nodiscard]] RuntimeControlResult shutdown();
  // Records an adapter/capture failure on the runtime's separate sticky error
  // channel and stops admission. This can never produce a Pass verdict.
  [[nodiscard]] RuntimeControlResult fail(RuntimeError error);

  [[nodiscard]] std::optional<ConsistencyVerdict> verdict() const;
  [[nodiscard]] std::optional<RuntimeError> error() const;
  [[nodiscard]] RuntimeMetrics metrics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace co_checker::online
