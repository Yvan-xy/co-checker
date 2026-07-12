#include "co_checker/online/bounded_mpsc_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
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
  if (condition) {
    return;
  }
  std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
  ++failures;
}

#define CHECK(expression)                                                      \
  check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

struct LifetimeState {
  std::atomic<std::size_t> destroyed{0};
};

struct Payload {
  std::uint64_t id = 0;
  std::shared_ptr<LifetimeState> lifetime;

  Payload(const std::uint64_t payload_id,
          std::shared_ptr<LifetimeState> lifetime_state)
      : id(payload_id), lifetime(std::move(lifetime_state)) {}

  Payload(const Payload &)            = delete;
  Payload &operator=(const Payload &) = delete;

  ~Payload() {
    if (lifetime) {
      lifetime->destroyed.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

using Item  = std::unique_ptr<Payload>;
using Queue = BoundedMpscQueue<Item>;

[[nodiscard]] Item makeItem(const std::uint64_t id,
                            const std::shared_ptr<LifetimeState> &lifetime) {
  return std::make_unique<Payload>(id, lifetime);
}

void waitUntilBackpressured(const Queue &queue) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (queue.metrics().backpressure_wait_count == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  CHECK(queue.metrics().backpressure_wait_count != 0);
}

void test_control_bypasses_full_data_budget_in_global_order() {
  auto lifetime = std::make_shared<LifetimeState>();
  {
    Queue queue(BoundedQueueOptions{.capacity_bytes = 16, .push_timeout = 5s});

    auto first              = makeItem(1, lifetime);
    const auto first_result = queue.pushData(std::move(first), 16);
    CHECK(first_result.accepted);
    CHECK(!first);

    auto blocked = makeItem(3, lifetime);
    QueuePushResult blocked_result;
    std::thread producer(
        [&] { blocked_result = queue.pushData(std::move(blocked), 8); });
    waitUntilBackpressured(queue);

    auto control              = makeItem(2, lifetime);
    const auto control_result = queue.pushControl(std::move(control));
    CHECK(control_result.accepted);
    CHECK(!control);

    const auto saturated_metrics = queue.metrics();
    CHECK(saturated_metrics.queued_bytes == 16);
    CHECK(saturated_metrics.queued_depth == 2);
    CHECK(saturated_metrics.queued_data_depth == 1);
    CHECK(saturated_metrics.queued_control_depth == 1);

    auto first_pop = queue.waitPop();
    CHECK(first_pop.hasEntry());
    CHECK(first_pop.entry->value->id == 1);
    CHECK(first_pop.entry->kind == QueueEntryKind::Data);
    CHECK(first_pop.entry->enqueue_sequence == 0);

    producer.join();
    CHECK(blocked_result.accepted);
    CHECK(!blocked);
    (void)queue.close();

    auto control_pop = queue.waitPop();
    CHECK(control_pop.hasEntry());
    CHECK(control_pop.entry->value->id == 2);
    CHECK(control_pop.entry->kind == QueueEntryKind::Control);
    CHECK(control_pop.entry->charged_reserved_bytes == 0);
    CHECK(control_pop.entry->enqueue_sequence == 1);

    auto blocked_pop = queue.waitPop();
    CHECK(blocked_pop.hasEntry());
    CHECK(blocked_pop.entry->value->id == 3);
    CHECK(blocked_pop.entry->kind == QueueEntryKind::Data);
    CHECK(blocked_pop.entry->enqueue_sequence == 2);

    const auto terminal = queue.waitPop();
    CHECK(terminal.isTerminal());
    CHECK(terminal.terminal_error->code == QueueErrorCode::Shutdown);

    const auto metrics = queue.metrics();
    CHECK(metrics.queued_bytes == 0);
    CHECK(metrics.queued_depth == 0);
    CHECK(metrics.high_water_queued_bytes == 16);
    CHECK(metrics.high_water_queued_depth == 2);
    CHECK(metrics.total_enqueued == 3);
    CHECK(metrics.total_dequeued == 3);
    CHECK(metrics.total_backpressure_time > 0ns);
  }
  CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == 3);
}

void test_timeout_is_sticky_and_stalled_bytes_plateau() {
  auto lifetime = std::make_shared<LifetimeState>();
  {
    Queue queue(
        BoundedQueueOptions{.capacity_bytes = 128, .push_timeout = 35ms});
    for (std::uint64_t id = 0; id < 4; ++id) {
      auto item         = makeItem(id, lifetime);
      const auto result = queue.pushData(std::move(item), 32);
      CHECK(result.accepted);
      CHECK(!item);
    }

    auto timed_out_item = makeItem(10, lifetime);
    QueuePushResult timeout_result;
    std::atomic<bool> finished{false};
    std::thread producer([&] {
      timeout_result = queue.pushData(std::move(timed_out_item), 32);
      finished.store(true, std::memory_order_release);
    });

    while (!finished.load(std::memory_order_acquire)) {
      const auto metrics = queue.metrics();
      CHECK(metrics.queued_bytes == 128);
      CHECK(metrics.queued_bytes <= metrics.capacity_bytes);
      CHECK(metrics.queued_depth == 4);
      std::this_thread::sleep_for(1ms);
    }
    producer.join();

    CHECK(!timeout_result.accepted);
    CHECK(timeout_result.error.has_value());
    CHECK(timeout_result.error->code == QueueErrorCode::PushTimeout);
    CHECK(timed_out_item != nullptr);
    CHECK(queue.error() == timeout_result.error);

    auto rejected_control     = makeItem(11, lifetime);
    const auto control_result = queue.pushControl(std::move(rejected_control));
    CHECK(!control_result.accepted);
    CHECK(control_result.error == timeout_result.error);
    CHECK(rejected_control != nullptr);

    for (std::uint64_t expected = 0; expected < 4; ++expected) {
      auto popped = queue.waitPop();
      CHECK(popped.hasEntry());
      CHECK(popped.entry->value->id == expected);
    }
    const auto terminal = queue.waitPop();
    CHECK(terminal.isTerminal());
    CHECK(terminal.terminal_error->code == QueueErrorCode::PushTimeout);

    const auto metrics = queue.metrics();
    CHECK(metrics.queued_bytes == 0);
    CHECK(metrics.high_water_queued_bytes == 128);
    CHECK(metrics.high_water_queued_bytes <= metrics.capacity_bytes);
    CHECK(metrics.backpressure_wait_count == 1);
    CHECK(metrics.total_backpressure_time > 0ns);
  }
  CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == 6);
}

void test_close_wakes_waiters_and_drains_accepted_entries() {
  auto lifetime = std::make_shared<LifetimeState>();
  {
    Queue queue(BoundedQueueOptions{.capacity_bytes = 8, .push_timeout = 10s});
    auto accepted = makeItem(1, lifetime);
    CHECK(queue.pushData(std::move(accepted), 8).accepted);

    auto blocked = makeItem(2, lifetime);
    QueuePushResult blocked_result;
    std::thread producer(
        [&] { blocked_result = queue.pushData(std::move(blocked), 8); });
    waitUntilBackpressured(queue);

    const auto close_error = queue.close();
    CHECK(close_error.code == QueueErrorCode::Shutdown);
    producer.join();
    CHECK(!blocked_result.accepted);
    CHECK(blocked_result.error.has_value());
    CHECK(blocked_result.error->code == QueueErrorCode::Shutdown);
    CHECK(blocked != nullptr);

    auto accepted_pop = queue.waitPop();
    CHECK(accepted_pop.hasEntry());
    CHECK(accepted_pop.entry->value->id == 1);
    const auto terminal = queue.waitPop();
    CHECK(terminal.isTerminal());
    CHECK(terminal.terminal_error->code == QueueErrorCode::Shutdown);
  }
  CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == 2);
}

void test_oversized_and_invalid_charge_preserve_caller_ownership() {
  auto lifetime = std::make_shared<LifetimeState>();
  {
    Queue queue(BoundedQueueOptions{.capacity_bytes = 16, .push_timeout = 1s});
    auto oversized    = makeItem(1, lifetime);
    const auto result = queue.pushData(std::move(oversized), 17);
    CHECK(!result.accepted);
    CHECK(result.error.has_value());
    CHECK(result.error->code == QueueErrorCode::OversizedItem);
    CHECK(oversized != nullptr);
    const auto terminal = queue.waitPop();
    CHECK(terminal.isTerminal());
    CHECK(terminal.terminal_error == result.error);
  }

  {
    Queue queue(BoundedQueueOptions{.capacity_bytes = 16, .push_timeout = 1s});
    auto zero_charge  = makeItem(2, lifetime);
    const auto result = queue.pushData(std::move(zero_charge), 0);
    CHECK(!result.accepted);
    CHECK(result.error.has_value());
    CHECK(result.error->code == QueueErrorCode::InvalidDataCharge);
    CHECK(zero_charge != nullptr);
  }
  CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == 2);
}

void test_producer_failure_is_sticky_after_accepted_drain() {
  auto lifetime = std::make_shared<LifetimeState>();
  {
    Queue queue(BoundedQueueOptions{.capacity_bytes = 16, .push_timeout = 1s});
    auto accepted = makeItem(1, lifetime);
    CHECK(queue.pushData(std::move(accepted), 8).accepted);

    const auto failure = queue.failProducer("producer 7 stopped");
    CHECK(failure.code == QueueErrorCode::ProducerFailure);
    CHECK(failure.message == "producer 7 stopped");
    CHECK(queue.close() == failure);

    auto rejected              = makeItem(2, lifetime);
    const auto rejected_result = queue.pushData(std::move(rejected), 8);
    CHECK(!rejected_result.accepted);
    CHECK(rejected_result.error == failure);
    CHECK(rejected != nullptr);

    auto popped = queue.waitPop();
    CHECK(popped.hasEntry());
    CHECK(popped.entry->value->id == 1);
    const auto terminal = queue.waitPop();
    CHECK(terminal.isTerminal());
    CHECK(terminal.terminal_error == failure);
  }
  CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == 2);
}

void test_many_producers_have_no_loss_or_duplicates() {
  constexpr std::size_t producer_count     = 8;
  constexpr std::size_t items_per_producer = 600;
  constexpr std::size_t total_items = producer_count * items_per_producer;
  constexpr std::size_t charge      = 32;

  auto lifetime = std::make_shared<LifetimeState>();
  Queue queue(
      BoundedQueueOptions{.capacity_bytes = charge * 32, .push_timeout = 30s});

  std::vector<unsigned char> seen(total_items, 0);
  std::atomic<std::size_t> duplicate_count{0};
  std::atomic<std::size_t> out_of_range_count{0};
  std::atomic<std::size_t> consumer_count{0};
  std::atomic<std::size_t> push_error_count{0};
  std::atomic<std::size_t> bad_transfer_count{0};
  std::atomic<std::uint64_t> terminal_code{
      static_cast<std::uint64_t>(QueueErrorCode::InvalidCapacity)};

  std::thread consumer([&] {
    std::uint64_t expected_sequence = 0;
    while (true) {
      auto result = queue.waitPop();
      if (result.hasEntry()) {
        if (result.entry->enqueue_sequence != expected_sequence) {
          duplicate_count.fetch_add(1, std::memory_order_relaxed);
        }
        ++expected_sequence;
        const auto id = result.entry->value->id;
        if (id >= total_items) {
          out_of_range_count.fetch_add(1, std::memory_order_relaxed);
        } else if (seen[static_cast<std::size_t>(id)] != 0) {
          duplicate_count.fetch_add(1, std::memory_order_relaxed);
        } else {
          seen[static_cast<std::size_t>(id)] = 1;
        }
        consumer_count.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (result.isTerminal()) {
        terminal_code.store(
            static_cast<std::uint64_t>(result.terminal_error->code),
            std::memory_order_relaxed);
        break;
      }
    }
  });

  std::vector<std::thread> producers;
  producers.reserve(producer_count);
  for (std::size_t producer = 0; producer < producer_count; ++producer) {
    producers.emplace_back([&, producer] {
      for (std::size_t item_index = 0; item_index < items_per_producer;
           ++item_index) {
        const auto id = static_cast<std::uint64_t>(
            producer * items_per_producer + item_index);
        auto item         = makeItem(id, lifetime);
        const auto result = queue.pushData(std::move(item), charge);
        if (!result.accepted) {
          push_error_count.fetch_add(1, std::memory_order_relaxed);
        } else if (item != nullptr) {
          bad_transfer_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto &producer : producers) {
    producer.join();
  }
  (void)queue.close();
  consumer.join();

  CHECK(push_error_count.load(std::memory_order_relaxed) == 0);
  CHECK(bad_transfer_count.load(std::memory_order_relaxed) == 0);
  CHECK(consumer_count.load(std::memory_order_relaxed) == total_items);
  CHECK(duplicate_count.load(std::memory_order_relaxed) == 0);
  CHECK(out_of_range_count.load(std::memory_order_relaxed) == 0);
  CHECK(std::all_of(seen.begin(), seen.end(),
                    [](const unsigned char value) { return value == 1; }));
  CHECK(terminal_code.load(std::memory_order_relaxed) ==
        static_cast<std::uint64_t>(QueueErrorCode::Shutdown));

  const auto metrics = queue.metrics();
  CHECK(metrics.queued_bytes == 0);
  CHECK(metrics.queued_depth == 0);
  CHECK(metrics.high_water_queued_bytes <= metrics.capacity_bytes);
  CHECK(metrics.total_enqueued == total_items);
  CHECK(metrics.total_dequeued == total_items);
  CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == total_items);
}

} // namespace

int run_bounded_queue_tests() {
  test_control_bypasses_full_data_budget_in_global_order();
  test_timeout_is_sticky_and_stalled_bytes_plateau();
  test_close_wakes_waiters_and_drains_accepted_entries();
  test_oversized_and_invalid_charge_preserve_caller_ownership();
  test_producer_failure_is_sticky_after_accepted_drain();
  test_many_producers_have_no_loss_or_duplicates();
  return failures;
}

#ifdef CO_CHECKER_ONLINE_BOUNDED_QUEUE_TEST_MAIN
int main() {
  return run_bounded_queue_tests() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
