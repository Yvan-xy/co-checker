#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace co_checker::online {

enum class ByteBudgetErrorCode : std::uint8_t {
  InvalidCapacity,
  InvalidReservation,
  OversizedReservation,
  ReservationTimeout,
  ResourceFailure,
  Shutdown,
};

struct ByteBudgetError {
  ByteBudgetErrorCode code = ByteBudgetErrorCode::Shutdown;
  std::string message;

  friend bool operator==(const ByteBudgetError &,
                         const ByteBudgetError &) = default;
};

struct ByteBudgetOptions {
  std::size_t capacity_bytes                   = 0;
  std::chrono::nanoseconds reservation_timeout = std::chrono::seconds{30};
};

struct ByteBudgetMetrics {
  std::size_t capacity_bytes            = 0;
  std::size_t reserved_bytes            = 0;
  std::size_t peak_reserved_bytes       = 0;
  std::size_t active_leases             = 0;
  std::size_t peak_active_leases        = 0;
  std::uint64_t successful_reservations = 0;
  std::uint64_t backpressure_wait_count = 0;
  std::chrono::nanoseconds total_backpressure_time{};
};

namespace detail {

struct ByteBudgetState {
  explicit ByteBudgetState(ByteBudgetOptions configured_options)
      : options(configured_options) {
    metrics.capacity_bytes = options.capacity_bytes;
    if (options.capacity_bytes == 0U) {
      error = ByteBudgetError{ByteBudgetErrorCode::InvalidCapacity,
                              "byte budget capacity must be greater than zero"};
    }
    if (options.reservation_timeout < std::chrono::nanoseconds::zero()) {
      options.reservation_timeout = std::chrono::nanoseconds::zero();
    }
  }

  ByteBudgetOptions options;
  mutable std::mutex mutex;
  std::condition_variable changed;
  ByteBudgetMetrics metrics;
  std::optional<ByteBudgetError> error;
};

} // namespace detail

class ByteBudgetLease {
public:
  ByteBudgetLease() = default;
  ~ByteBudgetLease() { reset(); }

  ByteBudgetLease(const ByteBudgetLease &)            = delete;
  ByteBudgetLease &operator=(const ByteBudgetLease &) = delete;

  ByteBudgetLease(ByteBudgetLease &&other) noexcept
      : state_(std::move(other.state_)),
        bytes_(std::exchange(other.bytes_, 0)) {}

  ByteBudgetLease &operator=(ByteBudgetLease &&other) noexcept {
    if (this != &other) {
      reset();
      state_ = std::move(other.state_);
      bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return state_ != nullptr;
  }
  [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

  void reset() noexcept {
    if (state_ == nullptr) {
      return;
    }
    auto state       = std::move(state_);
    const auto bytes = std::exchange(bytes_, 0);
    {
      std::lock_guard lock(state->mutex);
      if (state->metrics.reserved_bytes >= bytes &&
          state->metrics.active_leases != 0U) {
        state->metrics.reserved_bytes -= bytes;
        --state->metrics.active_leases;
      } else if (!state->error.has_value()) {
        state->error =
            ByteBudgetError{ByteBudgetErrorCode::ResourceFailure,
                            "byte budget lease accounting underflow"};
      }
    }
    state->changed.notify_all();
  }

private:
  friend class ByteBudget;

  ByteBudgetLease(std::shared_ptr<detail::ByteBudgetState> state,
                  const std::size_t bytes) noexcept
      : state_(std::move(state)), bytes_(bytes) {}

  std::shared_ptr<detail::ByteBudgetState> state_;
  std::size_t bytes_ = 0;
};

struct ByteReservationResult {
  ByteBudgetLease lease;
  std::optional<ByteBudgetError> error;

  [[nodiscard]] bool ok() const noexcept {
    return static_cast<bool>(lease) && !error.has_value();
  }
};

// Global transient-memory admission. A lease may safely outlive this facade;
// closing the budget stops new reservations while existing owners retain and
// eventually release their exact charge.
class ByteBudget {
public:
  explicit ByteBudget(ByteBudgetOptions options)
      : state_(std::make_shared<detail::ByteBudgetState>(options)) {}

  ~ByteBudget() { (void)close(); }

  ByteBudget(const ByteBudget &)            = delete;
  ByteBudget &operator=(const ByteBudget &) = delete;
  ByteBudget(ByteBudget &&)                 = delete;
  ByteBudget &operator=(ByteBudget &&)      = delete;

  [[nodiscard]] ByteReservationResult reserve(const std::size_t bytes) {
    return reserveFor(bytes, state_->options.reservation_timeout);
  }

  template <typename Rep, typename Period>
  [[nodiscard]] ByteReservationResult
  reserveFor(const std::size_t bytes,
             const std::chrono::duration<Rep, Period> timeout) {
    auto state = state_;
    std::unique_lock lock(state->mutex);
    if (state->error.has_value()) {
      return rejectedLocked(*state);
    }
    if (bytes == 0U) {
      return failLocked(lock, *state, ByteBudgetErrorCode::InvalidReservation,
                        "byte reservation must be nonzero");
    }
    if (bytes > state->options.capacity_bytes) {
      return failLocked(lock, *state, ByteBudgetErrorCode::OversizedReservation,
                        "byte reservation exceeds the configured capacity");
    }

    const auto has_capacity = [&] {
      return state->error.has_value() ||
             bytes <=
                 state->options.capacity_bytes - state->metrics.reserved_bytes;
    };
    if (!has_capacity()) {
      ++state->metrics.backpressure_wait_count;
      const auto wait_started = std::chrono::steady_clock::now();
      auto wait_duration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
      if (wait_duration < std::chrono::nanoseconds::zero()) {
        wait_duration = std::chrono::nanoseconds::zero();
      }
      bool awakened = true;
      if (wait_duration == std::chrono::nanoseconds::max()) {
        state->changed.wait(lock, has_capacity);
      } else {
        awakened = state->changed.wait_for(lock, wait_duration, has_capacity);
      }
      state->metrics.total_backpressure_time +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - wait_started);
      if (!awakened) {
        return failLocked(lock, *state, ByteBudgetErrorCode::ReservationTimeout,
                          "timed out waiting for transient byte capacity");
      }
      if (state->error.has_value()) {
        return rejectedLocked(*state);
      }
    }

    state->metrics.reserved_bytes += bytes;
    ++state->metrics.active_leases;
    ++state->metrics.successful_reservations;
    state->metrics.peak_reserved_bytes = std::max(
        state->metrics.peak_reserved_bytes, state->metrics.reserved_bytes);
    state->metrics.peak_active_leases = std::max(
        state->metrics.peak_active_leases, state->metrics.active_leases);
    return ByteReservationResult{ByteBudgetLease(std::move(state), bytes),
                                 std::nullopt};
  }

  [[nodiscard]] ByteBudgetError close() {
    auto state = state_;
    std::unique_lock lock(state->mutex);
    if (!state->error.has_value()) {
      state->error = ByteBudgetError{ByteBudgetErrorCode::Shutdown,
                                     "byte budget is closed"};
    }
    const auto error = *state->error;
    lock.unlock();
    state->changed.notify_all();
    return error;
  }

  [[nodiscard]] ByteBudgetError failResource(std::string message) {
    auto state = state_;
    std::unique_lock lock(state->mutex);
    if (!state->error.has_value()) {
      if (message.empty()) {
        message = "native transient-memory resource failed";
      }
      state->error = ByteBudgetError{ByteBudgetErrorCode::ResourceFailure,
                                     std::move(message)};
    }
    const auto error = *state->error;
    lock.unlock();
    state->changed.notify_all();
    return error;
  }

  [[nodiscard]] ByteBudgetMetrics metrics() const {
    std::lock_guard lock(state_->mutex);
    return state_->metrics;
  }

  [[nodiscard]] std::optional<ByteBudgetError> error() const {
    std::lock_guard lock(state_->mutex);
    return state_->error;
  }

private:
  [[nodiscard]] static ByteReservationResult
  rejectedLocked(const detail::ByteBudgetState &state) {
    return ByteReservationResult{ByteBudgetLease{}, state.error};
  }

  [[nodiscard]] static ByteReservationResult
  failLocked(std::unique_lock<std::mutex> &lock, detail::ByteBudgetState &state,
             const ByteBudgetErrorCode code, std::string message) {
    if (!state.error.has_value()) {
      state.error = ByteBudgetError{code, std::move(message)};
    }
    auto result = rejectedLocked(state);
    lock.unlock();
    state.changed.notify_all();
    return result;
  }

  std::shared_ptr<detail::ByteBudgetState> state_;
};

} // namespace co_checker::online
