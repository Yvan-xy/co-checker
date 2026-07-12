#include "co_checker/online/byte_budget.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace co_checker::online;
using namespace std::chrono_literals;

int failures = 0;

void check(const bool condition, const std::string_view expression,
           const std::string_view file, const int line) {
  if (!condition) {
    std::cerr << file << ':' << line << ": check failed: " << expression
              << '\n';
    ++failures;
  }
}

#define CHECK(expression)                                                      \
  check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

void test_lease_move_and_release_account_exactly() {
  ByteBudget budget(
      ByteBudgetOptions{.capacity_bytes = 100, .reservation_timeout = 1s});
  auto first = budget.reserve(40);
  CHECK(first.ok());
  CHECK(first.lease.bytes() == 40U);
  CHECK(budget.metrics().reserved_bytes == 40U);

  ByteBudgetLease moved(std::move(first.lease));
  CHECK(!first.lease);
  CHECK(moved.bytes() == 40U);
  CHECK(budget.metrics().active_leases == 1U);
  moved.reset();
  CHECK(budget.metrics().reserved_bytes == 0U);
  CHECK(budget.metrics().active_leases == 0U);
}

void test_stalled_holder_plateaus_then_timeout_is_sticky() {
  ByteBudget budget(
      ByteBudgetOptions{.capacity_bytes = 64, .reservation_timeout = 30ms});
  auto holder = budget.reserve(64);
  CHECK(holder.ok());

  const auto timed_out = budget.reserve(1);
  CHECK(!timed_out.ok());
  CHECK(timed_out.error.has_value());
  CHECK(timed_out.error->code == ByteBudgetErrorCode::ReservationTimeout);
  const auto metrics = budget.metrics();
  CHECK(metrics.reserved_bytes == 64U);
  CHECK(metrics.peak_reserved_bytes == 64U);
  CHECK(metrics.backpressure_wait_count == 1U);
  CHECK(metrics.total_backpressure_time >= 20ms);

  holder.lease.reset();
  CHECK(budget.metrics().reserved_bytes == 0U);
  const auto rejected = budget.reserve(1);
  CHECK(rejected.error == timed_out.error);
}

void test_close_wakes_waiter_and_existing_lease_outlives_facade() {
  ByteBudgetLease surviving;
  {
    ByteBudget budget(
        ByteBudgetOptions{.capacity_bytes = 8, .reservation_timeout = 5s});
    auto holder = budget.reserve(8);
    CHECK(holder.ok());
    surviving = std::move(holder.lease);

    ByteReservationResult blocked;
    std::thread waiter([&] { blocked = budget.reserve(8); });
    while (budget.metrics().backpressure_wait_count == 0U) {
      std::this_thread::yield();
    }
    const auto error = budget.close();
    CHECK(error.code == ByteBudgetErrorCode::Shutdown);
    waiter.join();
    CHECK(!blocked.ok());
    CHECK(blocked.error.has_value());
    CHECK(blocked.error->code == ByteBudgetErrorCode::Shutdown);
  }
  CHECK(surviving.bytes() == 8U);
  surviving.reset();
}

void test_concurrent_reservations_never_exceed_capacity() {
  constexpr std::size_t capacity     = 256;
  constexpr std::size_t thread_count = 8;
  constexpr std::size_t iterations   = 300;
  ByteBudget budget(ByteBudgetOptions{.capacity_bytes      = capacity,
                                      .reservation_timeout = 10s});
  std::atomic<std::size_t> errors{0};
  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (std::size_t thread_index = 0; thread_index < thread_count;
       ++thread_index) {
    threads.emplace_back([&, thread_index] {
      for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto bytes = 1U + ((thread_index * 17U + iteration * 13U) % 64U);
        auto reservation = budget.reserve(bytes);
        if (!reservation.ok()) {
          errors.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        const auto metrics = budget.metrics();
        if (metrics.reserved_bytes > metrics.capacity_bytes) {
          errors.fetch_add(1, std::memory_order_relaxed);
        }
        if ((iteration % 11U) == 0U) {
          std::this_thread::yield();
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  CHECK(errors.load(std::memory_order_relaxed) == 0U);
  const auto metrics = budget.metrics();
  CHECK(metrics.reserved_bytes == 0U);
  CHECK(metrics.peak_reserved_bytes <= capacity);
  CHECK(metrics.successful_reservations == thread_count * iterations);
}

void test_invalid_and_oversized_reservations_fail_closed() {
  {
    ByteBudget budget(
        ByteBudgetOptions{.capacity_bytes = 16, .reservation_timeout = 1s});
    const auto invalid = budget.reserve(0);
    CHECK(invalid.error.has_value());
    CHECK(invalid.error->code == ByteBudgetErrorCode::InvalidReservation);
  }
  {
    ByteBudget budget(
        ByteBudgetOptions{.capacity_bytes = 16, .reservation_timeout = 1s});
    const auto oversized = budget.reserve(17);
    CHECK(oversized.error.has_value());
    CHECK(oversized.error->code == ByteBudgetErrorCode::OversizedReservation);
  }
}

} // namespace

int run_byte_budget_tests() {
  test_lease_move_and_release_account_exactly();
  test_stalled_holder_plateaus_then_timeout_is_sticky();
  test_close_wakes_waiter_and_existing_lease_outlives_facade();
  test_concurrent_reservations_never_exceed_capacity();
  test_invalid_and_oversized_reservations_fail_closed();
  return failures;
}
