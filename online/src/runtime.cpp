#include "co_checker/online/runtime.h"

#include "co_checker/online/engine.h"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace co_checker::online {
namespace {

template <typename... Callables> struct Overloaded : Callables... {
  using Callables::operator()...;
};
template <typename... Callables>
Overloaded(Callables...) -> Overloaded<Callables...>;

class ControlAcknowledgement final {
public:
  void complete(std::optional<RuntimeError> error) {
    std::lock_guard lock(mutex_);
    if (completed_) {
      return;
    }
    error_     = std::move(error);
    completed_ = true;
    changed_.notify_all();
  }

  [[nodiscard]] RuntimeControlResult wait() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return completed_; });
    return RuntimeControlResult{error_};
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool completed_ = false;
  std::optional<RuntimeError> error_;
};

using ControlAck = std::shared_ptr<ControlAcknowledgement>;

struct CommittedBatchMessage {
  PackedBatchEnvelope batch;
  bool read_only = false;
  std::chrono::steady_clock::time_point submitted_at;
};

struct RegisterActiveMessage {
  TransactionAttemptUid attempt_id;
  ProducerId producer_id = 0;
  CommitOrderToken start_token;
  ControlAck acknowledgement;
};

struct AbortActiveMessage {
  TransactionAttemptUid attempt_id;
  ControlAck acknowledgement;
};

struct DatabaseFrontierMessage {
  CommitOrderToken frontier;
  ControlAck acknowledgement;
};

struct ProducerFrontierMessage {
  ProducerId producer_id = 0;
  CommitOrderToken frontier;
  ProducerSeq delivered_sequence = 0;
  ControlAck acknowledgement;
};

struct FlushMessage {
  bool require_quiescence = false;
  bool stop_after         = false;
  ControlAck acknowledgement;
};

using RuntimeMessagePayload =
    std::variant<CommittedBatchMessage, RegisterActiveMessage,
                 AbortActiveMessage, DatabaseFrontierMessage,
                 ProducerFrontierMessage, FlushMessage>;

struct RuntimeMessage {
  RuntimeMessagePayload payload;

  explicit RuntimeMessage(RuntimeMessagePayload value) noexcept
      : payload(std::move(value)) {}
  RuntimeMessage(RuntimeMessage &&) noexcept            = default;
  RuntimeMessage &operator=(RuntimeMessage &&) noexcept = default;
  RuntimeMessage(const RuntimeMessage &)                = delete;
  RuntimeMessage &operator=(const RuntimeMessage &)     = delete;
};

static_assert(std::is_nothrow_move_constructible_v<RuntimeMessage>);

[[nodiscard]] ControlAck control_ack(RuntimeMessage &message) {
  return std::visit(
      Overloaded{
          [](CommittedBatchMessage &) -> ControlAck { return {}; },
          [](auto &control) -> ControlAck { return control.acknowledgement; },
      },
      message.payload);
}

[[nodiscard]] bool is_stop_message(const RuntimeMessage &message) noexcept {
  const auto *flush = std::get_if<FlushMessage>(&message.payload);
  return flush != nullptr && flush->stop_after;
}

[[nodiscard]] RuntimeError stopped_error() {
  RuntimeError result;
  result.code    = RuntimeErrorCode::Stopped;
  result.message = "online runtime is stopping";
  return result;
}

[[nodiscard]] RuntimeError queue_error(const QueueError &error) {
  const auto is_resource = error.code == QueueErrorCode::InvalidCapacity ||
                           error.code == QueueErrorCode::OversizedItem ||
                           error.code == QueueErrorCode::ResourceExhausted;
  RuntimeError result;
  result.code        = is_resource ? RuntimeErrorCode::ResourceExhausted
                                   : RuntimeErrorCode::QueueFailure;
  result.message     = error.message;
  result.queue_error = error.code;
  return result;
}

[[nodiscard]] RuntimeError order_error(const OrderGateError &error) {
  RuntimeError result;
  result.code        = RuntimeErrorCode::OrderGateFailure;
  result.message     = error.message;
  result.order_error = error.code;
  return result;
}

[[nodiscard]] RuntimeError engine_error(const EngineError &error) {
  RuntimeError result;
  result.code         = RuntimeErrorCode::EngineFailure;
  result.message      = error.message;
  result.engine_error = error.code;
  return result;
}

[[nodiscard]] RuntimeError runtime_error(const RuntimeErrorCode code,
                                         std::string message) {
  RuntimeError result;
  result.code    = code;
  result.message = std::move(message);
  return result;
}

} // namespace

PackedBatchEnvelope::PackedBatchEnvelope(
    std::unique_ptr<VectorBatch> batch) noexcept
    : storage_(std::move(batch)) {}

PackedBatchEnvelope::PackedBatchEnvelope(
    std::unique_ptr<ChunkBatch> batch) noexcept
    : storage_(std::move(batch)) {}

PackedBatchEnvelope::PackedBatchEnvelope(std::unique_ptr<VectorBatch> batch,
                                         ByteBudgetLease budget_lease) noexcept
    : budget_lease_(std::move(budget_lease)), storage_(std::move(batch)) {}

PackedBatchEnvelope::PackedBatchEnvelope(std::unique_ptr<ChunkBatch> batch,
                                         ByteBudgetLease budget_lease) noexcept
    : budget_lease_(std::move(budget_lease)), storage_(std::move(batch)) {}

PackedBatchEnvelope::PackedBatchEnvelope(PackedBatchEnvelope &&other) noexcept
    : budget_lease_(std::move(other.budget_lease_)),
      storage_(std::move(other.storage_)) {}

PackedBatchEnvelope &
PackedBatchEnvelope::operator=(PackedBatchEnvelope &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  // Retire an allocation before releasing its matching lease. Otherwise a
  // woken allocator could observe budget capacity while the old storage is
  // still live.
  Storage incoming_storage(std::move(other.storage_));
  ByteBudgetLease incoming_lease(std::move(other.budget_lease_));
  storage_.template emplace<std::monostate>();
  budget_lease_.reset();
  storage_      = std::move(incoming_storage);
  budget_lease_ = std::move(incoming_lease);
  return *this;
}

bool PackedBatchEnvelope::valid() const noexcept {
  return std::visit(Overloaded{
                        [](const std::monostate &) { return false; },
                        [](const auto &batch) { return batch != nullptr; },
                    },
                    storage_);
}

PackedBatchBackend PackedBatchEnvelope::backend() const noexcept {
  if (std::holds_alternative<std::unique_ptr<VectorBatch>>(storage_)) {
    return PackedBatchBackend::Vector;
  }
  if (std::holds_alternative<std::unique_ptr<ChunkBatch>>(storage_)) {
    return PackedBatchBackend::Chunk;
  }
  return PackedBatchBackend::None;
}

const ValidatedTxnView &PackedBatchEnvelope::view() const noexcept {
  return std::visit(Overloaded{
                        [](const std::monostate &) -> const ValidatedTxnView & {
                          std::terminate();
                        },
                        [](const auto &batch) -> const ValidatedTxnView & {
                          if (batch == nullptr) {
                            std::terminate();
                          }
                          return batch->view();
                        },
                    },
                    storage_);
}

std::size_t PackedBatchEnvelope::used_bytes() const noexcept {
  return std::visit(Overloaded{
                        [](const std::monostate &) -> std::size_t { return 0; },
                        [](const auto &batch) -> std::size_t {
                          return batch == nullptr ? 0 : batch->used_bytes();
                        },
                    },
                    storage_);
}

std::size_t PackedBatchEnvelope::reserved_bytes() const noexcept {
  return std::visit(Overloaded{
                        [](const std::monostate &) -> std::size_t { return 0; },
                        [](const auto &batch) -> std::size_t {
                          return batch == nullptr ? 0 : batch->reserved_bytes();
                        },
                    },
                    storage_);
}

std::size_t PackedBatchEnvelope::budget_lease_bytes() const noexcept {
  return budget_lease_.bytes();
}

class OnlineRuntime::Impl {
public:
  Impl(RuntimeOptions options, std::vector<ProducerId> required_producers)
      : options_(std::move(options)), queue_(options_.queue),
        gate_(std::move(required_producers)),
        engine_(
            SnapshotSOWWOptions{.retained_state_capacity_bytes =
                                    options_.retained_state_capacity_bytes}) {
    if (options_.checking_enabled) {
      verdict_ = ConsistencyVerdict{};
    }

    if (options_.reorder_capacity_bytes == 0) {
      (void)record_error(
          runtime_error(RuntimeErrorCode::ResourceExhausted,
                        "reorder byte capacity must be greater than zero"),
          false);
    }
    if (const auto queue_error_state = queue_.error();
        queue_error_state.has_value()) {
      (void)record_error(queue_error(*queue_error_state), false);
    }
    if (const auto gate_error = gate_.error(); gate_error.has_value()) {
      (void)record_error(order_error(*gate_error), false);
    }

    consumer_ = std::thread([this] { consumer_loop(); });
    if (error().has_value()) {
      (void)queue_.close();
    }
  }

  ~Impl() { (void)shutdown(); }

  [[nodiscard]] RuntimeBatchSubmitResult
  submit_committed(PackedBatchEnvelope batch, const bool read_only) {
    if (!batch.valid()) {
      auto error =
          record_error(runtime_error(RuntimeErrorCode::ProtocolFailure,
                                     "cannot submit an empty packed batch"));
      return RuntimeBatchSubmitResult{false, std::move(error),
                                      std::move(batch)};
    }

    const auto charged_bytes = batch.reserved_bytes();
    std::shared_lock admission_lock(admission_mutex_);
    if (stopping_) {
      return RuntimeBatchSubmitResult{false, stopped_error(), std::move(batch)};
    }

    RuntimeMessage message{CommittedBatchMessage{
        .batch        = std::move(batch),
        .read_only    = read_only,
        .submitted_at = std::chrono::steady_clock::now(),
    }};
    const auto push_result = queue_.pushData(std::move(message), charged_bytes);
    if (!push_result.ok()) {
      auto returned_batch =
          std::move(std::get<CommittedBatchMessage>(message.payload).batch);
      const auto mapped = queue_error(*push_result.error);
      const auto sticky = record_error(mapped);
      return RuntimeBatchSubmitResult{false, sticky, std::move(returned_batch)};
    }

    {
      std::lock_guard state_lock(state_mutex_);
      ++published_transactions_;
    }
    return RuntimeBatchSubmitResult{true, std::nullopt, {}};
  }

  [[nodiscard]] RuntimeControlResult
  register_active_start(const TransactionAttemptUid &attempt_id,
                        const ProducerId producer_id,
                        const CommitOrderToken &start_token) {
    try {
      auto acknowledgement = std::make_shared<ControlAcknowledgement>();
      return send_control(RegisterActiveMessage{
          attempt_id, producer_id, start_token, std::move(acknowledgement)});
    } catch (const std::bad_alloc &) {
      return RuntimeControlResult{record_resource_error(
          "could not allocate active-start acknowledgement")};
    }
  }

  [[nodiscard]] RuntimeControlResult
  abort_active_start(const TransactionAttemptUid &attempt_id) {
    try {
      auto acknowledgement = std::make_shared<ControlAcknowledgement>();
      return send_control(
          AbortActiveMessage{attempt_id, std::move(acknowledgement)});
    } catch (const std::bad_alloc &) {
      return RuntimeControlResult{
          record_resource_error("could not allocate abort acknowledgement")};
    }
  }

  [[nodiscard]] RuntimeControlResult
  update_database_frontier(const CommitOrderToken &frontier) {
    try {
      auto acknowledgement = std::make_shared<ControlAcknowledgement>();
      return send_control(
          DatabaseFrontierMessage{frontier, std::move(acknowledgement)});
    } catch (const std::bad_alloc &) {
      return RuntimeControlResult{record_resource_error(
          "could not allocate database-frontier acknowledgement")};
    }
  }

  [[nodiscard]] RuntimeControlResult
  update_producer_frontier(const ProducerId producer_id,
                           const CommitOrderToken &frontier,
                           const ProducerSeq delivered_sequence) {
    try {
      auto acknowledgement = std::make_shared<ControlAcknowledgement>();
      return send_control(ProducerFrontierMessage{producer_id, frontier,
                                                  delivered_sequence,
                                                  std::move(acknowledgement)});
    } catch (const std::bad_alloc &) {
      return RuntimeControlResult{record_resource_error(
          "could not allocate producer-frontier acknowledgement")};
    }
  }

  [[nodiscard]] RuntimeControlResult flush(const bool require_quiescence) {
    try {
      auto acknowledgement = std::make_shared<ControlAcknowledgement>();
      return send_control(
          FlushMessage{require_quiescence, false, std::move(acknowledgement)});
    } catch (const std::bad_alloc &) {
      return RuntimeControlResult{
          record_resource_error("could not allocate barrier acknowledgement")};
    }
  }

  [[nodiscard]] RuntimeControlResult fail(RuntimeError failure) {
    if (failure.message.empty()) {
      failure.message = "online adapter reported a fatal error";
    }
    return RuntimeControlResult{record_error(std::move(failure))};
  }

  [[nodiscard]] RuntimeControlResult shutdown() {
    std::lock_guard shutdown_lock(shutdown_mutex_);
    if (joined_) {
      return RuntimeControlResult{error()};
    }

    ControlAck acknowledgement;
    try {
      acknowledgement = std::make_shared<ControlAcknowledgement>();
    } catch (const std::bad_alloc &) {
      (void)record_resource_error(
          "could not allocate shutdown acknowledgement");
    }

    bool accepted = false;
    {
      std::unique_lock admission_lock(admission_mutex_);
      stopping_ = true;
      if (acknowledgement != nullptr) {
        RuntimeMessage message{FlushMessage{
            .require_quiescence = true,
            .stop_after         = true,
            .acknowledgement    = acknowledgement,
        }};
        const auto result = queue_.pushControl(std::move(message));
        accepted          = result.ok();
        if (!accepted && result.error.has_value()) {
          (void)record_error(queue_error(*result.error));
        }
      }
    }

    if (accepted) {
      (void)acknowledgement->wait();
    } else {
      (void)queue_.close();
    }
    if (consumer_.joinable()) {
      consumer_.join();
    }
    joined_ = true;
    return RuntimeControlResult{error()};
  }

  [[nodiscard]] std::optional<ConsistencyVerdict> verdict() const {
    std::lock_guard lock(state_mutex_);
    if (error_.has_value()) {
      return std::nullopt;
    }
    if (!options_.checking_enabled) {
      return std::nullopt;
    }
    return verdict_;
  }

  [[nodiscard]] std::optional<RuntimeError> error() const {
    std::lock_guard lock(state_mutex_);
    return error_;
  }

  [[nodiscard]] RuntimeMetrics metrics() const {
    RuntimeMetrics result;
    {
      std::lock_guard lock(state_mutex_);
      result.reorder_capacity_bytes   = options_.reorder_capacity_bytes;
      result.published_transactions   = published_transactions_;
      result.processed_messages       = processed_messages_;
      result.processed_controls       = processed_controls_;
      result.ordered_transactions     = ordered_transactions_;
      result.checked_transactions     = checked_transactions_;
      result.checked_events           = checked_events_;
      result.violating_transactions   = violating_transactions_;
      result.total_queue_lag          = total_queue_lag_;
      result.max_queue_lag            = max_queue_lag_;
      result.total_ordering_time      = total_ordering_time_;
      result.total_checking_time      = total_checking_time_;
      result.max_checking_time        = max_checking_time_;
      result.current_commit_token_lag = current_commit_token_lag_;
      result.max_commit_token_lag     = max_commit_token_lag_;
      result.highest_observed_token   = highest_observed_token_;
      result.last_ordered_token       = last_ordered_token_;
      result.last_checked_token       = last_checked_token_;
      result.consumer_running         = consumer_running_;
      result.checking_enabled         = options_.checking_enabled;
    }
    result.queue = queue_.metrics();
    result.gate  = gate_.metrics();
    {
      std::lock_guard engine_lock(engine_mutex_);
      result.engine = engine_.metrics();
    }
    return result;
  }

private:
  template <typename Operation> auto timed_ordering(Operation &&operation) {
    const auto started = std::chrono::steady_clock::now();
    auto result        = std::forward<Operation>(operation)();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started);
    {
      std::lock_guard lock(state_mutex_);
      total_ordering_time_ += elapsed;
    }
    return result;
  }

  template <typename Control>
  [[nodiscard]] RuntimeControlResult send_control(Control control) {
    const auto acknowledgement = control.acknowledgement;
    std::shared_lock admission_lock(admission_mutex_);
    if (stopping_) {
      return RuntimeControlResult{stopped_error()};
    }
    RuntimeMessage message{std::move(control)};
    const auto result = queue_.pushControl(std::move(message));
    if (!result.ok()) {
      return RuntimeControlResult{record_error(queue_error(*result.error))};
    }
    admission_lock.unlock();
    return acknowledgement->wait();
  }

  [[nodiscard]] std::optional<RuntimeError>
  record_error(RuntimeError error, const bool close_queue = true) {
    std::optional<RuntimeError> sticky;
    {
      std::lock_guard lock(state_mutex_);
      if (!error_.has_value()) {
        error_ = std::move(error);
      }
      sticky = error_;
    }
    if (close_queue) {
      (void)queue_.close();
    }
    return sticky;
  }

  [[nodiscard]] std::optional<RuntimeError>
  record_resource_error(std::string message) {
    return record_error(
        runtime_error(RuntimeErrorCode::ResourceExhausted, std::move(message)));
  }

  void complete_with_current_error(const ControlAck &acknowledgement) {
    if (acknowledgement != nullptr) {
      acknowledgement->complete(error());
    }
  }

  void consumer_loop() noexcept {
    {
      std::lock_guard lock(state_mutex_);
      consumer_running_ = true;
    }

    bool stop_after_entry = false;
    while (!stop_after_entry) {
      auto popped = queue_.waitPop();
      if (popped.hasEntry()) {
        auto &message = popped.entry->value;
        {
          std::lock_guard lock(state_mutex_);
          ++processed_messages_;
          if (!std::holds_alternative<CommittedBatchMessage>(message.payload)) {
            ++processed_controls_;
          }
        }

        if (error().has_value()) {
          complete_with_current_error(control_ack(message));
          stop_after_entry = is_stop_message(message);
          if (stop_after_entry) {
            (void)queue_.close();
          }
          continue;
        }

        try {
          stop_after_entry =
              process_message(message, popped.entry->charged_reserved_bytes);
        } catch (const std::bad_alloc &) {
          (void)record_resource_error(
              "online consumer could not allocate required state");
          complete_with_current_error(control_ack(message));
        } catch (const std::exception &exception) {
          (void)record_error(runtime_error(RuntimeErrorCode::ProtocolFailure,
                                           exception.what()));
          complete_with_current_error(control_ack(message));
        } catch (...) {
          (void)record_error(runtime_error(
              RuntimeErrorCode::ProtocolFailure,
              "online consumer encountered an unknown exception"));
          complete_with_current_error(control_ack(message));
        }
        continue;
      }

      if (popped.isTerminal()) {
        if (!error().has_value() &&
            popped.terminal_error->code != QueueErrorCode::Shutdown) {
          (void)record_error(queue_error(*popped.terminal_error), false);
        }
        break;
      }
    }

    {
      std::lock_guard lock(state_mutex_);
      consumer_running_ = false;
    }
  }

  [[nodiscard]] bool process_message(RuntimeMessage &message,
                                     const std::size_t charged_bytes) {
    return std::visit(
        Overloaded{
            [&](CommittedBatchMessage &committed) {
              process_committed(committed, charged_bytes);
              return false;
            },
            [&](RegisterActiveMessage &registration) {
              if (const auto gate_error = timed_ordering([&] {
                    return gate_.registerActiveStart(registration.attempt_id,
                                                     registration.producer_id,
                                                     registration.start_token);
                  });
                  gate_error.has_value()) {
                (void)record_error(order_error(*gate_error));
              } else {
                drain_eligible();
              }
              complete_with_current_error(registration.acknowledgement);
              return false;
            },
            [&](AbortActiveMessage &abort) {
              if (const auto gate_error = timed_ordering(
                      [&] { return gate_.abortActiveStart(abort.attempt_id); });
                  gate_error.has_value()) {
                (void)record_error(order_error(*gate_error));
              } else {
                drain_eligible();
              }
              complete_with_current_error(abort.acknowledgement);
              return false;
            },
            [&](DatabaseFrontierMessage &frontier) {
              if (const auto gate_error = timed_ordering([&] {
                    return gate_.updateDatabaseFrontier(frontier.frontier);
                  });
                  gate_error.has_value()) {
                (void)record_error(order_error(*gate_error));
              } else {
                drain_eligible();
              }
              complete_with_current_error(frontier.acknowledgement);
              return false;
            },
            [&](ProducerFrontierMessage &frontier) {
              if (const auto gate_error = timed_ordering([&] {
                    return gate_.updateProducerDeliveryFrontier(
                        frontier.producer_id, frontier.frontier,
                        frontier.delivered_sequence);
                  });
                  gate_error.has_value()) {
                (void)record_error(order_error(*gate_error));
              } else {
                drain_eligible();
              }
              complete_with_current_error(frontier.acknowledgement);
              return false;
            },
            [&](FlushMessage &flush) {
              drain_eligible();
              if (flush.require_quiescence && !error().has_value()) {
                require_quiescence();
              }
              complete_with_current_error(flush.acknowledgement);
              if (flush.stop_after) {
                (void)queue_.close();
              }
              return flush.stop_after;
            },
        },
        message.payload);
  }

  void process_committed(CommittedBatchMessage &committed,
                         const std::size_t charged_bytes) {
    const auto now = std::chrono::steady_clock::now();
    const auto lag = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - committed.submitted_at);
    {
      std::lock_guard lock(state_mutex_);
      total_queue_lag_ += lag;
      max_queue_lag_ = std::max(max_queue_lag_, lag);
    }

    if (!committed.batch.valid() || charged_bytes == 0 ||
        charged_bytes != committed.batch.reserved_bytes()) {
      (void)record_error(runtime_error(
          RuntimeErrorCode::ProtocolFailure,
          "queue byte charge disagrees with packed batch ownership"));
      return;
    }

    const auto gate_metrics = gate_.metrics();
    if (charged_bytes > options_.reorder_capacity_bytes ||
        gate_metrics.pending_reserved_bytes >
            options_.reorder_capacity_bytes - charged_bytes) {
      (void)record_error(runtime_error(
          RuntimeErrorCode::ReorderLimitExceeded,
          "commit-order pending bytes exceed the configured limit"));
      return;
    }

    auto owner =
        std::make_shared<PackedBatchEnvelope>(std::move(committed.batch));
    const auto &captured_meta = owner->view().meta();
    if (committed.read_only && owner->view().write_count() != 0U) {
      (void)record_error(runtime_error(
          RuntimeErrorCode::ProtocolFailure,
          "a read-only token-kind batch contains a normalized write"));
      return;
    }
    if (captured_meta.commit_seq != kUnassignedCommitSeq) {
      (void)record_error(runtime_error(
          RuntimeErrorCode::ProtocolFailure,
          "captured packed batch already has an assigned CommitSeq"));
      return;
    }

    std::shared_ptr<const void> erased_owner = owner;
    PendingTransaction pending{
        .meta           = captured_meta,
        .read_only      = committed.read_only,
        .reserved_bytes = charged_bytes,
        .owner          = std::move(erased_owner),
    };
    if (const auto gate_error = timed_ordering(
            [&] { return gate_.submitCommitted(std::move(pending)); });
        gate_error.has_value()) {
      (void)record_error(order_error(*gate_error));
      return;
    }

    update_highest_observed(captured_meta.commit_token);
    drain_eligible();
  }

  void drain_eligible() {
    auto released = timed_ordering([&] { return gate_.releaseEligible(); });
    if (const auto gate_error = gate_.error(); gate_error.has_value()) {
      (void)record_error(order_error(*gate_error));
      return;
    }

    for (auto &transaction : released) {
      const auto *raw_owner =
          static_cast<const PackedBatchEnvelope *>(transaction.owner.get());
      if (raw_owner == nullptr || !raw_owner->valid()) {
        (void)record_error(runtime_error(
            RuntimeErrorCode::ProtocolFailure,
            "released gate transaction has no packed batch owner"));
        return;
      }
      const std::shared_ptr<const PackedBatchEnvelope> owner(transaction.owner,
                                                             raw_owner);
      const auto ordered_view = owner->view().with_meta(transaction.meta);
      {
        std::lock_guard lock(state_mutex_);
        ++ordered_transactions_;
        last_ordered_token_ = transaction.meta.commit_token;
        update_token_lag_locked();
      }
      if (!options_.checking_enabled) {
        continue;
      }
      ConsumeResult result;
      const auto checking_started = std::chrono::steady_clock::now();
      {
        std::lock_guard engine_lock(engine_mutex_);
        result = engine_.consume(ordered_view);
      }
      const auto checking_elapsed =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - checking_started);
      {
        std::lock_guard lock(state_mutex_);
        total_checking_time_ += checking_elapsed;
        max_checking_time_ = std::max(max_checking_time_, checking_elapsed);
      }
      if (!result.ok()) {
        (void)record_error(engine_error(*result.error));
        return;
      }

      {
        std::lock_guard lock(state_mutex_);
        ++checked_transactions_;
        checked_events_ += ordered_view.event_count();
        if (result.transaction_violation.has_value()) {
          ++violating_transactions_;
        }
        verdict_            = result.verdict;
        last_checked_token_ = transaction.meta.commit_token;
      }
    }
  }

  void require_quiescence() {
    const auto gate_metrics = gate_.metrics();
    if (gate_metrics.pending_transactions != 0 ||
        gate_metrics.active_starts != 0) {
      (void)record_error(runtime_error(
          RuntimeErrorCode::IncompletePrefix,
          "quiescent barrier reached with pending transactions or active "
          "starts"));
    }
  }

  void update_highest_observed(const CommitOrderToken &token) {
    std::lock_guard lock(state_mutex_);
    if (!highest_observed_token_.has_value() ||
        compareCommitOrderTokens(*highest_observed_token_, token) ==
            TokenRelation::Less) {
      highest_observed_token_ = token;
    }
    update_token_lag_locked();
  }

  void update_token_lag_locked() noexcept {
    current_commit_token_lag_ = 0;
    if (highest_observed_token_.has_value() &&
        last_ordered_token_.has_value() &&
        highest_observed_token_->cluster_epoch ==
            last_ordered_token_->cluster_epoch &&
        highest_observed_token_->timestamp >= last_ordered_token_->timestamp) {
      current_commit_token_lag_ =
          highest_observed_token_->timestamp - last_ordered_token_->timestamp;
      max_commit_token_lag_ =
          std::max(max_commit_token_lag_, current_commit_token_lag_);
    }
  }

  RuntimeOptions options_;
  BoundedMpscQueue<RuntimeMessage> queue_;
  CommitOrderGate gate_;
  SnapshotSOWWEngine engine_;
  mutable std::mutex engine_mutex_;
  std::thread consumer_;

  mutable std::shared_mutex admission_mutex_;
  bool stopping_ = false;
  mutable std::mutex shutdown_mutex_;
  bool joined_ = false;

  mutable std::mutex state_mutex_;
  std::optional<RuntimeError> error_;
  std::optional<ConsistencyVerdict> verdict_;
  std::uint64_t published_transactions_ = 0;
  std::uint64_t processed_messages_     = 0;
  std::uint64_t processed_controls_     = 0;
  std::uint64_t ordered_transactions_   = 0;
  std::uint64_t checked_transactions_   = 0;
  std::uint64_t checked_events_         = 0;
  std::uint64_t violating_transactions_ = 0;
  std::chrono::nanoseconds total_queue_lag_{};
  std::chrono::nanoseconds max_queue_lag_{};
  std::chrono::nanoseconds total_ordering_time_{};
  std::chrono::nanoseconds total_checking_time_{};
  std::chrono::nanoseconds max_checking_time_{};
  std::optional<CommitOrderToken> highest_observed_token_;
  std::optional<CommitOrderToken> last_ordered_token_;
  std::optional<CommitOrderToken> last_checked_token_;
  std::uint64_t current_commit_token_lag_ = 0;
  std::uint64_t max_commit_token_lag_     = 0;
  bool consumer_running_                  = false;
};

OnlineRuntime::OnlineRuntime(RuntimeOptions options,
                             std::vector<ProducerId> required_producers)
    : impl_(std::make_unique<Impl>(std::move(options),
                                   std::move(required_producers))) {}

OnlineRuntime::~OnlineRuntime() = default;

RuntimeBatchSubmitResult
OnlineRuntime::submitCommitted(PackedBatchEnvelope batch,
                               const bool read_only) {
  return impl_->submit_committed(std::move(batch), read_only);
}

RuntimeBatchSubmitResult
OnlineRuntime::submitCommitted(std::unique_ptr<VectorBatch> batch,
                               const bool read_only) {
  return submitCommitted(PackedBatchEnvelope(std::move(batch)), read_only);
}

RuntimeBatchSubmitResult
OnlineRuntime::submitCommitted(std::unique_ptr<ChunkBatch> batch,
                               const bool read_only) {
  return submitCommitted(PackedBatchEnvelope(std::move(batch)), read_only);
}

RuntimeControlResult
OnlineRuntime::registerActiveStart(const TransactionAttemptUid &attempt_id,
                                   const ProducerId producer_id,
                                   const CommitOrderToken &start_token) {
  return impl_->register_active_start(attempt_id, producer_id, start_token);
}

RuntimeControlResult
OnlineRuntime::abortActiveStart(const TransactionAttemptUid &attempt_id) {
  return impl_->abort_active_start(attempt_id);
}

RuntimeControlResult
OnlineRuntime::updateDatabaseFrontier(const CommitOrderToken &frontier) {
  return impl_->update_database_frontier(frontier);
}

RuntimeControlResult OnlineRuntime::updateProducerDeliveryFrontier(
    const ProducerId producer_id, const CommitOrderToken &frontier,
    const ProducerSeq delivered_sequence) {
  return impl_->update_producer_frontier(producer_id, frontier,
                                         delivered_sequence);
}

RuntimeControlResult OnlineRuntime::flush() { return impl_->flush(false); }

RuntimeControlResult OnlineRuntime::quiescentBarrier() {
  return impl_->flush(true);
}

RuntimeControlResult OnlineRuntime::shutdown() { return impl_->shutdown(); }

RuntimeControlResult OnlineRuntime::fail(RuntimeError error) {
  return impl_->fail(std::move(error));
}

std::optional<ConsistencyVerdict> OnlineRuntime::verdict() const {
  return impl_->verdict();
}

std::optional<RuntimeError> OnlineRuntime::error() const {
  return impl_->error();
}

RuntimeMetrics OnlineRuntime::metrics() const { return impl_->metrics(); }

} // namespace co_checker::online
