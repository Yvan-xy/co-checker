#include "co_checker/online/engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace co_checker::online;

constexpr std::array<char, 8> kTraceMagic{'C', 'O', 'T', 'R',
                                          'A', 'C', 'E', '1'};
constexpr std::uint32_t kTraceVersion = 1;
constexpr std::uint8_t kHasValue      = 1U << 0U;
constexpr std::uint8_t kDelete        = 1U << 1U;
constexpr std::uint8_t kMiss          = 1U << 2U;

class BinaryInput {
public:
  explicit BinaryInput(const std::string &path)
      : stream_(path, std::ios::binary) {}

  [[nodiscard]] bool is_open() const { return stream_.is_open(); }

  [[nodiscard]] bool read_bytes(void *destination, const std::size_t size) {
    stream_.read(static_cast<char *>(destination),
                 static_cast<std::streamsize>(size));
    return stream_.good() ||
           stream_.gcount() == static_cast<std::streamsize>(size);
  }

  [[nodiscard]] bool read_u8(std::uint8_t &value) {
    return read_bytes(&value, sizeof(value));
  }

  [[nodiscard]] bool read_u16(std::uint16_t &value) {
    std::array<std::uint8_t, 2> bytes{};
    if (!read_bytes(bytes.data(), bytes.size())) {
      return false;
    }
    value =
        static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
    return true;
  }

  [[nodiscard]] bool read_u32(std::uint32_t &value) {
    std::array<std::uint8_t, 4> bytes{};
    if (!read_bytes(bytes.data(), bytes.size())) {
      return false;
    }
    value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      value |= static_cast<std::uint32_t>(bytes[index])
               << static_cast<unsigned int>(index * 8U);
    }
    return true;
  }

  [[nodiscard]] bool read_u64(std::uint64_t &value) {
    std::array<std::uint8_t, 8> bytes{};
    if (!read_bytes(bytes.data(), bytes.size())) {
      return false;
    }
    value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      value |= static_cast<std::uint64_t>(bytes[index])
               << static_cast<unsigned int>(index * 8U);
    }
    return true;
  }

  [[nodiscard]] bool read_blob(OwnedBytes &bytes) {
    std::uint32_t size = 0;
    if (!read_u32(size)) {
      return false;
    }
    bytes.resize(size);
    return bytes.empty() || read_bytes(bytes.data(), bytes.size());
  }

private:
  std::ifstream stream_;
};

struct OwnedEvent {
  EventSeq sequence = 0;
  EventOp op        = EventOp::Read;
  ValueState state  = ValueState::Absent;
  OwnedBytes key;
  OwnedBytes value;

  [[nodiscard]] EventView view() const {
    return EventView{sequence,
                     op,
                     state,
                     ReadSource::Snapshot,
                     ByteView{key.data(), key.size()},
                     ByteView{value.data(), value.size()}};
  }
};

struct TraceHeader {
  std::uint64_t transaction_count = 0;
  std::uint64_t event_count       = 0;
};

[[nodiscard]] bool read_header(BinaryInput &input, TraceHeader &header,
                               std::string &error) {
  std::array<char, kTraceMagic.size()> magic{};
  std::uint32_t version = 0;
  std::uint32_t flags   = 0;
  if (!input.read_bytes(magic.data(), magic.size()) ||
      !input.read_u32(version) || !input.read_u32(flags) ||
      !input.read_u64(header.transaction_count) ||
      !input.read_u64(header.event_count)) {
    error = "truncated binary trace header";
    return false;
  }
  if (magic != kTraceMagic) {
    error = "invalid binary trace magic";
    return false;
  }
  if (version != kTraceVersion || flags != 0U) {
    error = "unsupported binary trace version or feature flags";
    return false;
  }
  return true;
}

[[nodiscard]] bool read_event(BinaryInput &input, OwnedEvent &event,
                              std::string &error) {
  std::uint8_t op        = 0;
  std::uint8_t flags     = 0;
  std::uint16_t reserved = 0;
  if (!input.read_u32(event.sequence) || !input.read_u8(op) ||
      !input.read_u8(flags) || !input.read_u16(reserved)) {
    error = "truncated event descriptor";
    return false;
  }
  if (reserved != 0U || (flags & ~(kHasValue | kDelete | kMiss)) != 0U) {
    error = "invalid event flags or reserved field";
    return false;
  }
  if (op > 1U) {
    error = "invalid event operation";
    return false;
  }
  event.op = op == 0U ? EventOp::Read : EventOp::Write;
  if (!input.read_blob(event.key)) {
    error = "truncated event key";
    return false;
  }

  const bool has_value = (flags & kHasValue) != 0U;
  const bool is_delete = (flags & kDelete) != 0U;
  const bool is_miss   = (flags & kMiss) != 0U;
  if (event.op == EventOp::Read) {
    if (is_delete || has_value == is_miss) {
      error = "read must carry exactly one of value or miss";
      return false;
    }
  } else if (is_miss || has_value == is_delete) {
    error = "write must carry exactly one of value or delete";
    return false;
  }

  event.state = has_value ? ValueState::Present : ValueState::Absent;
  if (has_value && !input.read_blob(event.value)) {
    error = "truncated event value";
    return false;
  }
  return true;
}

[[nodiscard]] SessionUid make_session(const std::uint32_t legacy_session) {
  return SessionUid{
      RuntimeEpoch{{0x74726163652d7631ULL, 0x6f6e6c696e652d31ULL}}, 1,
      legacy_session};
}

[[nodiscard]] OrderedTxnMeta
make_metadata(const CommitSeq commit_seq, const std::uint32_t legacy_session,
              const std::uint64_t session_seq,
              const std::uint64_t commit_timestamp) {
  OrderedTxnMeta meta;
  meta.commit_seq           = commit_seq;
  meta.transaction_id.words = {0, commit_seq + 1U};
  meta.attempt_id.words     = {1, commit_seq + 1U};
  meta.session_id           = make_session(legacy_session);
  meta.session_seq          = session_seq;
  meta.commit_token = CommitOrderToken{1, commit_timestamp, std::nullopt};
  meta.producer_id  = 1;
  meta.producer_seq = commit_seq + 1U;
  return meta;
}

struct Options {
  std::string path;
  std::optional<std::uint64_t> limit;
};

[[nodiscard]] std::optional<Options> parse_options(const int argc,
                                                   char **argv) {
  if (argc < 2) {
    return std::nullopt;
  }
  Options options;
  options.path = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--limit" && index + 1 < argc) {
      try {
        options.limit = std::stoull(argv[++index]);
      } catch (const std::exception &) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  }
  return options;
}

void print_usage(const char *program) {
  std::cerr << "usage: " << program << " TRACE.trace [--limit TXNS]\n";
}

} // namespace

int main(const int argc, char **argv) {
  const auto options = parse_options(argc, argv);
  if (!options.has_value()) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  BinaryInput input(options->path);
  if (!input.is_open()) {
    std::cerr << "input error: cannot open " << options->path << '\n';
    return 3;
  }
  TraceHeader header;
  std::string error;
  if (!read_header(input, header, error)) {
    std::cerr << "input error: " << error << '\n';
    return 3;
  }

  const auto transaction_limit =
      std::min(header.transaction_count,
               options->limit.value_or(header.transaction_count));
  SnapshotSOWWEngine engine;
  std::unordered_map<std::uint32_t, std::uint64_t> session_sequences;
  std::optional<std::uint64_t> previous_commit_timestamp;
  std::uint64_t consumed_events        = 0;
  std::uint64_t violation_transactions = 0;
  const auto begin                     = std::chrono::steady_clock::now();

  for (std::uint64_t transaction_index = 0;
       transaction_index < transaction_limit; ++transaction_index) {
    std::uint32_t session_id  = 0;
    std::uint64_t start_ts    = 0;
    std::uint64_t commit_ts   = 0;
    std::uint32_t event_count = 0;
    if (!input.read_u32(session_id) || !input.read_u64(start_ts) ||
        !input.read_u64(commit_ts) || !input.read_u32(event_count)) {
      std::cerr << "input error: truncated transaction " << transaction_index
                << '\n';
      return 3;
    }
    if (previous_commit_timestamp.has_value() &&
        commit_ts <= *previous_commit_timestamp) {
      std::cerr << "input error: commit tokens are not strictly increasing at "
                << transaction_index << '\n';
      return 3;
    }
    previous_commit_timestamp = commit_ts;

    std::vector<OwnedEvent> owned_events;
    owned_events.reserve(event_count);
    std::optional<EventSeq> previous_event_sequence;
    bool has_write = false;
    for (std::uint32_t event_index = 0; event_index < event_count;
         ++event_index) {
      OwnedEvent event;
      if (!read_event(input, event, error)) {
        std::cerr << "input error at transaction " << transaction_index
                  << ", event " << event_index << ": " << error << '\n';
        return 3;
      }
      if (previous_event_sequence.has_value() &&
          event.sequence <= *previous_event_sequence) {
        std::cerr << "input error: event sequence is not strictly increasing"
                  << '\n';
        return 3;
      }
      previous_event_sequence = event.sequence;
      has_write               = has_write || event.op == EventOp::Write;
      owned_events.push_back(std::move(event));
    }
    if (!has_write && commit_ts != start_ts) {
      std::cerr << "input error: read-only transaction " << transaction_index
                << " is not ordered by start_ts\n";
      return 3;
    }

    std::vector<EventView> views;
    views.reserve(owned_events.size());
    for (const auto &event : owned_events) {
      views.push_back(event.view());
    }
    auto &session_sequence = session_sequences[session_id];
    ++session_sequence;
    const auto result =
        engine.consume(make_metadata(transaction_index, session_id,
                                     session_sequence, commit_ts),
                       std::span<const EventView>(views));
    if (!result.ok()) {
      std::cerr << "engine error at transaction " << transaction_index << ": "
                << result.error->message << '\n';
      return 3;
    }
    if (result.transaction_violation.has_value()) {
      ++violation_transactions;
    }
    consumed_events += event_count;
  }

  const auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - begin);
  if (transaction_limit == header.transaction_count &&
      consumed_events != header.event_count) {
    std::cerr << "input error: event count does not match trace header\n";
    return 3;
  }
  const auto verdict = engine.verdict();
  if (!verdict.has_value()) {
    std::cerr << "engine error: verdict unavailable\n";
    return 3;
  }
  const auto status  = verdict->status == ConsistencyStatus::Pass
                           ? std::string_view("Pass")
                           : std::string_view("Violation");
  const auto metrics = engine.metrics();
  std::cout << "status=" << status << " transactions=" << transaction_limit
            << " events=" << consumed_events
            << " violating_transactions=" << violation_transactions
            << " retained_state_bytes=" << metrics.retained_state_bytes
            << " interned_key_bytes=" << metrics.interned_key_bytes
            << " interned_value_bytes=" << metrics.interned_value_bytes
            << " history_deltas=" << metrics.history_deltas
            << " elapsed_seconds=" << elapsed.count() << '\n';
  return verdict->status == ConsistencyStatus::Pass ? EXIT_SUCCESS : 2;
}
