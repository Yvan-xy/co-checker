#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace co_checker::online {

enum class QueueErrorCode : std::uint8_t {
  InvalidCapacity,
  InvalidDataCharge,
  OversizedItem,
  PushTimeout,
  Shutdown,
  ProducerFailure,
  ResourceExhausted,
  SequenceExhausted,
};

struct QueueError {
  QueueErrorCode code = QueueErrorCode::Shutdown;
  std::string message;

  friend bool operator==(const QueueError &, const QueueError &) = default;
};

struct QueuePushResult {
  bool accepted = false;
  std::optional<QueueError> error;

  [[nodiscard]] bool ok() const noexcept { return accepted; }
};

enum class QueueEntryKind : std::uint8_t {
  Data,
  Control,
};

struct BoundedQueueOptions {
  std::size_t capacity_bytes            = 0;
  std::chrono::nanoseconds push_timeout = std::chrono::seconds{30};
};

struct BoundedQueueMetrics {
  std::size_t capacity_bytes            = 0;
  std::size_t reserved_bytes            = 0;
  std::size_t reserved_depth            = 0;
  std::size_t queued_bytes              = 0;
  std::size_t queued_depth              = 0;
  std::size_t queued_data_depth         = 0;
  std::size_t queued_control_depth      = 0;
  std::size_t high_water_queued_bytes   = 0;
  std::size_t high_water_queued_depth   = 0;
  std::uint64_t total_enqueued          = 0;
  std::uint64_t total_dequeued          = 0;
  std::uint64_t backpressure_wait_count = 0;
  std::chrono::nanoseconds total_backpressure_time{};
};

// A bounded many-producer, single-consumer ownership handoff. Successful
// enqueue is the ownership-transfer point. Failed enqueue never moves from the
// caller's value. T must therefore have a non-throwing move constructor.
//
// Data entries wait for their complete byte charge to fit. Control entries are
// admitted with zero charge without waiting for data capacity. Both paths take
// the same mutex, receive a single monotonic sequence, and enter the same
// deque, so the consumer observes the global successful-enqueue order.
template <typename T> class BoundedMpscQueue {
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "queue values must be nothrow move constructible");

public:
  struct Entry {
    T value;
    QueueEntryKind kind                = QueueEntryKind::Data;
    std::size_t charged_reserved_bytes = 0;
    std::uint64_t enqueue_sequence     = 0;

    Entry(T &&entry_value, const QueueEntryKind entry_kind,
          const std::size_t charged_bytes,
          const std::uint64_t sequence) noexcept
        : value(std::move(entry_value)), kind(entry_kind),
          charged_reserved_bytes(charged_bytes), enqueue_sequence(sequence) {}

    Entry(Entry &&) noexcept            = default;
    Entry &operator=(Entry &&) noexcept = default;
    Entry(const Entry &)                = delete;
    Entry &operator=(const Entry &)     = delete;
  };

private:
  struct Node {
    std::optional<Entry> entry;
    std::unique_ptr<Node> next;
  };

public:
  // A reservation owns byte admission and one already allocated queue node.
  // Publishing a reserved value therefore neither waits for capacity nor
  // allocates. Destroying an unused reservation releases its byte admission.
  class DataReservation final {
  public:
    DataReservation() = default;
    DataReservation(DataReservation &&other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)),
          node_(std::move(other.node_)),
          charged_bytes_(std::exchange(other.charged_bytes_, 0)) {}

    DataReservation &operator=(DataReservation &&other) noexcept {
      if (this == &other) {
        return *this;
      }
      reset();
      owner_         = std::exchange(other.owner_, nullptr);
      node_          = std::move(other.node_);
      charged_bytes_ = std::exchange(other.charged_bytes_, 0);
      return *this;
    }

    DataReservation(const DataReservation &)            = delete;
    DataReservation &operator=(const DataReservation &) = delete;
    ~DataReservation() { reset(); }

    [[nodiscard]] bool valid() const noexcept {
      return owner_ != nullptr && node_ != nullptr && charged_bytes_ != 0;
    }
    [[nodiscard]] std::size_t charged_bytes() const noexcept {
      return charged_bytes_;
    }

    void reset() noexcept {
      if (owner_ != nullptr) {
        owner_->cancelReservation(*this);
      }
      node_.reset();
      owner_         = nullptr;
      charged_bytes_ = 0;
    }

  private:
    friend class BoundedMpscQueue;
    DataReservation(BoundedMpscQueue *owner, std::unique_ptr<Node> node,
                    const std::size_t charged_bytes) noexcept
        : owner_(owner), node_(std::move(node)),
          charged_bytes_(charged_bytes) {}

    BoundedMpscQueue *owner_ = nullptr;
    std::unique_ptr<Node> node_;
    std::size_t charged_bytes_ = 0;
  };

  struct ReserveResult {
    std::optional<DataReservation> reservation;
    std::optional<QueueError> error;

    [[nodiscard]] bool ok() const noexcept {
      return reservation.has_value();
    }
  };

  struct PopResult {
    std::optional<Entry> entry;
    std::optional<QueueError> terminal_error;

    [[nodiscard]] bool hasEntry() const noexcept { return entry.has_value(); }
    [[nodiscard]] bool isTerminal() const noexcept {
      return terminal_error.has_value();
    }
  };

  explicit BoundedMpscQueue(BoundedQueueOptions options) : options_(options) {
    metrics_.capacity_bytes = options_.capacity_bytes;
    if (options_.capacity_bytes == 0) {
      sticky_error_ =
          QueueError{QueueErrorCode::InvalidCapacity,
                     "bounded queue capacity must be greater than zero"};
    }
    if (options_.push_timeout < std::chrono::nanoseconds::zero()) {
      options_.push_timeout = std::chrono::nanoseconds::zero();
    }
  }

  ~BoundedMpscQueue() { (void)close(); }

  BoundedMpscQueue(const BoundedMpscQueue &)            = delete;
  BoundedMpscQueue &operator=(const BoundedMpscQueue &) = delete;
  BoundedMpscQueue(BoundedMpscQueue &&)                 = delete;
  BoundedMpscQueue &operator=(BoundedMpscQueue &&)      = delete;

  [[nodiscard]] QueuePushResult pushData(T &&value,
                                         const std::size_t charged_bytes) {
    return pushDataFor(std::move(value), charged_bytes, options_.push_timeout);
  }

  template <typename Rep, typename Period>
  [[nodiscard]] QueuePushResult
  pushDataFor(T &&value, const std::size_t charged_bytes,
              const std::chrono::duration<Rep, Period> timeout) {
    auto reserved = reserveDataFor(charged_bytes, timeout);
    if (!reserved.ok()) {
      return QueuePushResult{false, std::move(reserved.error)};
    }
    return pushReserved(std::move(value),
                        std::move(*reserved.reservation));
  }

  [[nodiscard]] ReserveResult reserveData(const std::size_t charged_bytes) {
    return reserveDataFor(charged_bytes, options_.push_timeout);
  }

  template <typename Rep, typename Period>
  [[nodiscard]] ReserveResult
  reserveDataFor(const std::size_t charged_bytes,
                 const std::chrono::duration<Rep, Period> timeout) {
    if (charged_bytes == 0) {
      return failReservation(
          QueueErrorCode::InvalidDataCharge,
          "data entries must have a nonzero reserved-byte charge");
    }
    if (charged_bytes > options_.capacity_bytes) {
      return failReservation(
          QueueErrorCode::OversizedItem,
          "data entry charge exceeds the queue byte capacity");
    }

    std::unique_ptr<Node> node;
    try {
      node = std::make_unique<Node>();
    } catch (const std::bad_alloc &) {
      return failReservation(
          QueueErrorCode::ResourceExhausted,
          "bounded queue could not allocate a reserved entry");
    }

    std::unique_lock lock(mutex_);
    if (sticky_error_.has_value()) {
      return ReserveResult{std::nullopt, sticky_error_};
    }
    if (data_admission_closed_) {
      return ReserveResult{
          std::nullopt,
          QueueError{QueueErrorCode::Shutdown,
                     "bounded queue data admission is closed"}};
    }

    const bool backpressured = !hasCapacityLocked(charged_bytes);
    std::chrono::steady_clock::time_point wait_started;
    if (backpressured) {
      wait_started = std::chrono::steady_clock::now();
      ++metrics_.backpressure_wait_count;

      auto wait_duration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
      if (wait_duration < std::chrono::nanoseconds::zero()) {
        wait_duration = std::chrono::nanoseconds::zero();
      }

      const auto predicate = [this, charged_bytes] {
        return sticky_error_.has_value() || data_admission_closed_ ||
               hasCapacityLocked(charged_bytes);
      };
      bool awakened = true;
      if (wait_duration == std::chrono::nanoseconds::max()) {
        capacity_or_terminal_.wait(lock, predicate);
      } else {
        awakened =
            capacity_or_terminal_.wait_for(lock, wait_duration, predicate);
      }

      metrics_.total_backpressure_time +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - wait_started);
      if (!awakened) {
        auto result = failReservationLocked(
            QueueErrorCode::PushTimeout,
            "timed out waiting for bounded queue byte capacity");
        lock.unlock();
        capacity_or_terminal_.notify_all();
        not_empty_or_terminal_.notify_all();
        return result;
      }
      if (sticky_error_.has_value()) {
        return ReserveResult{std::nullopt, sticky_error_};
      }
      if (data_admission_closed_) {
        return ReserveResult{
            std::nullopt,
            QueueError{QueueErrorCode::Shutdown,
                       "bounded queue data admission is closed"}};
      }
    }

    metrics_.reserved_bytes += charged_bytes;
    ++metrics_.reserved_depth;
    return ReserveResult{DataReservation(this, std::move(node), charged_bytes),
                         std::nullopt};
  }

  [[nodiscard]] QueuePushResult
  pushReserved(T &&value, DataReservation reservation) {
    if (reservation.owner_ != this || !reservation.valid()) {
      std::unique_lock lock(mutex_);
      const auto result = failLocked(
          QueueErrorCode::InvalidDataCharge,
          "queue reservation does not belong to this queue");
      lock.unlock();
      capacity_or_terminal_.notify_all();
      not_empty_or_terminal_.notify_all();
      return result;
    }

    std::unique_lock lock(mutex_);
    if (sticky_error_.has_value()) {
      const auto result = rejectedLocked();
      releaseReservationLocked(reservation);
      lock.unlock();
      capacity_or_terminal_.notify_all();
      return result;
    }
    if (next_enqueue_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      const auto result = failLocked(
          QueueErrorCode::SequenceExhausted,
          "bounded queue enqueue sequence is exhausted");
      releaseReservationLocked(reservation);
      lock.unlock();
      capacity_or_terminal_.notify_all();
      not_empty_or_terminal_.notify_all();
      return result;
    }

    const auto charged_bytes = reservation.charged_bytes_;
    reservation.node_->entry.emplace(std::move(value), QueueEntryKind::Data,
                                     charged_bytes, next_enqueue_sequence_);
    appendNodeLocked(std::move(reservation.node_));
    reservation.owner_         = nullptr;
    reservation.charged_bytes_ = 0;

    ++next_enqueue_sequence_;
    metrics_.reserved_bytes -= charged_bytes;
    --metrics_.reserved_depth;
    accountEnqueueLocked(QueueEntryKind::Data, charged_bytes);
    lock.unlock();
    not_empty_or_terminal_.notify_one();
    return QueuePushResult{true, std::nullopt};
  }

  [[nodiscard]] QueuePushResult pushControl(T &&value) {
    std::unique_ptr<Node> node;
    try {
      node = std::make_unique<Node>();
    } catch (const std::bad_alloc &) {
      std::unique_lock lock(mutex_);
      const auto result = failLocked(
          QueueErrorCode::ResourceExhausted,
          "bounded queue could not allocate a control entry");
      lock.unlock();
      capacity_or_terminal_.notify_all();
      not_empty_or_terminal_.notify_all();
      return result;
    }

    std::unique_lock lock(mutex_);
    if (sticky_error_.has_value()) {
      return rejectedLocked();
    }
    const auto result =
        enqueueLocked(std::move(value), QueueEntryKind::Control, 0,
                      std::move(node));
    if (!result.accepted) {
      lock.unlock();
      capacity_or_terminal_.notify_all();
      not_empty_or_terminal_.notify_all();
      return result;
    }
    lock.unlock();
    not_empty_or_terminal_.notify_one();
    return result;
  }

  // Waits until the next globally ordered entry is available or the queue has
  // reached a terminal state and all previously accepted entries are drained.
  [[nodiscard]] PopResult waitPop() {
    std::unique_lock lock(mutex_);
    not_empty_or_terminal_.wait(lock, [this] {
      return head_ != nullptr || sticky_error_.has_value();
    });
    auto result = popLocked();
    const bool released_capacity =
        result.entry.has_value() && result.entry->charged_reserved_bytes != 0;
    lock.unlock();
    if (released_capacity) {
      capacity_or_terminal_.notify_all();
    }
    return result;
  }

  // Returns an empty, non-terminal result when the queue is still open but no
  // item is currently available.
  [[nodiscard]] PopResult tryPop() {
    std::unique_lock lock(mutex_);
    auto result = popLocked();
    const bool released_capacity =
        result.entry.has_value() && result.entry->charged_reserved_bytes != 0;
    lock.unlock();
    if (released_capacity) {
      capacity_or_terminal_.notify_all();
    }
    return result;
  }

  // Stops admission and wakes all waiters. Entries accepted before close stay
  // in FIFO order and remain available to the consumer before Shutdown is
  // returned as the terminal result.
  [[nodiscard]] QueueError close() {
    std::unique_lock lock(mutex_);
    if (!sticky_error_.has_value()) {
      setErrorLocked(QueueErrorCode::Shutdown, "bounded queue is closed");
    }
    const auto result = *sticky_error_;
    lock.unlock();
    capacity_or_terminal_.notify_all();
    not_empty_or_terminal_.notify_all();
    return result;
  }

  // Cancels waiters and future data reservations without preventing already
  // reserved data or FIFO control barriers from being enqueued.
  void closeDataAdmission() noexcept {
    {
      std::lock_guard lock(mutex_);
      data_admission_closed_ = true;
    }
    capacity_or_terminal_.notify_all();
  }

  [[nodiscard]] QueueError failProducer(std::string message) {
    std::unique_lock lock(mutex_);
    if (!sticky_error_.has_value()) {
      if (message.empty()) {
        message = "a required producer failed";
      }
      setErrorLocked(QueueErrorCode::ProducerFailure, std::move(message));
    }
    const auto result = *sticky_error_;
    lock.unlock();
    capacity_or_terminal_.notify_all();
    not_empty_or_terminal_.notify_all();
    return result;
  }

  [[nodiscard]] BoundedQueueMetrics metrics() const {
    std::lock_guard lock(mutex_);
    return metrics_;
  }

  [[nodiscard]] std::optional<QueueError> error() const {
    std::lock_guard lock(mutex_);
    return sticky_error_;
  }

private:
  [[nodiscard]] bool
  hasCapacityLocked(const std::size_t charged_bytes) const noexcept {
    const auto admitted = metrics_.queued_bytes + metrics_.reserved_bytes;
    return admitted <= options_.capacity_bytes &&
           charged_bytes <= options_.capacity_bytes - admitted;
  }

  [[nodiscard]] QueuePushResult
  enqueueLocked(T &&value, const QueueEntryKind kind,
                const std::size_t charged_bytes, std::unique_ptr<Node> node) {
    if (next_enqueue_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      return failLocked(QueueErrorCode::SequenceExhausted,
                        "bounded queue enqueue sequence is exhausted");
    }

    node->entry.emplace(std::move(value), kind, charged_bytes,
                        next_enqueue_sequence_);
    appendNodeLocked(std::move(node));

    ++next_enqueue_sequence_;
    accountEnqueueLocked(kind, charged_bytes);
    return QueuePushResult{true, std::nullopt};
  }

  void appendNodeLocked(std::unique_ptr<Node> node) noexcept {
    auto *raw = node.get();
    if (tail_ == nullptr) {
      head_ = std::move(node);
    } else {
      tail_->next = std::move(node);
    }
    tail_ = raw;
  }

  void accountEnqueueLocked(const QueueEntryKind kind,
                            const std::size_t charged_bytes) noexcept {
    metrics_.queued_bytes += charged_bytes;
    ++metrics_.queued_depth;
    if (kind == QueueEntryKind::Data) {
      ++metrics_.queued_data_depth;
    } else {
      ++metrics_.queued_control_depth;
    }
    metrics_.high_water_queued_bytes =
        std::max(metrics_.high_water_queued_bytes, metrics_.queued_bytes);
    metrics_.high_water_queued_depth =
        std::max(metrics_.high_water_queued_depth, metrics_.queued_depth);
    ++metrics_.total_enqueued;
  }

  [[nodiscard]] PopResult popLocked() {
    PopResult result;
    if (head_ == nullptr) {
      if (sticky_error_.has_value()) {
        result.terminal_error = sticky_error_;
      }
      return result;
    }

    result.entry.emplace(std::move(*head_->entry));
    head_ = std::move(head_->next);
    if (head_ == nullptr) {
      tail_ = nullptr;
    }
    metrics_.queued_bytes -= result.entry->charged_reserved_bytes;
    --metrics_.queued_depth;
    if (result.entry->kind == QueueEntryKind::Data) {
      --metrics_.queued_data_depth;
    } else {
      --metrics_.queued_control_depth;
    }
    ++metrics_.total_dequeued;
    return result;
  }

  [[nodiscard]] ReserveResult failReservation(const QueueErrorCode code,
                                               std::string message) {
    std::unique_lock lock(mutex_);
    auto result = failReservationLocked(code, std::move(message));
    lock.unlock();
    capacity_or_terminal_.notify_all();
    not_empty_or_terminal_.notify_all();
    return result;
  }

  [[nodiscard]] ReserveResult
  failReservationLocked(const QueueErrorCode code, std::string message) {
    if (!sticky_error_.has_value()) {
      setErrorLocked(code, std::move(message));
    }
    return ReserveResult{std::nullopt, sticky_error_};
  }

  void releaseReservationLocked(DataReservation &reservation) noexcept {
    metrics_.reserved_bytes -= reservation.charged_bytes_;
    --metrics_.reserved_depth;
    reservation.owner_         = nullptr;
    reservation.charged_bytes_ = 0;
  }

  void cancelReservation(DataReservation &reservation) noexcept {
    std::unique_lock lock(mutex_);
    if (reservation.owner_ == this && reservation.charged_bytes_ != 0) {
      releaseReservationLocked(reservation);
    }
    lock.unlock();
    capacity_or_terminal_.notify_all();
  }

  [[nodiscard]] QueuePushResult failLocked(const QueueErrorCode code,
                                           std::string message) {
    if (!sticky_error_.has_value()) {
      setErrorLocked(code, std::move(message));
    }
    return rejectedLocked();
  }

  void setErrorLocked(const QueueErrorCode code, std::string message) {
    sticky_error_ = QueueError{code, std::move(message)};
  }

  [[nodiscard]] QueuePushResult rejectedLocked() const {
    QueuePushResult result;
    result.error = sticky_error_;
    return result;
  }

  BoundedQueueOptions options_;
  mutable std::mutex mutex_;
  // Capacity waiters and the consumer have opposite predicates. Separate
  // condition variables prevent an enqueue notification from waking only a
  // producer that still cannot proceed while the consumer remains asleep.
  std::condition_variable capacity_or_terminal_;
  std::condition_variable not_empty_or_terminal_;
  std::unique_ptr<Node> head_;
  Node *tail_ = nullptr;
  BoundedQueueMetrics metrics_;
  std::uint64_t next_enqueue_sequence_ = 0;
  std::optional<QueueError> sticky_error_;
  bool data_admission_closed_ = false;
};

} // namespace co_checker::online
