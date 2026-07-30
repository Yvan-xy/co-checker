#include "co_checker/online/c_api.h"

#include "co_checker/online/packed_batch.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" int co_checker_online_c_header_smoke(void);

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

static_assert(sizeof(co_checker_online_options_v1) == 112);
static_assert(offsetof(co_checker_online_options_v1, mode) == 80);
static_assert(sizeof(co_checker_online_session_v1) == 32);
static_assert(sizeof(co_checker_online_txn_input_header_v1) == 64);
static_assert(sizeof(co_checker_online_event_input_v1) == 32);
static_assert(sizeof(co_checker_online_verdict_v1) == 104);
static_assert(sizeof(co_checker_online_error_info_v1) == 64);

struct RuntimeOwner {
  co_checker_online_runtime_v1 *runtime = nullptr;

  RuntimeOwner() = default;
  explicit RuntimeOwner(co_checker_online_runtime_v1 *value) : runtime(value) {}
  RuntimeOwner(const RuntimeOwner &)            = delete;
  RuntimeOwner &operator=(const RuntimeOwner &) = delete;
  RuntimeOwner(RuntimeOwner &&other) noexcept
      : runtime(std::exchange(other.runtime, nullptr)) {}
  RuntimeOwner &operator=(RuntimeOwner &&other) noexcept {
    if (this != &other) {
      co_checker_online_runtime_destroy_v1(runtime);
      runtime = std::exchange(other.runtime, nullptr);
    }
    return *this;
  }
  ~RuntimeOwner() { co_checker_online_runtime_destroy_v1(runtime); }
};

[[nodiscard]] co_checker_online_options_v1 test_options(
    const co_checker_online_mode_v1 mode = CO_CHECKER_ONLINE_MODE_FULL_V1,
    const co_checker_online_ordering_protocol_v1 ordering_protocol =
        CO_CHECKER_ONLINE_ORDERING_FRONTIER_REORDER_V1) {
  co_checker_online_options_v1 options{};
  co_checker_online_options_init_v1(&options);
  options.queue_capacity_bytes          = 1U << 20U;
  options.reorder_capacity_bytes =
      ordering_protocol == CO_CHECKER_ONLINE_ORDERING_FRONTIER_REORDER_V1
          ? 1U << 20U
          : 0U;
  options.capture_capacity_bytes        = 1U << 20U;
  options.retained_state_capacity_bytes = 1U << 20U;
  options.queue_timeout_ns              = 5'000'000'000ULL;
  options.capture_timeout_ns            = 5'000'000'000ULL;
  options.chunk_segment_bytes           = 64U;
  options.max_cached_chunk_segments     = 16U;
  options.cluster_epoch                 = 7U;
  options.mode                          = mode;
  options.ordering_protocol              = ordering_protocol;
  return options;
}

[[nodiscard]] RuntimeOwner
create_runtime(const co_checker_online_options_v1 &options,
               const std::vector<std::uint32_t> &producers = {1}) {
  co_checker_online_runtime_v1 *runtime = nullptr;
  CHECK(co_checker_online_runtime_create_v1(
            &options, producers.data(),
            static_cast<std::uint32_t>(producers.size()),
            &runtime) == CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(runtime != nullptr);
  return RuntimeOwner(runtime);
}

void advance_frontiers(co_checker_online_runtime_v1 *runtime,
                       const std::uint64_t timestamp) {
  CHECK(co_checker_online_database_frontier_v1(runtime, timestamp) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(co_checker_online_producer_frontier_v1(runtime, 1, timestamp) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
}

struct InputEvent {
  std::uint32_t sequence   = 0;
  std::uint8_t operation   = CO_CHECKER_ONLINE_EVENT_READ_V1;
  std::uint8_t value_state = CO_CHECKER_ONLINE_VALUE_ABSENT_V1;
  std::uint8_t read_source = CO_CHECKER_ONLINE_READ_SNAPSHOT_V1;
  std::string key;
  std::string value;
};

[[nodiscard]] InputEvent
read_event(const std::uint32_t sequence, std::string key,
           std::optional<std::string> value,
           const std::uint8_t source = CO_CHECKER_ONLINE_READ_SNAPSHOT_V1) {
  InputEvent result;
  result.sequence    = sequence;
  result.operation   = CO_CHECKER_ONLINE_EVENT_READ_V1;
  result.read_source = source;
  result.key         = std::move(key);
  if (value.has_value()) {
    result.value_state = CO_CHECKER_ONLINE_VALUE_PRESENT_V1;
    result.value       = std::move(*value);
  }
  return result;
}

[[nodiscard]] InputEvent write_event(const std::uint32_t sequence,
                                     std::string key,
                                     std::optional<std::string> value) {
  InputEvent result;
  result.sequence    = sequence;
  result.operation   = CO_CHECKER_ONLINE_EVENT_WRITE_V1;
  result.read_source = 0;
  result.key         = std::move(key);
  if (value.has_value()) {
    result.value_state = CO_CHECKER_ONLINE_VALUE_PRESENT_V1;
    result.value       = std::move(*value);
  }
  return result;
}

[[nodiscard]] std::vector<std::byte>
flat_input(const std::uint64_t start_ts, const std::uint64_t commit_ts,
           const bool read_only, const std::vector<InputEvent> &events) {
  const auto descriptor_bytes =
      events.size() * sizeof(co_checker_online_event_input_v1);
  std::size_t total =
      sizeof(co_checker_online_txn_input_header_v1) + descriptor_bytes;
  for (const auto &event : events) {
    total += event.key.size();
    if (event.value_state == CO_CHECKER_ONLINE_VALUE_PRESENT_V1) {
      total += event.value.size();
    }
  }
  std::vector<std::byte> result(total);
  co_checker_online_txn_input_header_v1 header{};
  co_checker_online_txn_input_header_init_v1(&header);
  header.total_size  = total;
  header.start_ts    = start_ts;
  header.commit_ts   = commit_ts;
  header.read_only   = read_only ? 1U : 0U;
  header.event_count = static_cast<std::uint32_t>(events.size());
  std::memcpy(result.data(), &header, sizeof(header));

  std::size_t payload_offset = sizeof(header) + descriptor_bytes;
  for (std::size_t index = 0; index < events.size(); ++index) {
    const auto &input = events[index];
    co_checker_online_event_input_v1 event{};
    event.sequence    = input.sequence;
    event.operation   = input.operation;
    event.value_state = input.value_state;
    event.read_source = input.read_source;
    if (!input.key.empty()) {
      event.key_offset = static_cast<std::uint32_t>(payload_offset);
      event.key_size   = static_cast<std::uint32_t>(input.key.size());
      std::memcpy(result.data() + payload_offset, input.key.data(),
                  input.key.size());
      payload_offset += input.key.size();
    }
    if (input.value_state == CO_CHECKER_ONLINE_VALUE_PRESENT_V1 &&
        !input.value.empty()) {
      event.value_offset = static_cast<std::uint32_t>(payload_offset);
      event.value_size   = static_cast<std::uint32_t>(input.value.size());
      std::memcpy(result.data() + payload_offset, input.value.data(),
                  input.value.size());
      payload_offset += input.value.size();
    }
    std::memcpy(result.data() + sizeof(header) +
                    index * sizeof(co_checker_online_event_input_v1),
                &event, sizeof(event));
  }
  CHECK(payload_offset == result.size());
  return result;
}

[[nodiscard]] co_checker_online_session_v1
new_session(co_checker_online_runtime_v1 *runtime,
            const std::uint32_t producer = 1) {
  co_checker_online_session_v1 session{};
  CHECK(co_checker_online_session_create_v1(runtime, producer, &session) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  return session;
}

struct RegisteredAttempt {
  co_checker_online_uid_v1 transaction{};
  co_checker_online_uid_v1 attempt{};
};

[[nodiscard]] RegisteredAttempt
register_attempt(co_checker_online_runtime_v1 *runtime,
                 const co_checker_online_session_v1 &session,
                 const std::uint64_t session_seq,
                 const std::uint64_t start_ts) {
  RegisteredAttempt result;
  CHECK(co_checker_online_logical_transaction_create_v1(
            runtime, &session, session_seq, &result.transaction) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(co_checker_online_attempt_register_v1(
            runtime, &session, &result.transaction, session_seq, start_ts,
            &result.attempt) == CO_CHECKER_ONLINE_STATUS_OK_V1);
  return result;
}

[[nodiscard]] std::string metrics_json(co_checker_online_runtime_v1 *runtime) {
  std::uint64_t required = 0;
  CHECK(co_checker_online_metrics_json_v1(runtime, nullptr, 0, &required) ==
        CO_CHECKER_ONLINE_STATUS_BUFFER_TOO_SMALL_V1);
  CHECK(required > 1U);
  const auto capacity = required + 4096U;
  std::vector<char> buffer(static_cast<std::size_t>(capacity));
  CHECK(co_checker_online_metrics_json_v1(runtime, buffer.data(), capacity,
                                          &required) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  return std::string(buffer.data());
}

[[nodiscard]] co_checker_online_error_info_v1
get_error(co_checker_online_runtime_v1 *runtime, std::string &message) {
  co_checker_online_error_info_v1 info{};
  std::uint64_t required = 0;
  const auto query =
      co_checker_online_error_get_v1(runtime, &info, nullptr, 0, &required);
  if (info.present == 0U) {
    CHECK(query == CO_CHECKER_ONLINE_STATUS_OK_V1);
    message.clear();
    return info;
  }
  CHECK(query == CO_CHECKER_ONLINE_STATUS_BUFFER_TOO_SMALL_V1);
  std::vector<char> buffer(static_cast<std::size_t>(required));
  CHECK(co_checker_online_error_get_v1(runtime, &info, buffer.data(), required,
                                       &required) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  message.assign(buffer.data());
  return info;
}

void test_c_header_and_fixed_layout() {
  CHECK(co_checker_online_c_header_smoke() == 0);
  co_checker_online_options_v1 options{};
  co_checker_online_options_init_v1(&options);
  CHECK(options.struct_size == sizeof(options));
  CHECK(options.abi_version == CO_CHECKER_ONLINE_ABI_VERSION_V1);
  CHECK(options.chunk_segment_bytes == 4096U);
  CHECK(options.reorder_capacity_bytes == 0U);
  CHECK(options.ordering_protocol ==
        CO_CHECKER_ONLINE_ORDERING_SERIALIZED_GLOBAL_LOCK_V1);
}

void test_runtime_generated_logical_ids_are_unique() {
  auto owner                             = create_runtime(test_options());
  const auto session                     = new_session(owner.runtime);
  constexpr std::size_t allocation_count = 10'000;
  std::set<std::pair<std::uint64_t, std::uint64_t>> identities;
  for (std::size_t index = 0; index < allocation_count; ++index) {
    co_checker_online_uid_v1 transaction{};
    CHECK(co_checker_online_logical_transaction_create_v1(
              owner.runtime, &session, static_cast<std::uint64_t>(index + 1U),
              &transaction) == CO_CHECKER_ONLINE_STATUS_OK_V1);
    CHECK(identities.emplace(transaction.word0, transaction.word1).second);
  }
  CHECK(identities.size() == allocation_count);
  CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
}

void test_full_lifecycle_retry_ownership_and_clean_close() {
  auto owner         = create_runtime(test_options());
  auto *runtime      = owner.runtime;
  const auto session = new_session(runtime);
  co_checker_online_epoch_v1 epoch{};
  CHECK(co_checker_online_runtime_epoch_v1(runtime, &epoch) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(epoch.word0 == session.runtime_epoch.word0);
  CHECK(epoch.word1 == session.runtime_epoch.word1);

  const auto first = register_attempt(runtime, session, 1, 10);
  auto write = flat_input(10, 20, false, {write_event(1, "key", "value")});
  CHECK(co_checker_online_attempt_publish_v1(runtime, &first.attempt,
                                             write.data(), write.size()) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  std::fill(write.begin(), write.end(), std::byte{0xff});

  co_checker_online_uid_v1 second_txn{};
  CHECK(co_checker_online_logical_transaction_create_v1(runtime, &session, 2,
                                                        &second_txn) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  co_checker_online_uid_v1 aborted{};
  CHECK(co_checker_online_attempt_register_v1(runtime, &session, &second_txn, 2,
                                              30, &aborted) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(co_checker_online_attempt_abort_v1(runtime, &aborted) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  co_checker_online_uid_v1 retry{};
  CHECK(co_checker_online_attempt_register_v1(runtime, &session, &second_txn, 2,
                                              30, &retry) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  auto read = flat_input(
      30, 0, true, {read_event(1, "key", std::optional<std::string>{"value"})});
  CHECK(co_checker_online_attempt_publish_v1(runtime, &retry, read.data(),
                                             read.size()) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  std::fill(read.begin(), read.end(), std::byte{0xee});
  advance_frontiers(runtime, 30);

  CHECK(co_checker_online_quiescent_close_v1(runtime) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  co_checker_online_verdict_v1 verdict{};
  CHECK(co_checker_online_verdict_get_v1(runtime, &verdict) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(verdict.status == CO_CHECKER_ONLINE_VERDICT_PASS_V1);
  std::string error_message;
  CHECK(get_error(runtime, error_message).present == 0U);
  const auto metrics = metrics_json(runtime);
  CHECK(metrics.find("\"captured_transactions\":2") != std::string::npos);
  CHECK(metrics.find("\"checked_transactions\":2") != std::string::npos);
  CHECK(metrics.find("\"capture_active_bytes\":0") != std::string::npos);
  CHECK(metrics.find("\"chunk_recycles\":") != std::string::npos);
  CHECK(metrics.find("\"native_parse_ns\":") != std::string::npos);
  CHECK(metrics.find("\"native_reserve_ns\":") != std::string::npos);
  CHECK(metrics.find("\"producer_mutex_wait_ns\":") != std::string::npos);
  CHECK(metrics.find("\"native_build_ns\":") != std::string::npos);
  CHECK(metrics.find("\"native_handoff_ns\":") != std::string::npos);
  CHECK(metrics.find("\"ordering_ns\":") != std::string::npos);
  CHECK(metrics.find("\"checking_ns\":") != std::string::npos);
  CHECK(metrics.find("\"max_checking_ns\":") != std::string::npos);
}

void test_live_frontiers_check_before_quiescent_close() {
  auto owner         = create_runtime(test_options());
  const auto session = new_session(owner.runtime);
  const auto attempt = register_attempt(owner.runtime, session, 1, 10);
  auto input = flat_input(10, 20, false, {write_event(1, "key", "value")});
  CHECK(co_checker_online_attempt_publish_v1(owner.runtime, &attempt.attempt,
                                             input.data(), input.size()) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);

  const auto before = metrics_json(owner.runtime);
  CHECK(before.find("\"captured_transactions\":1") != std::string::npos);
  CHECK(before.find("\"checked_transactions\":0") != std::string::npos);
  CHECK(co_checker_online_database_frontier_v1(owner.runtime, 20) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(co_checker_online_producer_frontier_v1(owner.runtime, 1, 20) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);

  const auto live = metrics_json(owner.runtime);
  CHECK(live.find("\"ordered_transactions\":1") != std::string::npos);
  CHECK(live.find("\"checked_transactions\":1") != std::string::npos);
  CHECK(live.find("\"checked_events\":1") != std::string::npos);
  CHECK(live.find("\"closed\":false") != std::string::npos);

  CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  co_checker_online_verdict_v1 verdict{};
  CHECK(co_checker_online_verdict_get_v1(owner.runtime, &verdict) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(verdict.status == CO_CHECKER_ONLINE_VERDICT_PASS_V1);
}

void test_violation_getter() {
  auto owner         = create_runtime(test_options());
  const auto session = new_session(owner.runtime);
  const auto attempt = register_attempt(owner.runtime, session, 1, 10);
  auto input         = flat_input(
      10, 0, true,
      {read_event(1, "missing", std::optional<std::string>{"ghost"})});
  CHECK(co_checker_online_attempt_publish_v1(owner.runtime, &attempt.attempt,
                                             input.data(), input.size()) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  advance_frontiers(owner.runtime, 10);
  CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  co_checker_online_verdict_v1 verdict{};
  CHECK(co_checker_online_verdict_get_v1(owner.runtime, &verdict) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(verdict.status == CO_CHECKER_ONLINE_VERDICT_VIOLATION_V1);
  CHECK(verdict.violation_kind ==
        CO_CHECKER_ONLINE_VIOLATION_NO_COMPATIBLE_SNAPSHOT_V1);
  CHECK(verdict.transaction_id.word0 == attempt.transaction.word0);
  CHECK(verdict.transaction_id.word1 == attempt.transaction.word1);
}

void test_modes_capture_and_order_never_expose_pass() {
  for (const auto mode : {CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1,
                          CO_CHECKER_ONLINE_MODE_ORDER_ONLY_V1}) {
    auto owner         = create_runtime(test_options(mode));
    const auto session = new_session(owner.runtime);
    const auto attempt = register_attempt(owner.runtime, session, 1, 10);
    auto input = flat_input(10, 20, false, {write_event(1, "key", "value")});
    CHECK(co_checker_online_attempt_publish_v1(owner.runtime, &attempt.attempt,
                                               input.data(), input.size()) ==
          CO_CHECKER_ONLINE_STATUS_OK_V1);
    if (mode != CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1) {
      advance_frontiers(owner.runtime, 20);
    }
    CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
          CO_CHECKER_ONLINE_STATUS_OK_V1);
    co_checker_online_verdict_v1 verdict{};
    CHECK(co_checker_online_verdict_get_v1(owner.runtime, &verdict) ==
          CO_CHECKER_ONLINE_STATUS_OK_V1);
    CHECK(verdict.status == CO_CHECKER_ONLINE_VERDICT_UNAVAILABLE_V1);
    const auto metrics = metrics_json(owner.runtime);
    CHECK(metrics.find("\"captured_transactions\":1") != std::string::npos);
    if (mode == CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1) {
      CHECK(metrics.find("\"ordered_transactions\":0") != std::string::npos);
    } else {
      CHECK(metrics.find("\"ordered_transactions\":1") != std::string::npos);
      CHECK(metrics.find("\"checked_transactions\":0") != std::string::npos);
    }
  }
}

void test_malformed_input_is_sticky_and_precludes_pass() {
  auto owner         = create_runtime(test_options());
  const auto session = new_session(owner.runtime);
  const auto attempt = register_attempt(owner.runtime, session, 1, 10);
  auto input = flat_input(10, 0, true, {read_event(1, "key", std::nullopt)});
  co_checker_online_event_input_v1 malformed{};
  std::memcpy(&malformed,
              input.data() + sizeof(co_checker_online_txn_input_header_v1),
              sizeof(malformed));
  malformed.key_offset = 1U;
  std::memcpy(input.data() + sizeof(co_checker_online_txn_input_header_v1),
              &malformed, sizeof(malformed));
  CHECK(co_checker_online_attempt_publish_v1(owner.runtime, &attempt.attempt,
                                             input.data(), input.size()) ==
        CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1);

  std::string message;
  const auto error = get_error(owner.runtime, message);
  CHECK(error.present == 1U);
  CHECK(error.adapter_error ==
        CO_CHECKER_ONLINE_ADAPTER_ERROR_MALFORMED_INPUT_V1);
  CHECK(!message.empty());
  co_checker_online_verdict_v1 verdict{};
  CHECK(co_checker_online_verdict_get_v1(owner.runtime, &verdict) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(verdict.status == CO_CHECKER_ONLINE_VERDICT_UNAVAILABLE_V1);
  CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
        CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1);
}

void test_capture_flat_layout_conformance_across_chunk_boundaries() {
  auto owner =
      create_runtime(test_options(CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1));
  const auto session                      = new_session(owner.runtime);
  constexpr std::size_t transaction_count = 96;
  for (std::size_t index = 0; index < transaction_count; ++index) {
    const auto session_seq = static_cast<std::uint64_t>(index + 1U);
    const auto start_ts    = 100U + session_seq * 2U;
    const auto attempt =
        register_attempt(owner.runtime, session, session_seq, start_ts);
    const auto key_size   = (index * 29U) % 131U;
    const auto value_size = (index * 47U) % 137U;
    std::vector<InputEvent> events;
    events.push_back(read_event(1, std::string(key_size, 'k'),
                                std::string(value_size, 'r'),
                                CO_CHECKER_ONLINE_READ_LOCAL_V1));
    events.push_back(write_event(3, std::string(key_size, 'w'),
                                 std::string(value_size, 'v')));
    events.push_back(read_event(7, "empty", std::string{}));
    auto input = flat_input(start_ts, start_ts + 1U, false, events);
    CHECK(co_checker_online_attempt_publish_v1(owner.runtime, &attempt.attempt,
                                               input.data(), input.size()) ==
          CO_CHECKER_ONLINE_STATUS_OK_V1);
  }
  CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  const auto metrics = metrics_json(owner.runtime);
  CHECK(metrics.find("\"captured_transactions\":96") != std::string::npos);
  CHECK(metrics.find("\"capture_active_bytes\":0") != std::string::npos);
}

void test_same_producer_concurrent_publish_assigns_lossless_sequences() {
  auto owner                              = create_runtime(test_options());
  constexpr std::size_t thread_count      = 4;
  constexpr std::size_t jobs_per_thread   = 32;
  constexpr std::size_t transaction_count = thread_count * jobs_per_thread;
  struct Job {
    co_checker_online_uid_v1 attempt{};
    std::vector<std::byte> input;
  };
  std::vector<Job> jobs;
  jobs.reserve(transaction_count);
  for (std::size_t index = 0; index < transaction_count; ++index) {
    const auto session  = new_session(owner.runtime);
    const auto start_ts = static_cast<std::uint64_t>(1000U + index * 2U);
    const auto attempt  = register_attempt(owner.runtime, session, 1, start_ts);
    jobs.push_back(Job{
        attempt.attempt,
        flat_input(start_ts, start_ts + 1U, false,
                   {write_event(1, "key", std::to_string(index))}),
    });
  }

  std::atomic<std::size_t> failures{0};
  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (std::size_t thread_index = 0; thread_index < thread_count;
       ++thread_index) {
    threads.emplace_back([&, thread_index] {
      const auto begin = thread_index * jobs_per_thread;
      const auto end   = begin + jobs_per_thread;
      for (std::size_t index = begin; index < end; ++index) {
        if (co_checker_online_attempt_publish_v1(
                owner.runtime, &jobs[index].attempt, jobs[index].input.data(),
                jobs[index].input.size()) != CO_CHECKER_ONLINE_STATUS_OK_V1) {
          failures.fetch_add(1U, std::memory_order_relaxed);
          return;
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  CHECK(failures.load(std::memory_order_relaxed) == 0U);
  advance_frontiers(owner.runtime, 10'000);
  CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  const auto metrics = metrics_json(owner.runtime);
  CHECK(metrics.find("\"checked_transactions\":128") != std::string::npos);
  CHECK(metrics.find("\"fatal\":false") != std::string::npos);
}

void test_active_attempt_makes_quiescent_close_fail_closed() {
  auto owner         = create_runtime(test_options());
  const auto session = new_session(owner.runtime);
  (void)register_attempt(owner.runtime, session, 1, 10);
  CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
        CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1);
  co_checker_online_verdict_v1 verdict{};
  CHECK(co_checker_online_verdict_get_v1(owner.runtime, &verdict) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(verdict.status == CO_CHECKER_ONLINE_VERDICT_UNAVAILABLE_V1);
  std::string message;
  const auto error = get_error(owner.runtime, message);
  CHECK(error.present == 1U);
  CHECK(error.adapter_error == CO_CHECKER_ONLINE_ADAPTER_ERROR_LIFECYCLE_V1);
}

void test_write_path_without_a_normalized_write_uses_commit_token() {
  auto owner         = create_runtime(test_options());
  const auto session = new_session(owner.runtime);
  const auto attempt = register_attempt(owner.runtime, session, 1, 10);
  auto input         = flat_input(10, 20, false, {});
  CHECK(co_checker_online_attempt_publish_v1(owner.runtime, &attempt.attempt,
                                             input.data(), input.size()) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  advance_frontiers(owner.runtime, 20);
  CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  co_checker_online_verdict_v1 verdict{};
  CHECK(co_checker_online_verdict_get_v1(owner.runtime, &verdict) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(verdict.status == CO_CHECKER_ONLINE_VERDICT_PASS_V1);
  const auto metrics = metrics_json(owner.runtime);
  CHECK(metrics.find("\"checked_transactions\":1") != std::string::npos);
}

void test_stalled_capture_budget_plateaus_then_fails_closed() {
  auto options = test_options();
  constexpr std::uint64_t one_batch_reserved =
      sizeof(BatchHeaderV1) + 64U + 64U;
  options.capture_capacity_bytes = one_batch_reserved;
  options.capture_timeout_ns     = 30'000'000ULL;
  auto owner                     = create_runtime(options);
  const auto first_session       = new_session(owner.runtime);
  const auto second_session      = new_session(owner.runtime);
  const auto first = register_attempt(owner.runtime, first_session, 1, 10);
  auto first_input = flat_input(10, 20, false, {write_event(1, "k", "v")});
  CHECK(co_checker_online_attempt_publish_v1(
            owner.runtime, &first.attempt, first_input.data(),
            first_input.size()) == CO_CHECKER_ONLINE_STATUS_OK_V1);

  const auto second  = register_attempt(owner.runtime, second_session, 1, 30);
  auto second_input  = flat_input(30, 40, false, {write_event(1, "x", "y")});
  const auto started = std::chrono::steady_clock::now();
  CHECK(co_checker_online_attempt_publish_v1(
            owner.runtime, &second.attempt, second_input.data(),
            second_input.size()) == CO_CHECKER_ONLINE_STATUS_RESOURCE_ERROR_V1);
  CHECK(std::chrono::steady_clock::now() - started >= 20ms);
  const auto metrics = metrics_json(owner.runtime);
  CHECK(metrics.find("\"capture_peak_bytes\":296") != std::string::npos);
  CHECK(metrics.find("\"capture_active_bytes\":296") != std::string::npos);
  CHECK(metrics.find("\"capture_backpressure_wait_count\":1") !=
        std::string::npos);
  co_checker_online_verdict_v1 verdict{};
  CHECK(co_checker_online_verdict_get_v1(owner.runtime, &verdict) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(verdict.status == CO_CHECKER_ONLINE_VERDICT_UNAVAILABLE_V1);
}

void test_external_fatal_is_sticky() {
  auto owner                         = create_runtime(test_options());
  constexpr std::string_view message = "instrumentation lost an event";
  CHECK(co_checker_online_fatal_v1(owner.runtime, 77, message.data(),
                                   message.size()) ==
        CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1);
  std::string actual;
  const auto error = get_error(owner.runtime, actual);
  CHECK(error.present == 1U);
  CHECK(error.adapter_error == CO_CHECKER_ONLINE_ADAPTER_ERROR_EXTERNAL_V1);
  CHECK(error.external_error == 77U);
  CHECK(actual == message);
  CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
        CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1);
}

void test_retained_state_limit_is_a_fatal_engine_error() {
  auto options                          = test_options();
  options.retained_state_capacity_bytes = 1U;
  auto owner                            = create_runtime(options);
  const auto session                    = new_session(owner.runtime);
  const auto attempt = register_attempt(owner.runtime, session, 1, 10);
  auto input = flat_input(10, 20, false, {write_event(1, "key", "value")});
  CHECK(co_checker_online_attempt_publish_v1(owner.runtime, &attempt.attempt,
                                             input.data(), input.size()) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(co_checker_online_database_frontier_v1(owner.runtime, 20) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(co_checker_online_producer_frontier_v1(owner.runtime, 1, 20) ==
        CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1);
  CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
        CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1);
  co_checker_online_verdict_v1 verdict{};
  CHECK(co_checker_online_verdict_get_v1(owner.runtime, &verdict) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(verdict.status == CO_CHECKER_ONLINE_VERDICT_UNAVAILABLE_V1);
  std::string message;
  const auto error = get_error(owner.runtime, message);
  CHECK(error.present == 1U);
  CHECK(error.runtime_error != 0U);
  CHECK(error.source_error != 0U);
}


void test_serialized_full_prepare_finalize_and_completion() {
  auto owner = create_runtime(test_options(
      CO_CHECKER_ONLINE_MODE_FULL_V1,
      CO_CHECKER_ONLINE_ORDERING_SERIALIZED_GLOBAL_LOCK_V1));
  const auto session = new_session(owner.runtime);

  const auto writer = register_attempt(owner.runtime, session, 1, 10);
  auto write =
      flat_input(10, 0, false, {write_event(1, "key", "value")});
  CHECK(co_checker_online_attempt_prepare_v1(
            owner.runtime, &writer.attempt, write.data(), write.size()) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(metrics_json(owner.runtime).find("\"queue_reserved_depth\":1") !=
        std::string::npos);
  CHECK(co_checker_online_attempt_finalize_v1(
            owner.runtime, &writer.attempt,
            CO_CHECKER_ONLINE_ORDER_TOKEN_WRITE_COMMIT_V1, 20, 1) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);

  const auto reader = register_attempt(owner.runtime, session, 2, 30);
  auto read = flat_input(
      30, 0, true,
      {read_event(1, "key", std::optional<std::string>{"value"})});
  CHECK(co_checker_online_attempt_prepare_v1(
            owner.runtime, &reader.attempt, read.data(), read.size()) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(co_checker_online_attempt_finalize_v1(
            owner.runtime, &reader.attempt,
            CO_CHECKER_ONLINE_ORDER_TOKEN_READ_ONLY_COMPLETION_V1, 40, 2) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);

  CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  co_checker_online_verdict_v1 verdict{};
  CHECK(co_checker_online_verdict_get_v1(owner.runtime, &verdict) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(verdict.status == CO_CHECKER_ONLINE_VERDICT_PASS_V1);
  const auto metrics = metrics_json(owner.runtime);
  CHECK(metrics.find("\"ordering_protocol\":1") != std::string::npos);
  CHECK(metrics.find("\"successful_write_commits\":1") !=
        std::string::npos);
  CHECK(metrics.find("\"completion_transactions\":1") !=
        std::string::npos);
  CHECK(metrics.find("\"accepted_publications\":2") != std::string::npos);
  CHECK(metrics.find("\"accepted_events\":2") != std::string::npos);
  CHECK(metrics.find("\"queue_peak_depth\":") != std::string::npos);
  CHECK(metrics.find("\"total_queue_lag_ns\":") !=
        std::string::npos);
  CHECK(metrics.find("\"ordered_transactions\":2") != std::string::npos);
  CHECK(metrics.find("\"checked_transactions\":2") != std::string::npos);
  CHECK(metrics.find("\"queue_reserved_depth\":0") != std::string::npos);
}

void test_serialized_mode_matrix_and_no_write_completion() {
  for (const auto mode : {CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1,
                          CO_CHECKER_ONLINE_MODE_ORDER_ONLY_V1,
                          CO_CHECKER_ONLINE_MODE_FULL_V1}) {
    auto owner = create_runtime(test_options(
        mode, CO_CHECKER_ONLINE_ORDERING_SERIALIZED_GLOBAL_LOCK_V1));
    const auto session = new_session(owner.runtime);
    const auto attempt = register_attempt(owner.runtime, session, 1, 10);
    auto input = flat_input(10, 0, false, {});
    CHECK(co_checker_online_attempt_prepare_v1(
              owner.runtime, &attempt.attempt, input.data(), input.size()) ==
          CO_CHECKER_ONLINE_STATUS_OK_V1);
    CHECK(co_checker_online_attempt_finalize_v1(
              owner.runtime, &attempt.attempt,
              CO_CHECKER_ONLINE_ORDER_TOKEN_NO_WRITE_COMPLETION_V1, 20, 1) ==
          CO_CHECKER_ONLINE_STATUS_OK_V1);
    CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
          CO_CHECKER_ONLINE_STATUS_OK_V1);

    co_checker_online_verdict_v1 verdict{};
    CHECK(co_checker_online_verdict_get_v1(owner.runtime, &verdict) ==
          CO_CHECKER_ONLINE_STATUS_OK_V1);
    CHECK(verdict.status ==
          (mode == CO_CHECKER_ONLINE_MODE_FULL_V1
               ? CO_CHECKER_ONLINE_VERDICT_PASS_V1
               : CO_CHECKER_ONLINE_VERDICT_UNAVAILABLE_V1));
    const auto metrics = metrics_json(owner.runtime);
    CHECK(metrics.find("\"accepted_publications\":1") !=
          std::string::npos);
    if (mode == CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1) {
      CHECK(metrics.find("\"recycled_transactions\":1") !=
            std::string::npos);
      CHECK(metrics.find("\"ordered_transactions\":0") !=
            std::string::npos);
    }
  }
}

void test_serialized_prepared_abort_releases_reservations() {
  auto owner = create_runtime(test_options(
      CO_CHECKER_ONLINE_MODE_FULL_V1,
      CO_CHECKER_ONLINE_ORDERING_SERIALIZED_GLOBAL_LOCK_V1));
  const auto session = new_session(owner.runtime);
  const auto attempt = register_attempt(owner.runtime, session, 1, 10);
  auto input =
      flat_input(10, 0, false, {write_event(1, "key", "value")});
  CHECK(co_checker_online_attempt_prepare_v1(
            owner.runtime, &attempt.attempt, input.data(), input.size()) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  CHECK(metrics_json(owner.runtime).find("\"queue_reserved_depth\":1") !=
        std::string::npos);
  CHECK(co_checker_online_attempt_abort_v1(owner.runtime, &attempt.attempt) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
  const auto metrics = metrics_json(owner.runtime);
  CHECK(metrics.find("\"accepted_events\":0") != std::string::npos);
  CHECK(metrics.find("\"queue_reserved_depth\":0") != std::string::npos);
  CHECK(metrics.find("\"capture_active_bytes\":0") != std::string::npos);
  CHECK(metrics.find("\"definitive_aborts\":1") != std::string::npos);
  CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
        CO_CHECKER_ONLINE_STATUS_OK_V1);
}

void test_serialized_sequence_and_wrong_protocol_fail_closed() {
  {
    auto owner = create_runtime(test_options(
        CO_CHECKER_ONLINE_MODE_FULL_V1,
        CO_CHECKER_ONLINE_ORDERING_SERIALIZED_GLOBAL_LOCK_V1));
    const auto session = new_session(owner.runtime);
    const auto attempt = register_attempt(owner.runtime, session, 1, 10);
    auto input =
        flat_input(10, 0, false, {write_event(1, "key", "value")});
    CHECK(co_checker_online_attempt_prepare_v1(
              owner.runtime, &attempt.attempt, input.data(), input.size()) ==
          CO_CHECKER_ONLINE_STATUS_OK_V1);
    CHECK(co_checker_online_attempt_finalize_v1(
              owner.runtime, &attempt.attempt,
              CO_CHECKER_ONLINE_ORDER_TOKEN_WRITE_COMMIT_V1, 20, 2) ==
          CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1);
    CHECK(co_checker_online_quiescent_close_v1(owner.runtime) ==
          CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1);
  }
  {
    auto owner = create_runtime(test_options(
        CO_CHECKER_ONLINE_MODE_FULL_V1,
        CO_CHECKER_ONLINE_ORDERING_SERIALIZED_GLOBAL_LOCK_V1));
    const auto session = new_session(owner.runtime);
    const auto attempt = register_attempt(owner.runtime, session, 1, 10);
    auto input =
        flat_input(10, 20, false, {write_event(1, "key", "value")});
    CHECK(co_checker_online_attempt_publish_v1(
              owner.runtime, &attempt.attempt, input.data(), input.size()) ==
          CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1);
  }
  {
    auto owner = create_runtime(test_options());
    const auto session = new_session(owner.runtime);
    const auto attempt = register_attempt(owner.runtime, session, 1, 10);
    auto input =
        flat_input(10, 0, false, {write_event(1, "key", "value")});
    CHECK(co_checker_online_attempt_prepare_v1(
              owner.runtime, &attempt.attempt, input.data(), input.size()) ==
          CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1);
  }
}

void test_serialized_queue_backpressure_occurs_during_prepare() {
  auto options = test_options(
      CO_CHECKER_ONLINE_MODE_FULL_V1,
      CO_CHECKER_ONLINE_ORDERING_SERIALIZED_GLOBAL_LOCK_V1);
  options.queue_capacity_bytes = sizeof(BatchHeaderV1);
  options.queue_timeout_ns     = 30'000'000ULL;
  auto owner                   = create_runtime(options);
  const auto first_session     = new_session(owner.runtime);
  const auto second_session    = new_session(owner.runtime);
  const auto first =
      register_attempt(owner.runtime, first_session, 1, 10);
  const auto second =
      register_attempt(owner.runtime, second_session, 1, 20);
  auto first_input  = flat_input(10, 0, false, {});
  auto second_input = flat_input(20, 0, false, {});

  CHECK(co_checker_online_attempt_prepare_v1(
            owner.runtime, &first.attempt, first_input.data(),
            first_input.size()) == CO_CHECKER_ONLINE_STATUS_OK_V1);
  const auto started = std::chrono::steady_clock::now();
  CHECK(co_checker_online_attempt_prepare_v1(
            owner.runtime, &second.attempt, second_input.data(),
            second_input.size()) == CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1);
  CHECK(std::chrono::steady_clock::now() - started >= 20ms);
  const auto metrics = metrics_json(owner.runtime);
  CHECK(metrics.find("\"queue_reserved_depth\":1") != std::string::npos);
  CHECK(metrics.find("\"queue_backpressure_wait_count\":1") !=
        std::string::npos);
}

} // namespace

int run_c_api_tests() {
  try {
    test_c_header_and_fixed_layout();
    test_runtime_generated_logical_ids_are_unique();
    test_full_lifecycle_retry_ownership_and_clean_close();
    test_live_frontiers_check_before_quiescent_close();
    test_violation_getter();
    test_modes_capture_and_order_never_expose_pass();
    test_malformed_input_is_sticky_and_precludes_pass();
    test_capture_flat_layout_conformance_across_chunk_boundaries();
    test_same_producer_concurrent_publish_assigns_lossless_sequences();
    test_active_attempt_makes_quiescent_close_fail_closed();
    test_write_path_without_a_normalized_write_uses_commit_token();
    test_stalled_capture_budget_plateaus_then_fails_closed();
    test_external_fatal_is_sticky();
    test_retained_state_limit_is_a_fatal_engine_error();
    test_serialized_full_prepare_finalize_and_completion();
    test_serialized_mode_matrix_and_no_write_completion();
    test_serialized_prepared_abort_releases_reservations();
    test_serialized_sequence_and_wrong_protocol_fail_closed();
    test_serialized_queue_backpressure_occurs_during_prepare();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "C ABI tests failed: " << error.what() << '\n';
    return 1;
  }
}

#ifdef CO_CHECKER_C_API_STANDALONE_TEST_MAIN
int main() { return run_c_api_tests(); }
#endif
