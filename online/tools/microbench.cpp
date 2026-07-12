#include "co_checker/online/packed_batch.h"
#include "co_checker/online/repeated_read_table.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

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

} // namespace

int main(int argc, char **argv) {
  try {
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
