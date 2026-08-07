#include "co_checker/online/packed_batch.h"
#include "co_checker/online/repeated_read_table.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif
namespace {

using namespace co_checker::online;

[[nodiscard]] const char *name(const PackedValidationMode mode) noexcept {
  switch (mode) {
  case PackedValidationMode::Scalar:
    return "scalar";
  case PackedValidationMode::SSE2:
    return "sse2";
  case PackedValidationMode::AVX2:
    return "avx2";
  case PackedValidationMode::NEON:
    return "neon";
  }
  return "unknown";
}

[[nodiscard]] const char *name(const RepeatedReadProbeMode mode) noexcept {
  switch (mode) {
  case RepeatedReadProbeMode::Scalar:
    return "scalar";
  case RepeatedReadProbeMode::SSE2:
    return "sse2";
  case RepeatedReadProbeMode::AVX2:
    return "avx2";
  case RepeatedReadProbeMode::NEON:
    return "neon";
  }
  return "unknown";
}

[[nodiscard]] std::size_t parse_size(const char *text) {
  char *end        = nullptr;
  const auto value = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0' ||
      value > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("invalid positive integer argument");
  }
  return static_cast<std::size_t>(value);
}

struct OwnedRead {
  std::array<std::byte, 16> key{};
  std::array<std::byte, 8> value{};
  EventView event;
};

void store_index(std::span<std::byte> target, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8; ++index) {
    target[index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned>(index * 8U)));
  }
}

[[nodiscard]] OrderedTxnMeta metadata() {
  OrderedTxnMeta result;
  result.transaction_id.words             = {1, 2};
  result.attempt_id.words                 = {3, 4};
  result.session_id.runtime_epoch.words   = {5, 6};
  result.session_id.producer_id           = 1;
  result.session_id.local_session_counter = 1;
  result.session_seq                      = 1;
  result.commit_token.cluster_epoch       = 1;
  result.commit_token.timestamp           = 2;
  result.producer_id                      = 1;
  result.producer_seq                     = 1;
  return result;
}

template <typename Work>
[[nodiscard]] double nanoseconds_per_operation(const std::size_t operations,
                                               Work &&work) {
  const auto start = std::chrono::steady_clock::now();
  work();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start);
  return static_cast<double>(elapsed.count()) / static_cast<double>(operations);
}

[[nodiscard]] std::uint64_t constant_benchmark_hash(ByteView) noexcept {
  return 7U;
}

[[nodiscard]] std::size_t current_rss_bytes() noexcept {
#if defined(__linux__)
  std::ifstream statm("/proc/self/statm");
  std::size_t total_pages    = 0;
  std::size_t resident_pages = 0;
  if (!(statm >> total_pages >> resident_pages)) {
    return 0;
  }
  const auto page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return 0;
  }
  const auto page_bytes = static_cast<std::size_t>(page_size);
  if (resident_pages > std::numeric_limits<std::size_t>::max() / page_bytes) {
    return std::numeric_limits<std::size_t>::max();
  }
  return resident_pages * page_bytes;
#else
  return 0;
#endif
}

[[nodiscard]] std::size_t peak_rss_bytes() noexcept {
#if defined(__linux__)
  rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss <= 0) {
    return 0;
  }
  const auto peak_kib = static_cast<std::size_t>(usage.ru_maxrss);
  if (peak_kib > std::numeric_limits<std::size_t>::max() / 1024U) {
    return std::numeric_limits<std::size_t>::max();
  }
  return peak_kib * 1024U;
#else
  return 0;
#endif
}

int run_retained_capacity_benchmark(const std::size_t transactions,
                                    const std::size_t unique_keys,
                                    const bool adversarial_hash,
                                    const std::string_view mode_filter) {
  constexpr std::size_t kOutlierSnapshotReads = 2'400'331U;
  constexpr std::size_t kRetainedCapacity     = 4'194'304U;

  std::vector<OwnedRead> reads(unique_keys);
  for (std::size_t index = 0; index < unique_keys; ++index) {
    auto &read = reads[index];
    store_index(read.key, static_cast<std::uint64_t>(index));
    store_index(read.value, static_cast<std::uint64_t>(index * 0x9e3779b9U));
    read.event = EventView{
        static_cast<EventSeq>(index + 1U),
        EventOp::Read,
        ValueState::Present,
        ReadSource::Snapshot,
        ByteView(read.key),
        ByteView(read.value),
    };
  }

  std::cout
      << "component,mode,hash,unique_keys,transactions,"
         "profiling_enabled,total_ns_per_transaction,"
         "reset_ns_per_transaction,insert_ns_per_transaction,"
         "clear_ns_per_transaction,capacity,touched_capacity,shrinks,"
         "shrink_attempts,shrink_failures,max_shrink_rebuild_ns,clear_calls,"
         "clear_slot_visits,clear_control_bytes,occupied_slots_reset,"
         "current_rss_bytes,peak_rss_bytes,guard\n";

  std::uint64_t benchmark_guard = 0;
  for (const auto mode : supported_repeated_read_probe_modes()) {
    if (mode_filter != "all" && mode_filter != name(mode)) {
      continue;
    }
    RepeatedReadTable table(mode, adversarial_hash ? constant_benchmark_hash
                                                   : repeated_read_hash);
    table.reset(kOutlierSnapshotReads);
    if (table.metrics().capacity != kRetainedCapacity) {
      throw std::runtime_error(
          "retained-capacity benchmark did not reach 4,194,304 slots");
    }
    table.clear();

    const auto metrics_before = table.metrics();
    std::chrono::nanoseconds reset_elapsed{};
    std::chrono::nanoseconds clear_elapsed{};
    const auto total_started = std::chrono::steady_clock::now();
    for (std::size_t transaction = 0; transaction < transactions;
         ++transaction) {
      const auto reset_started = std::chrono::steady_clock::now();
      table.reset(unique_keys);
      reset_elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - reset_started);

      for (const auto &read : reads) {
        const auto inserted = table.insert(read.event);
        if (inserted.kind != RepeatedReadInsertKind::Inserted) {
          throw std::runtime_error(
              "retained-capacity benchmark key was not inserted");
        }
        benchmark_guard += inserted.first_sequence;
      }

      const auto clear_started = std::chrono::steady_clock::now();
      table.clear();
      clear_elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - clear_started);
    }
    const auto total_elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - total_started);
    const auto metrics_after  = table.metrics();
    const auto insert_elapsed = total_elapsed - reset_elapsed - clear_elapsed;
    const auto divisor        = static_cast<double>(transactions);
    std::cout << "repeated_read_retained," << name(mode) << ','
              << (adversarial_hash ? "constant" : "uniform") << ','
              << unique_keys << ',' << transactions << ','
              << (metrics_after.detailed_profiling_enabled ? 1 : 0) << ','
              << static_cast<double>(total_elapsed.count()) / divisor << ','
              << static_cast<double>(reset_elapsed.count()) / divisor << ','
              << static_cast<double>(insert_elapsed.count()) / divisor << ','
              << static_cast<double>(clear_elapsed.count()) / divisor << ','
              << metrics_after.capacity << ',' << metrics_after.touched_capacity
              << ',' << metrics_after.shrinks - metrics_before.shrinks << ','
              << metrics_after.shrink_attempts - metrics_before.shrink_attempts
              << ','
              << metrics_after.shrink_failures - metrics_before.shrink_failures
              << ',' << metrics_after.max_shrink_rebuild_ns << ','
              << metrics_after.clear_calls - metrics_before.clear_calls << ','
              << metrics_after.clear_slot_visits -
                     metrics_before.clear_slot_visits
              << ','
              << metrics_after.clear_control_bytes_filled -
                     metrics_before.clear_control_bytes_filled
              << ','
              << metrics_after.clear_occupied_slots_reset -
                     metrics_before.clear_occupied_slots_reset
              << ',' << current_rss_bytes() << ',' << peak_rss_bytes() << ','
              << benchmark_guard << '\n';
  }
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc > 1 && std::string_view(argv[1]) == "--retained-capacity") {
      const auto transactions =
          argc > 2 ? parse_size(argv[2]) : std::size_t{100'000U};
      const auto unique_keys =
          argc > 3 ? parse_size(argv[3]) : std::size_t{23U};
      const auto hash_name =
          argc > 4 ? std::string_view(argv[4]) : std::string_view{"uniform"};
      const auto mode_filter =
          argc > 5 ? std::string_view(argv[5]) : std::string_view{"all"};
      if (transactions == 0 ||
          unique_keys > std::numeric_limits<EventSeq>::max()) {
        throw std::invalid_argument(
            "retained benchmark transactions must be nonzero and unique keys "
            "must fit EventSeq");
      }
      if (hash_name != "uniform" && hash_name != "constant") {
        throw std::invalid_argument(
            "retained benchmark hash must be uniform or constant");
      }
      if (mode_filter != "all" && mode_filter != "scalar" &&
          mode_filter != "sse2" && mode_filter != "avx2" &&
          mode_filter != "neon") {
        throw std::invalid_argument(
            "retained benchmark mode must be all, scalar, sse2, avx2, or "
            "neon");
      }
      return run_retained_capacity_benchmark(
          transactions, unique_keys, hash_name == "constant", mode_filter);
    }
    std::size_t event_count = 100000;
    std::size_t iterations  = 20;
    if (argc > 1) {
      event_count = parse_size(argv[1]);
    }
    if (argc > 2) {
      iterations = parse_size(argv[2]);
    }
    if (event_count == 0 || iterations == 0 ||
        event_count > std::numeric_limits<EventSeq>::max()) {
      throw std::invalid_argument("event and iteration counts must be nonzero");
    }

    std::vector<OwnedRead> reads(event_count);
    VectorBatchBuilder builder(metadata());
    const auto reserve_status =
        builder.reserve(event_count, event_count * (16U + 8U));
    if (!reserve_status.ok()) {
      throw std::runtime_error("could not reserve validation benchmark batch");
    }
    for (std::size_t index = 0; index < event_count; ++index) {
      auto &read = reads[index];
      store_index(read.key, static_cast<std::uint64_t>(index));
      store_index(read.value, static_cast<std::uint64_t>(index * 0x9e3779b9U));
      read.event        = EventView{static_cast<EventSeq>(index + 1U),
                             EventOp::Read,
                             ValueState::Present,
                             ReadSource::Snapshot,
                             ByteView(read.key),
                             ByteView(read.value)};
      const auto status = builder.append_read(
          read.event.sequence, read.event.key, PresentBytes{read.event.value},
          ReadSource::Snapshot);
      if (!status.ok()) {
        throw std::runtime_error("could not build validation benchmark batch");
      }
    }
    auto sealed = builder.seal();
    if (!sealed.ok()) {
      throw std::runtime_error("could not seal validation benchmark batch");
    }

    std::uint64_t validation_guard = 0;
    std::cout << "component,mode,events,iterations,ns_per_event\n";
    for (const auto mode : supported_packed_validation_modes()) {
      const auto ns = nanoseconds_per_operation(event_count * iterations, [&] {
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
          const auto result =
              validate_packed_batch_with_mode(sealed.batch->input(), mode);
          const auto *view = std::get_if<ValidatedTxnView>(&result);
          if (view == nullptr) {
            throw std::runtime_error("valid benchmark batch was rejected");
          }
          validation_guard += view->event_count();
        }
      });
      std::cout << "packed_validation," << name(mode) << ',' << event_count
                << ',' << iterations << ',' << ns << '\n';
    }

    std::uint64_t read_guard = 0;
    for (const auto mode : supported_repeated_read_probe_modes()) {
      RepeatedReadTable table(mode);
      const auto ns = nanoseconds_per_operation(event_count * iterations, [&] {
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
          table.reset(event_count);
          for (const auto &read : reads) {
            const auto result = table.insert(read.event);
            if (result.kind != RepeatedReadInsertKind::Inserted) {
              throw std::runtime_error("unique benchmark key was not inserted");
            }
            read_guard += result.first_sequence;
          }
          table.clear();
        }
      });
      std::cout << "repeated_read_table," << name(mode) << ',' << event_count
                << ',' << iterations << ',' << ns << '\n';
    }
    if (validation_guard == 0 || read_guard == 0) {
      return 2;
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "microbenchmark failed: " << error.what() << '\n';
    return 1;
  }
}
