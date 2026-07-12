#include "co_checker/online/c_api.h"
#include "co_checker/online/packed_batch.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct RuntimeOwner {
  co_checker_online_runtime_v1 *runtime = nullptr;
  ~RuntimeOwner() { co_checker_online_runtime_destroy_v1(runtime); }
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
  RuntimeOwner() = default;
};

struct Configuration {
  std::uint64_t transactions        = 20'000;
  std::uint64_t warmup_transactions = 1'000;
  std::uint64_t frontier_every      = 256;
  std::uint64_t repetitions         = 3;
  std::uint64_t plateau_batches     = 128;
  std::string mode                  = "all";
  bool run_plateau                  = true;
};

[[nodiscard]] std::uint64_t parse_unsigned(const std::string_view text,
                                           const std::string_view option) {
  std::size_t consumed = 0;
  const auto value     = std::stoull(std::string(text), &consumed);
  if (consumed != text.size()) {
    throw std::invalid_argument(std::string(option) + " is not an integer");
  }
  return value;
}

[[nodiscard]] Configuration parse_arguments(const int argc, char **argv) {
  Configuration result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    const auto take_value = [&](const std::string_view option) {
      if (index + 1 >= argc) {
        throw std::invalid_argument(std::string(option) + " needs a value");
      }
      ++index;
      return std::string_view(argv[index]);
    };
    if (argument == "--transactions") {
      result.transactions = parse_unsigned(take_value(argument), argument);
    } else if (argument == "--warmup") {
      result.warmup_transactions =
          parse_unsigned(take_value(argument), argument);
    } else if (argument == "--frontier-every") {
      result.frontier_every = parse_unsigned(take_value(argument), argument);
    } else if (argument == "--repetitions") {
      result.repetitions = parse_unsigned(take_value(argument), argument);
    } else if (argument == "--plateau-batches") {
      result.plateau_batches = parse_unsigned(take_value(argument), argument);
    } else if (argument == "--mode") {
      result.mode = take_value(argument);
    } else if (argument == "--skip-plateau") {
      result.run_plateau = false;
    } else if (argument == "--help") {
      std::cout << "usage: co_checker_online_c_api_bench [--transactions N] "
                   "[--warmup N] [--frontier-every N] [--repetitions N] "
                   "[--mode all|capture-only|order-only|full] "
                   "[--plateau-batches N] [--skip-plateau]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + std::string(argument));
    }
  }
  if (result.transactions == 0U || result.frontier_every == 0U ||
      result.repetitions == 0U || result.plateau_batches == 0U ||
      (result.mode != "all" && result.mode != "capture-only" &&
       result.mode != "order-only" && result.mode != "full")) {
    throw std::invalid_argument("benchmark counts/mode are invalid");
  }
  return result;
}

[[nodiscard]] std::string error_text(co_checker_online_runtime_v1 *runtime) {
  co_checker_online_error_info_v1 error{};
  std::uint64_t required = 0;
  const auto query =
      co_checker_online_error_get_v1(runtime, &error, nullptr, 0, &required);
  if (error.present == 0U) {
    return "no native error";
  }
  if (query != CO_CHECKER_ONLINE_STATUS_BUFFER_TOO_SMALL_V1) {
    return "native error retrieval failed";
  }
  std::vector<char> text(static_cast<std::size_t>(required));
  if (co_checker_online_error_get_v1(runtime, &error, text.data(), required,
                                     &required) !=
      CO_CHECKER_ONLINE_STATUS_OK_V1) {
    return "native error copy failed";
  }
  return std::string(text.data());
}

void require_status(const co_checker_online_status_v1 status,
                    const co_checker_online_status_v1 expected,
                    co_checker_online_runtime_v1 *runtime,
                    const std::string_view operation) {
  if (status == expected) {
    return;
  }
  throw std::runtime_error(std::string(operation) + " returned status " +
                           std::to_string(status) + ": " + error_text(runtime));
}

[[nodiscard]] std::string metrics_json(co_checker_online_runtime_v1 *runtime) {
  std::uint64_t required = 0;
  require_status(
      co_checker_online_metrics_json_v1(runtime, nullptr, 0, &required),
      CO_CHECKER_ONLINE_STATUS_BUFFER_TOO_SMALL_V1, runtime,
      "metrics size query");
  std::vector<char> json(static_cast<std::size_t>(required));
  require_status(co_checker_online_metrics_json_v1(runtime, json.data(),
                                                   required, &required),
                 CO_CHECKER_ONLINE_STATUS_OK_V1, runtime, "metrics copy");
  return std::string(json.data());
}

void require_metric_value(const std::string_view json,
                          const std::string_view name,
                          const std::uint64_t expected) {
  const auto needle =
      '"' + std::string(name) + "\":" + std::to_string(expected);
  if (json.find(needle) == std::string_view::npos) {
    throw std::runtime_error("metric " + std::string(name) + " did not equal " +
                             std::to_string(expected));
  }
}

void require_json_fragment(const std::string_view json,
                           const std::string_view fragment) {
  if (json.find(fragment) == std::string_view::npos) {
    throw std::runtime_error("metrics JSON is missing " +
                             std::string(fragment));
  }
}

[[nodiscard]] RuntimeOwner create_runtime(const std::uint32_t mode,
                                          const std::uint64_t capture_bytes,
                                          const std::uint64_t reorder_bytes,
                                          const std::uint64_t timeout_ns) {
  co_checker_online_options_v1 options{};
  co_checker_online_options_init_v1(&options);
  options.mode                          = mode;
  options.queue_capacity_bytes          = 16U * 1024U * 1024U;
  options.reorder_capacity_bytes        = reorder_bytes;
  options.capture_capacity_bytes        = capture_bytes;
  options.retained_state_capacity_bytes = 512U * 1024U * 1024U;
  options.queue_timeout_ns              = timeout_ns;
  options.capture_timeout_ns            = timeout_ns;
  options.chunk_segment_bytes           = 4U * 1024U;
  options.max_cached_chunk_segments     = 2'048U;
  options.cluster_epoch                 = 1U;
  constexpr std::uint32_t producer      = 1;
  RuntimeOwner result;
  require_status(co_checker_online_runtime_create_v1(&options, &producer, 1,
                                                     &result.runtime),
                 CO_CHECKER_ONLINE_STATUS_OK_V1, result.runtime,
                 "runtime create");
  return result;
}

[[nodiscard]] std::vector<std::byte>
pack_point_transaction(const std::uint64_t transaction_index,
                       const std::uint64_t start_ts,
                       const std::uint64_t commit_ts) {
  constexpr std::size_t key_size    = 16;
  constexpr std::size_t value_size  = 32;
  constexpr std::size_t event_count = 2;
  constexpr std::size_t descriptor_bytes =
      event_count * sizeof(co_checker_online_event_input_v1);
  constexpr std::size_t payload_bytes = key_size * 2U + value_size;
  std::vector<std::byte> buffer(sizeof(co_checker_online_txn_input_header_v1) +
                                descriptor_bytes + payload_bytes);

  co_checker_online_txn_input_header_v1 header{};
  co_checker_online_txn_input_header_init_v1(&header);
  header.total_size  = buffer.size();
  header.start_ts    = start_ts;
  header.commit_ts   = commit_ts;
  header.read_only   = 0;
  header.event_count = event_count;
  std::memcpy(buffer.data(), &header, sizeof(header));

  const auto payload_start = sizeof(header) + descriptor_bytes;
  std::array<std::byte, key_size> key{};
  std::array<std::byte, value_size> value{};
  std::memcpy(key.data(), &transaction_index, sizeof(transaction_index));
  const auto key_suffix = transaction_index ^ 0xd1b54a32d192ed03ULL;
  std::memcpy(key.data() + sizeof(transaction_index), &key_suffix,
              sizeof(key_suffix));
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::byte>(
        static_cast<unsigned char>((transaction_index + index * 17U) & 0xffU));
  }

  co_checker_online_event_input_v1 read{};
  read.sequence    = 1;
  read.operation   = CO_CHECKER_ONLINE_EVENT_READ_V1;
  read.value_state = CO_CHECKER_ONLINE_VALUE_ABSENT_V1;
  read.read_source = CO_CHECKER_ONLINE_READ_SNAPSHOT_V1;
  read.key_offset  = static_cast<std::uint32_t>(payload_start);
  read.key_size    = key_size;
  std::memcpy(buffer.data() + sizeof(header), &read, sizeof(read));

  co_checker_online_event_input_v1 write{};
  write.sequence    = 2;
  write.operation   = CO_CHECKER_ONLINE_EVENT_WRITE_V1;
  write.value_state = CO_CHECKER_ONLINE_VALUE_PRESENT_V1;
  write.read_source = 0;
  write.key_offset  = static_cast<std::uint32_t>(payload_start + key_size);
  write.key_size    = key_size;
  write.value_offset =
      static_cast<std::uint32_t>(payload_start + key_size * 2U);
  write.value_size = value_size;
  std::memcpy(buffer.data() + sizeof(header) + sizeof(read), &write,
              sizeof(write));
  std::memcpy(buffer.data() + payload_start, key.data(), key.size());
  std::memcpy(buffer.data() + payload_start + key_size, key.data(), key.size());
  std::memcpy(buffer.data() + payload_start + key_size * 2U, value.data(),
              value.size());
  return buffer;
}

struct Mode {
  const char *name;
  std::uint32_t value;
};

void run_pipeline(const Mode mode, const std::uint64_t transaction_count,
                  const std::uint64_t frontier_every,
                  const std::uint64_t repetition, const bool report) {
  auto runtime =
      create_runtime(mode.value, 64U * 1024U * 1024U, 64U * 1024U * 1024U,
                     30U * 1000U * 1000U * 1000U);
  co_checker_online_session_v1 session{};
  require_status(
      co_checker_online_session_create_v1(runtime.runtime, 1, &session),
      CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime, "session create");

  std::chrono::nanoseconds identity_register_time{};
  std::chrono::nanoseconds pack_time{};
  std::chrono::nanoseconds publish_time{};
  std::chrono::nanoseconds frontier_time{};
  const auto run_started       = Clock::now();
  std::uint64_t last_commit_ts = 0;
  for (std::uint64_t index = 0; index < transaction_count; ++index) {
    const auto start_ts         = index * 2U + 1U;
    const auto commit_ts        = start_ts + 1U;
    const auto identity_started = Clock::now();
    co_checker_online_uid_v1 transaction{};
    require_status(co_checker_online_logical_transaction_create_v1(
                       runtime.runtime, &session, index + 1U, &transaction),
                   CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
                   "logical transaction create");
    co_checker_online_uid_v1 attempt{};
    require_status(co_checker_online_attempt_register_v1(
                       runtime.runtime, &session, &transaction, index + 1U,
                       start_ts, &attempt),
                   CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
                   "attempt register");
    identity_register_time +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                             identity_started);

    const auto pack_started = Clock::now();
    auto input = pack_point_transaction(index, start_ts, commit_ts);
    pack_time += std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - pack_started);
    const auto publish_started = Clock::now();
    require_status(co_checker_online_attempt_publish_v1(
                       runtime.runtime, &attempt, input.data(), input.size()),
                   CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
                   "attempt publish");
    publish_time += std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - publish_started);
    last_commit_ts = commit_ts;

    if ((index + 1U) % frontier_every == 0U) {
      const auto frontier_started = Clock::now();
      require_status(
          co_checker_online_database_frontier_v1(runtime.runtime, commit_ts),
          CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime, "database frontier");
      require_status(
          co_checker_online_producer_frontier_v1(runtime.runtime, 1, commit_ts),
          CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime, "producer frontier");
      frontier_time += std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now() - frontier_started);
    }
  }
  if (transaction_count % frontier_every != 0U) {
    const auto frontier_started = Clock::now();
    require_status(
        co_checker_online_database_frontier_v1(runtime.runtime, last_commit_ts),
        CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
        "final database frontier");
    require_status(co_checker_online_producer_frontier_v1(runtime.runtime, 1,
                                                          last_commit_ts),
                   CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
                   "final producer frontier");
    frontier_time += std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - frontier_started);
  }
  const auto close_started = Clock::now();
  require_status(co_checker_online_quiescent_close_v1(runtime.runtime),
                 CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
                 "quiescent close");
  const auto close_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - close_started);
  const auto wall_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now() - run_started);

  co_checker_online_verdict_v1 verdict{};
  require_status(co_checker_online_verdict_get_v1(runtime.runtime, &verdict),
                 CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
                 "verdict get");
  const auto expected_verdict = mode.value == CO_CHECKER_ONLINE_MODE_FULL_V1
                                    ? CO_CHECKER_ONLINE_VERDICT_PASS_V1
                                    : CO_CHECKER_ONLINE_VERDICT_UNAVAILABLE_V1;
  if (verdict.status != expected_verdict) {
    throw std::runtime_error("unexpected benchmark verdict");
  }
  const auto metrics = metrics_json(runtime.runtime);
  const auto events  = transaction_count * 2U;
  require_metric_value(metrics, "active_attempts", 0U);
  require_metric_value(metrics, "captured_transactions", transaction_count);
  require_metric_value(metrics, "captured_events", events);
  require_metric_value(metrics, "queue_bytes", 0U);
  require_metric_value(metrics, "reorder_bytes", 0U);
  require_json_fragment(metrics, "\"fatal\":false");
  if (mode.value == CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1) {
    require_metric_value(metrics, "ordered_transactions", 0U);
    require_metric_value(metrics, "checked_transactions", 0U);
  } else {
    require_metric_value(metrics, "ordered_transactions", transaction_count);
    require_metric_value(
        metrics, "checked_transactions",
        mode.value == CO_CHECKER_ONLINE_MODE_FULL_V1 ? transaction_count : 0U);
  }
  require_metric_value(metrics, "checked_events",
                       mode.value == CO_CHECKER_ONLINE_MODE_FULL_V1 ? events
                                                                    : 0U);
  require_metric_value(metrics, "violating_transactions", 0U);
  if (!report) {
    return;
  }

  const auto seconds = std::chrono::duration<double>(wall_time).count();
  std::cout << std::fixed << std::setprecision(2) << "RESULT_JSON {\"mode\":\""
            << mode.name << "\",\"repetition\":" << repetition
            << ",\"transactions\":" << transaction_count
            << ",\"events\":" << events << ",\"wall_ns\":" << wall_time.count()
            << ",\"txn_per_s\":"
            << static_cast<double>(transaction_count) / seconds
            << ",\"events_per_s\":" << static_cast<double>(events) / seconds
            << ",\"identity_register_ns\":" << identity_register_time.count()
            << ",\"pack_ns\":" << pack_time.count()
            << ",\"publish_call_ns\":" << publish_time.count()
            << ",\"frontier_call_ns\":" << frontier_time.count()
            << ",\"close_ns\":" << close_time.count()
            << ",\"native_metrics\":" << metrics << "}\n";
}

void run_plateau(const std::uint64_t batch_count) {
  constexpr std::uint64_t chunk_size = 4U * 1024U;
  const auto reserved_per_batch =
      static_cast<std::uint64_t>(sizeof(co_checker::online::BatchHeaderV1)) +
      chunk_size * 2U;
  const auto capacity = reserved_per_batch * batch_count;
  auto runtime = create_runtime(CO_CHECKER_ONLINE_MODE_ORDER_ONLY_V1, capacity,
                                capacity, 50U * 1000U * 1000U);
  co_checker_online_session_v1 session{};
  require_status(
      co_checker_online_session_create_v1(runtime.runtime, 1, &session),
      CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
      "plateau session create");
  for (std::uint64_t index = 0; index < batch_count; ++index) {
    co_checker_online_uid_v1 transaction{};
    co_checker_online_uid_v1 attempt{};
    const auto start_ts = index * 2U + 1U;
    require_status(co_checker_online_logical_transaction_create_v1(
                       runtime.runtime, &session, index + 1U, &transaction),
                   CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
                   "plateau logical create");
    require_status(co_checker_online_attempt_register_v1(
                       runtime.runtime, &session, &transaction, index + 1U,
                       start_ts, &attempt),
                   CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
                   "plateau attempt register");
    auto input = pack_point_transaction(index, start_ts, start_ts + 1U);
    require_status(co_checker_online_attempt_publish_v1(
                       runtime.runtime, &attempt, input.data(), input.size()),
                   CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
                   "plateau publish");
  }

  const auto blocked_index = batch_count;
  const auto blocked_start = blocked_index * 2U + 1U;
  co_checker_online_uid_v1 blocked_transaction{};
  co_checker_online_uid_v1 blocked_attempt{};
  require_status(
      co_checker_online_logical_transaction_create_v1(
          runtime.runtime, &session, blocked_index + 1U, &blocked_transaction),
      CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
      "blocked logical create");
  require_status(co_checker_online_attempt_register_v1(
                     runtime.runtime, &session, &blocked_transaction,
                     blocked_index + 1U, blocked_start, &blocked_attempt),
                 CO_CHECKER_ONLINE_STATUS_OK_V1, runtime.runtime,
                 "blocked attempt register");
  auto blocked_input =
      pack_point_transaction(blocked_index, blocked_start, blocked_start + 1U);
  const auto blocked_started = Clock::now();
  const auto blocked_status  = co_checker_online_attempt_publish_v1(
      runtime.runtime, &blocked_attempt, blocked_input.data(),
      blocked_input.size());
  const auto blocked_time =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                           blocked_started);
  if (blocked_status != CO_CHECKER_ONLINE_STATUS_RESOURCE_ERROR_V1) {
    throw std::runtime_error("stalled capture did not fail at its hard limit");
  }
  const auto metrics = metrics_json(runtime.runtime);
  require_metric_value(metrics, "active_attempts", 1U);
  require_metric_value(metrics, "captured_transactions", batch_count);
  require_metric_value(metrics, "captured_events", batch_count * 2U);
  require_metric_value(metrics, "capture_active_bytes", capacity);
  require_metric_value(metrics, "capture_peak_bytes", capacity);
  require_metric_value(metrics, "capture_backpressure_wait_count", 1U);
  require_metric_value(metrics, "reorder_bytes", capacity);
  require_metric_value(metrics, "reorder_peak_bytes", capacity);
  require_metric_value(metrics, "ordered_transactions", 0U);
  require_metric_value(metrics, "violating_transactions", 0U);
  require_json_fragment(metrics, "\"fatal\":true");
  std::cout << "PLATEAU_JSON {\"batches_held\":" << batch_count
            << ",\"reserved_per_batch\":" << reserved_per_batch
            << ",\"capacity_bytes\":" << capacity
            << ",\"blocked_publish_ns\":" << blocked_time.count()
            << ",\"blocked_status\":" << blocked_status
            << ",\"native_metrics\":" << metrics << "}\n";
}

} // namespace

int main(const int argc, char **argv) {
  try {
    const auto configuration = parse_arguments(argc, argv);
    constexpr std::array<Mode, 3> modes{
        Mode{"capture-only", CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1},
        Mode{"order-only", CO_CHECKER_ONLINE_MODE_ORDER_ONLY_V1},
        Mode{"full", CO_CHECKER_ONLINE_MODE_FULL_V1},
    };
    std::cout << "CONFIG_JSON {\"transactions\":" << configuration.transactions
              << ",\"warmup\":" << configuration.warmup_transactions
              << ",\"frontier_every\":" << configuration.frontier_every
              << ",\"repetitions\":" << configuration.repetitions
              << ",\"plateau_batches\":" << configuration.plateau_batches
              << "}\n";
    for (const auto mode : modes) {
      if (configuration.mode != "all" && configuration.mode != mode.name) {
        continue;
      }
      if (configuration.warmup_transactions != 0U) {
        run_pipeline(mode, configuration.warmup_transactions,
                     configuration.frontier_every, 0, false);
      }
      for (std::uint64_t repetition = 1;
           repetition <= configuration.repetitions; ++repetition) {
        run_pipeline(mode, configuration.transactions,
                     configuration.frontier_every, repetition, true);
      }
    }
    if (configuration.run_plateau) {
      run_plateau(configuration.plateau_batches);
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "C ABI pipeline benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
