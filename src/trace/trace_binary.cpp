#include "common/trace/trace_binary.h"

#include "common/trace/dense_id_interner.h"

#include "common/trace/timekiller_json.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace common {

namespace {

constexpr std::uint32_t kBinaryTraceVersion = 1;

constexpr std::uint8_t kEventHasValue = 1U << 0U;
constexpr std::uint8_t kEventDelete   = 1U << 1U;
constexpr std::uint8_t kEventMiss     = 1U << 2U;

Diagnostic binary_error(std::string message) {
  Diagnostic diagnostic;
  diagnostic.severity = Severity::Error;
  diagnostic.checker  = "binary-parser";
  diagnostic.message  = std::move(message);
  return diagnostic;
}

Result<Trace> single_error(std::string message) {
  DiagnosticList diagnostics;
  diagnostics.push_back(binary_error(std::move(message)));
  return Result<Trace>::err(std::move(diagnostics));
}

bool read_exact(std::istream &input, char *data, const std::size_t size) {
  input.read(data, static_cast<std::streamsize>(size));
  return input.good() || input.gcount() == static_cast<std::streamsize>(size);
}

bool write_exact(std::ostream &output, const char *data,
                 const std::size_t size) {
  output.write(data, static_cast<std::streamsize>(size));
  return output.good();
}

bool read_u8(std::istream &input, std::uint8_t &value) {
  char byte = 0;
  if (!read_exact(input, &byte, 1)) {
    return false;
  }
  value = static_cast<std::uint8_t>(byte);
  return true;
}

bool read_u16(std::istream &input, std::uint16_t &value) {
  std::array<unsigned char, 2> bytes{};
  if (!read_exact(input, reinterpret_cast<char *>(bytes.data()),
                  bytes.size())) {
    return false;
  }
  value = static_cast<std::uint16_t>(bytes[0]) |
          (static_cast<std::uint16_t>(bytes[1]) << 8U);
  return true;
}

bool read_u32(std::istream &input, std::uint32_t &value) {
  std::array<unsigned char, 4> bytes{};
  if (!read_exact(input, reinterpret_cast<char *>(bytes.data()),
                  bytes.size())) {
    return false;
  }
  value = static_cast<std::uint32_t>(bytes[0]) |
          (static_cast<std::uint32_t>(bytes[1]) << 8U) |
          (static_cast<std::uint32_t>(bytes[2]) << 16U) |
          (static_cast<std::uint32_t>(bytes[3]) << 24U);
  return true;
}

bool read_u64(std::istream &input, std::uint64_t &value) {
  std::array<unsigned char, 8> bytes{};
  if (!read_exact(input, reinterpret_cast<char *>(bytes.data()),
                  bytes.size())) {
    return false;
  }
  value = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
  }
  return true;
}

bool write_u8(std::ostream &output, const std::uint8_t value) {
  const char byte = static_cast<char>(value);
  return write_exact(output, &byte, 1);
}

bool write_u16(std::ostream &output, const std::uint16_t value) {
  const std::array<char, 2> bytes{
      static_cast<char>(value & 0xffU),
      static_cast<char>((value >> 8U) & 0xffU),
  };
  return write_exact(output, bytes.data(), bytes.size());
}

bool write_u32(std::ostream &output, const std::uint32_t value) {
  const std::array<char, 4> bytes{
      static_cast<char>(value & 0xffU),
      static_cast<char>((value >> 8U) & 0xffU),
      static_cast<char>((value >> 16U) & 0xffU),
      static_cast<char>((value >> 24U) & 0xffU),
  };
  return write_exact(output, bytes.data(), bytes.size());
}

bool write_u64(std::ostream &output, const std::uint64_t value) {
  std::array<char, 8> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>((value >> (i * 8U)) & 0xffU);
  }
  return write_exact(output, bytes.data(), bytes.size());
}

std::optional<HexValue> read_hex_value(std::istream &input,
                                       std::string &error) {
  std::uint32_t byte_len = 0;
  if (!read_u32(input, byte_len)) {
    error = "unexpected EOF while reading byte string length";
    return std::nullopt;
  }

  std::vector<std::uint8_t> bytes(byte_len);
  if (!bytes.empty() &&
      !read_exact(input, reinterpret_cast<char *>(bytes.data()),
                  bytes.size())) {
    error = "unexpected EOF while reading byte string payload";
    return std::nullopt;
  }

  return HexValue::from_bytes(bytes);
}

bool write_hex_value(std::ostream &output, const HexValue &value,
                     std::string *error) {
  const auto bytes = value.to_bytes();
  if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    if (error != nullptr) {
      *error = "byte string is too large";
    }
    return false;
  }

  if (!write_u32(output, static_cast<std::uint32_t>(bytes.size()))) {
    if (error != nullptr) {
      *error = "failed to write byte string length";
    }
    return false;
  }
  if (!bytes.empty() &&
      !write_exact(output, reinterpret_cast<const char *>(bytes.data()),
                   bytes.size())) {
    if (error != nullptr) {
      *error = "failed to write byte string payload";
    }
    return false;
  }
  return true;
}

std::optional<EventOp> decode_op(const std::uint8_t op) {
  if (op == 0) {
    return EventOp::Read;
  }
  if (op == 1) {
    return EventOp::Write;
  }
  return std::nullopt;
}

std::uint8_t encode_op(const EventOp op) { return op == EventOp::Read ? 0 : 1; }

struct BinaryTraceHeaderRecord {
  std::uint64_t transaction_count = 0;
  std::uint64_t event_count       = 0;
  std::uint32_t flags             = kBinaryTraceRawFlags;

  [[nodiscard]] bool has_dense_ids() const {
    return (flags & kBinaryTraceDenseIdsFlag) != 0;
  }
};

struct BinaryTransactionRecord {
  std::uint64_t index       = 0;
  SessionId session_id      = 0;
  Timestamp start_ts        = 0;
  Timestamp commit_ts       = 0;
  std::uint32_t event_count = 0;
};

struct BinaryEventRecord {
  std::uint64_t index             = 0;
  std::uint64_t transaction_index = 0;
  EventSeq seq                    = 0;
  EventOp op                      = EventOp::Read;
  Key key;
  std::optional<Value> value;
  KeyId key_id       = 0;
  ValueId value_id   = kNullValueId;
  bool has_dense_ids = false;
  bool has_value     = false;
  bool is_delete     = false;
  bool is_miss       = false;
};

struct BinaryDecodeStats {
  std::uint64_t transactions = 0;
  std::uint64_t events       = 0;
};

template <typename T> Result<T> decode_error(std::string message) {
  DiagnosticList diagnostics;
  diagnostics.push_back(binary_error(std::move(message)));
  return Result<T>::err(std::move(diagnostics));
}

template <typename Consumer>
Result<BinaryDecodeStats> decode_binary_trace_stream(std::istream &input,
                                                     ParseOptions options,
                                                     Consumer &consumer) {
  std::array<char, kBinaryTraceMagic.size()> magic{};
  if (!read_exact(input, magic.data(), magic.size()) ||
      std::string_view(magic.data(), magic.size()) != kBinaryTraceMagic) {
    return decode_error<BinaryDecodeStats>("invalid binary trace magic");
  }

  std::uint32_t version = 0;
  std::uint32_t flags   = 0;
  BinaryTraceHeaderRecord header;
  if (!read_u32(input, version) || !read_u32(input, flags) ||
      !read_u64(input, header.transaction_count) ||
      !read_u64(input, header.event_count)) {
    return decode_error<BinaryDecodeStats>("truncated binary trace header");
  }

  if (version != kBinaryTraceVersion) {
    return decode_error<BinaryDecodeStats>("unsupported binary trace version " +
                                           std::to_string(version));
  }
  if ((flags & ~kBinaryTraceDenseIdsFlag) != 0) {
    return decode_error<BinaryDecodeStats>("unsupported binary trace flags " +
                                           std::to_string(flags));
  }
  header.flags = flags;
  if (header.transaction_count > std::numeric_limits<TxnId>::max()) {
    return decode_error<BinaryDecodeStats>(
        "binary trace has too many transactions");
  }
  if (header.event_count > std::numeric_limits<EventId>::max()) {
    return decode_error<BinaryDecodeStats>("binary trace has too many events");
  }

  std::string consumer_error;
  if (!consumer.on_header(header, consumer_error)) {
    return decode_error<BinaryDecodeStats>(
        consumer_error.empty() ? "binary trace consumer rejected the header"
                               : std::move(consumer_error));
  }

  std::unordered_set<Timestamp> seen_commit_ts;
  std::optional<Timestamp> previous_commit_ts;
  std::uint64_t observed_events = 0;
  std::uint64_t next_key_id     = 0;
  std::uint64_t next_value_id   = 0;

  for (std::uint64_t txn_index = 0; txn_index < header.transaction_count;
       ++txn_index) {
    BinaryTransactionRecord txn;
    txn.index = txn_index;
    if (!read_u32(input, txn.session_id) || !read_u64(input, txn.start_ts) ||
        !read_u64(input, txn.commit_ts) || !read_u32(input, txn.event_count)) {
      return decode_error<BinaryDecodeStats>(
          "truncated transaction record at index " + std::to_string(txn_index));
    }

    if (previous_commit_ts.has_value() && options.require_commit_order &&
        txn.commit_ts < *previous_commit_ts) {
      return decode_error<BinaryDecodeStats>(
          "transaction " + std::to_string(txn_index) +
          " has commit_ts smaller than previous commit_ts");
    }
    if (options.reject_duplicate_commit_ts &&
        !seen_commit_ts.insert(txn.commit_ts).second) {
      return decode_error<BinaryDecodeStats>(
          "transaction " + std::to_string(txn_index) +
          " duplicates an earlier commit_ts");
    }
    if (txn.event_count > header.event_count - observed_events) {
      return decode_error<BinaryDecodeStats>(
          "binary trace event count exceeds header at transaction " +
          std::to_string(txn_index));
    }

    consumer_error.clear();
    if (!consumer.on_transaction_begin(txn, consumer_error)) {
      return decode_error<BinaryDecodeStats>(
          consumer_error.empty()
              ? "binary trace consumer rejected transaction " +
                    std::to_string(txn_index)
              : std::move(consumer_error));
    }

    std::optional<EventSeq> previous_seq;
    for (std::uint32_t event_index = 0; event_index < txn.event_count;
         ++event_index) {
      BinaryEventRecord event;
      event.index             = observed_events;
      event.transaction_index = txn_index;

      std::uint8_t op_byte     = 0;
      std::uint8_t event_flags = 0;
      std::uint16_t reserved   = 0;
      if (!read_u32(input, event.seq) || !read_u8(input, op_byte) ||
          !read_u8(input, event_flags) || !read_u16(input, reserved)) {
        return decode_error<BinaryDecodeStats>(
            "truncated event record at transaction " +
            std::to_string(txn_index) + ", event " +
            std::to_string(event_index));
      }

      if (reserved != 0) {
        return decode_error<BinaryDecodeStats>(
            "event reserved field must be zero at transaction " +
            std::to_string(txn_index) + ", event " +
            std::to_string(event_index));
      }
      if ((event_flags & ~(kEventHasValue | kEventDelete | kEventMiss)) != 0) {
        return decode_error<BinaryDecodeStats>(
            "unknown event flags at transaction " + std::to_string(txn_index) +
            ", event " + std::to_string(event_index));
      }
      const auto op = decode_op(op_byte);
      if (!op.has_value()) {
        return decode_error<BinaryDecodeStats>(
            "invalid event op at transaction " + std::to_string(txn_index) +
            ", event " + std::to_string(event_index));
      }
      event.op = *op;

      event.has_dense_ids = header.has_dense_ids();
      event.has_value     = (event_flags & kEventHasValue) != 0;
      if (event.has_dense_ids) {
        if (!read_u32(input, event.key_id) ||
            !read_u32(input, event.value_id)) {
          return decode_error<BinaryDecodeStats>(
              "truncated dense IDs at transaction " +
              std::to_string(txn_index) + ", event " +
              std::to_string(event_index));
        }
        if (event.key_id == std::numeric_limits<KeyId>::max() ||
            event.key_id > next_key_id) {
          return decode_error<BinaryDecodeStats>(
              "dense key IDs must be contiguous in first-seen order at "
              "transaction " +
              std::to_string(txn_index) + ", event " +
              std::to_string(event_index));
        }
        if (event.key_id == next_key_id) {
          ++next_key_id;
        }
        if (event.has_value) {
          if (event.value_id == kNullValueId ||
              event.value_id > next_value_id) {
            return decode_error<BinaryDecodeStats>(
                "dense value IDs must be contiguous in first-seen order at "
                "transaction " +
                std::to_string(txn_index) + ", event " +
                std::to_string(event_index));
          }
          if (event.value_id == next_value_id) {
            ++next_value_id;
          }
        } else if (event.value_id != kNullValueId) {
          return decode_error<BinaryDecodeStats>(
              "dense event without a value must use the null value ID at "
              "transaction " +
              std::to_string(txn_index) + ", event " +
              std::to_string(event_index));
        }
      } else {
        std::string error;
        auto key = read_hex_value(input, error);
        if (!key.has_value()) {
          return decode_error<BinaryDecodeStats>(std::move(error));
        }
        event.key = std::move(*key);

        if (event.has_value) {
          auto value = read_hex_value(input, error);
          if (!value.has_value()) {
            return decode_error<BinaryDecodeStats>(std::move(error));
          }
          event.value = std::move(*value);
        }
      }
      event.is_delete = (event_flags & kEventDelete) != 0;
      event.is_miss   = (event_flags & kEventMiss) != 0;

      if (previous_seq.has_value() && event.seq <= *previous_seq) {
        return decode_error<BinaryDecodeStats>(
            "event seq must be strictly increasing at transaction " +
            std::to_string(txn_index) + ", event " +
            std::to_string(event_index));
      }
      previous_seq = event.seq;

      if (event.is_miss && event.has_value) {
        return decode_error<BinaryDecodeStats>(
            "read event cannot have both miss and value at transaction " +
            std::to_string(txn_index) + ", event " +
            std::to_string(event_index));
      }
      if (event.is_delete && event.has_value) {
        return decode_error<BinaryDecodeStats>(
            "write event cannot have both delete and value at transaction " +
            std::to_string(txn_index) + ", event " +
            std::to_string(event_index));
      }
      if (event.op == EventOp::Read && event.is_delete) {
        return decode_error<BinaryDecodeStats>(
            "read event cannot have delete at transaction " +
            std::to_string(txn_index) + ", event " +
            std::to_string(event_index));
      }
      if (event.op == EventOp::Write && event.is_miss) {
        return decode_error<BinaryDecodeStats>(
            "write event cannot have miss at transaction " +
            std::to_string(txn_index) + ", event " +
            std::to_string(event_index));
      }

      consumer_error.clear();
      if (!consumer.on_event(std::move(event), consumer_error)) {
        return decode_error<BinaryDecodeStats>(
            consumer_error.empty()
                ? "binary trace consumer rejected event at transaction " +
                      std::to_string(txn_index) + ", event " +
                      std::to_string(event_index)
                : std::move(consumer_error));
      }
      ++observed_events;
    }

    consumer_error.clear();
    if (!consumer.on_transaction_end(txn, consumer_error)) {
      return decode_error<BinaryDecodeStats>(
          consumer_error.empty()
              ? "binary trace consumer rejected transaction " +
                    std::to_string(txn_index)
              : std::move(consumer_error));
    }
    previous_commit_ts = txn.commit_ts;
  }

  if (observed_events != header.event_count) {
    return decode_error<BinaryDecodeStats>(
        "binary trace event count does not match header");
  }

  if (input.peek() != std::char_traits<char>::eof()) {
    return decode_error<BinaryDecodeStats>(
        "binary trace has trailing data after declared records");
  }
  if (input.bad()) {
    return decode_error<BinaryDecodeStats>(
        "failed while checking the end of the binary trace");
  }

  consumer_error.clear();
  if (!consumer.on_trace_end(consumer_error)) {
    return decode_error<BinaryDecodeStats>(
        consumer_error.empty() ? "binary trace consumer failed to finish"
                               : std::move(consumer_error));
  }

  return Result<BinaryDecodeStats>::ok(
      {.transactions = header.transaction_count, .events = observed_events});
}

class TimeKillerJsonConsumer {
public:
  explicit TimeKillerJsonConsumer(std::ostream &output) : output_(output) {}

  bool on_header(const BinaryTraceHeaderRecord &header, std::string &error) {
    expected_transactions_ = header.transaction_count;
    expected_events_       = header.event_count;
    dense_input_           = header.has_dense_ids();
    return write("[", error);
  }

  bool on_transaction_begin(const BinaryTransactionRecord &record,
                            std::string &error) {
    constexpr auto java_long_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (record.index > java_long_max) {
      error = "transaction id at index " + std::to_string(record.index) +
              " does not fit signed Java long";
      return false;
    }
    if (record.session_id > java_long_max) {
      error = "session id at transaction " + std::to_string(record.index) +
              " does not fit signed Java long";
      return false;
    }
    if (record.start_ts > java_long_max) {
      error = "start_ts at transaction " + std::to_string(record.index) +
              " does not fit signed Java long";
      return false;
    }
    if (record.commit_ts > java_long_max) {
      error = "commit_ts at transaction " + std::to_string(record.index) +
              " does not fit signed Java long";
      return false;
    }
    if (record.start_ts > record.commit_ts) {
      error = "start_ts exceeds commit_ts at transaction " +
              std::to_string(record.index);
      return false;
    }

    current_transaction_events_ = 0;
    if (transactions_ != 0 && !write(",", error)) {
      return false;
    }
    if (!write("{\"tid\":", error) || !write_number(record.index, error) ||
        !write(",\"sid\":", error) || !write_number(record.session_id, error) ||
        !write(",\"sts\":{\"p\":", error) ||
        !write_number(record.start_ts, error) ||
        !write(",\"l\":0},\"cts\":{\"p\":", error) ||
        !write_number(record.commit_ts, error) ||
        !write(",\"l\":0},\"ops\":[", error)) {
      return false;
    }
    ++transactions_;
    return true;
  }

  bool on_event(BinaryEventRecord record, std::string &error) {
    std::int64_t key_id = 0;
    std::optional<std::int64_t> value_id;
    if (record.has_dense_ids) {
      key_id = static_cast<std::int64_t>(record.key_id);
      dense_key_count_ =
          std::max(dense_key_count_, static_cast<std::uint64_t>(record.key_id) +
                                         1U);
      if (record.has_value) {
        value_id = static_cast<std::int64_t>(record.value_id);
        dense_value_count_ =
            std::max(dense_value_count_,
                     static_cast<std::uint64_t>(record.value_id) + 1U);
      }
    } else {
      if (!intern(key_ids_, std::move(record.key), next_key_id_, key_id, "key",
                  error)) {
        return false;
      }
      if (record.value.has_value()) {
        std::int64_t id = 0;
        if (!intern(value_ids_, std::move(*record.value), next_value_id_, id,
                    "value", error)) {
          return false;
        }
        value_id = id;
      }
    }

    if (current_transaction_events_ != 0 && !write(",", error)) {
      return false;
    }
    if (!write(record.op == EventOp::Read ? "{\"t\":\"r\",\"k\":"
                                          : "{\"t\":\"w\",\"k\":",
               error) ||
        !write_number(static_cast<std::uint64_t>(key_id), error) ||
        !write(",\"v\":", error)) {
      return false;
    }
    if (value_id.has_value()) {
      if (!write_number(static_cast<std::uint64_t>(*value_id), error)) {
        return false;
      }
    } else if (!write("null", error)) {
      return false;
    }
    if (!write("}", error)) {
      return false;
    }

    ++current_transaction_events_;
    ++events_;
    return true;
  }

  bool on_transaction_end(const BinaryTransactionRecord &, std::string &error) {
    return write("]}", error);
  }

  bool on_trace_end(std::string &error) {
    if (transactions_ != expected_transactions_ ||
        events_ != expected_events_) {
      error = "converted record counts do not match the binary trace header";
      return false;
    }
    return write("]", error);
  }

  [[nodiscard]] TimeKillerJsonStats stats() const {
    return {.transactions = transactions_,
            .events       = events_,
            .keys         = dense_input_
                                ? dense_key_count_
                                : static_cast<std::uint64_t>(key_ids_.size()),
            .values       = dense_input_
                                ? dense_value_count_
                                : static_cast<std::uint64_t>(value_ids_.size()),
            .output_bytes = output_bytes_};
  }

private:
  using IdMap = std::unordered_map<HexValue, std::int64_t, HexValueHash>;

  bool write(const std::string_view text, std::string &error) {
    output_.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output_) {
      error = "failed to write TimeKiller JSON output";
      return false;
    }
    output_bytes_ += static_cast<std::uint64_t>(text.size());
    return true;
  }

  bool write_number(const std::uint64_t value, std::string &error) {
    std::array<char, 32> buffer{};
    const auto [end, status] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (status != std::errc{}) {
      error = "failed to format integer for TimeKiller JSON output";
      return false;
    }
    return write(std::string_view(buffer.data(), static_cast<std::size_t>(
                                                     end - buffer.data())),
                 error);
  }

  bool intern(IdMap &ids, HexValue value, std::uint64_t &next_id,
              std::int64_t &id, const std::string_view kind,
              std::string &error) {
    const auto found = ids.find(value);
    if (found != ids.end()) {
      id = found->second;
      return true;
    }

    constexpr auto java_long_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (next_id > java_long_max) {
      error = "generated " + std::string(kind) +
              " id does not fit signed Java long";
      return false;
    }

    id = static_cast<std::int64_t>(next_id);
    ids.emplace(std::move(value), id);
    ++next_id;
    return true;
  }

  std::ostream &output_;
  IdMap key_ids_;
  IdMap value_ids_;
  std::uint64_t next_key_id_                = 0;
  std::uint64_t next_value_id_              = 0;
  std::uint64_t expected_transactions_      = 0;
  std::uint64_t expected_events_            = 0;
  std::uint64_t transactions_               = 0;
  std::uint64_t events_                     = 0;
  std::uint64_t current_transaction_events_ = 0;
  std::uint64_t output_bytes_               = 0;
  std::uint64_t dense_key_count_             = 0;
  std::uint64_t dense_value_count_           = 0;
  bool dense_input_                          = false;
};

class DenseBinaryConsumer {
public:
  explicit DenseBinaryConsumer(std::ostream &output) : output_(output) {}

  bool on_header(const BinaryTraceHeaderRecord &header, std::string &error) {
    if (header.has_dense_ids()) {
      error = "dense trace input is already densified";
      return false;
    }
    expected_transactions_ = header.transaction_count;
    expected_events_       = header.event_count;
    if (!write_exact(output_, kBinaryTraceMagic.data(),
                     kBinaryTraceMagic.size()) ||
        !write_u32(output_, kBinaryTraceVersion) ||
        !write_u32(output_, kBinaryTraceDenseIdsFlag) ||
        !write_u64(output_, header.transaction_count) ||
        !write_u64(output_, header.event_count)) {
      error = "failed to write dense binary trace header";
      return false;
    }
    output_bytes_ = static_cast<std::uint64_t>(kBinaryTraceMagic.size()) +
                    sizeof(std::uint32_t) * 2U + sizeof(std::uint64_t) * 2U;
    return true;
  }

  bool on_transaction_begin(const BinaryTransactionRecord &record,
                            std::string &error) {
    if (!write_u32(output_, record.session_id) ||
        !write_u64(output_, record.start_ts) ||
        !write_u64(output_, record.commit_ts) ||
        !write_u32(output_, record.event_count)) {
      error = "failed to write dense transaction record at index " +
              std::to_string(record.index);
      return false;
    }
    output_bytes_ += sizeof(std::uint32_t) * 2U +
                     sizeof(std::uint64_t) * 2U;
    ++transactions_;
    return true;
  }

  bool on_event(BinaryEventRecord record, std::string &error) {
    KeyId key_id = 0;
    if (!intern(key_ids_, std::move(record.key), next_key_id_, key_id, "key",
                error)) {
      return false;
    }

    ValueId value_id = kNullValueId;
    if (record.value.has_value() &&
        !intern(value_ids_, std::move(*record.value), next_value_id_, value_id,
                "value", error)) {
      return false;
    }

    std::uint8_t flags = 0;
    if (record.has_value) {
      flags |= kEventHasValue;
    }
    if (record.is_delete) {
      flags |= kEventDelete;
    }
    if (record.is_miss) {
      flags |= kEventMiss;
    }
    if (!write_u32(output_, record.seq) ||
        !write_u8(output_, encode_op(record.op)) ||
        !write_u8(output_, flags) || !write_u16(output_, 0) ||
        !write_u32(output_, key_id) || !write_u32(output_, value_id)) {
      error = "failed to write dense event record at index " +
              std::to_string(record.index);
      return false;
    }
    output_bytes_ += sizeof(std::uint32_t) * 3U + sizeof(std::uint8_t) * 2U +
                     sizeof(std::uint16_t);
    ++events_;
    return true;
  }

  bool on_transaction_end(const BinaryTransactionRecord &,
                          std::string &) {
    return true;
  }

  bool on_trace_end(std::string &error) {
    if (transactions_ != expected_transactions_ || events_ != expected_events_) {
      error = "densified record counts do not match the binary trace header";
      return false;
    }
    return true;
  }

  [[nodiscard]] DenseBinaryTraceStats stats() const {
    return {.transactions = transactions_,
            .events       = events_,
            .keys         = static_cast<std::uint64_t>(key_ids_.size()),
            .values       = static_cast<std::uint64_t>(value_ids_.size()),
            .output_bytes = output_bytes_};
  }

private:
  using KeyIdMap   = std::unordered_map<HexValue, KeyId, HexValueHash>;
  using ValueIdMap = std::unordered_map<HexValue, ValueId, HexValueHash>;

  template <typename Id, typename Map>
  bool intern(Map &ids, HexValue value, std::uint64_t &next_id, Id &id,
              const std::string_view kind, std::string &error) {
    const auto found = ids.find(value);
    if (found != ids.end()) {
      id = found->second;
      return true;
    }
    if (next_id >= std::numeric_limits<Id>::max()) {
      error = "dense trace exceeds the " + std::string(kind) + " ID range";
      return false;
    }
    id = static_cast<Id>(next_id);
    ids.emplace(std::move(value), id);
    ++next_id;
    return true;
  }

  std::ostream &output_;
  KeyIdMap key_ids_;
  ValueIdMap value_ids_;
  std::uint64_t next_key_id_           = 0;
  std::uint64_t next_value_id_         = 0;
  std::uint64_t expected_transactions_ = 0;
  std::uint64_t expected_events_       = 0;
  std::uint64_t transactions_          = 0;
  std::uint64_t events_                = 0;
  std::uint64_t output_bytes_          = 0;
};

} // namespace

bool is_binary_trace_stream(std::istream &input) {
  const auto original_pos = input.tellg();
  if (original_pos == std::streampos(-1)) {
    return false;
  }

  std::array<char, kBinaryTraceMagic.size()> magic{};
  const auto matched =
      read_exact(input, magic.data(), magic.size()) &&
      std::string_view(magic.data(), magic.size()) == kBinaryTraceMagic;

  input.clear();
  input.seekg(original_pos);
  return matched;
}

Result<TimeKillerJsonStats> write_timekiller_json(std::istream &input,
                                                  std::ostream &output,
                                                  ParseOptions options) {
  TimeKillerJsonConsumer consumer(output);
  auto decoded = decode_binary_trace_stream(input, options, consumer);
  if (!decoded) {
    return Result<TimeKillerJsonStats>::err(decoded.error());
  }

  output.flush();
  if (!output) {
    return decode_error<TimeKillerJsonStats>(
        "failed to flush TimeKiller JSON output");
  }
  return Result<TimeKillerJsonStats>::ok(consumer.stats());
}

Result<DenseBinaryTraceStats>
write_dense_binary_trace(std::istream &input, std::ostream &output,
                         ParseOptions options) {
  DenseBinaryConsumer consumer(output);
  auto decoded = decode_binary_trace_stream(input, options, consumer);
  if (!decoded) {
    return Result<DenseBinaryTraceStats>::err(decoded.error());
  }

  output.flush();
  if (!output) {
    return decode_error<DenseBinaryTraceStats>(
        "failed to flush dense binary trace output");
  }
  return Result<DenseBinaryTraceStats>::ok(consumer.stats());
}

Result<Trace> read_binary_trace_stream(std::istream &input,
                                       ParseOptions options) {
  std::array<char, kBinaryTraceMagic.size()> magic{};
  if (!read_exact(input, magic.data(), magic.size()) ||
      std::string_view(magic.data(), magic.size()) != kBinaryTraceMagic) {
    return single_error("invalid binary trace magic");
  }

  std::uint32_t version     = 0;
  std::uint32_t flags       = 0;
  std::uint64_t txn_count   = 0;
  std::uint64_t event_count = 0;
  if (!read_u32(input, version) || !read_u32(input, flags) ||
      !read_u64(input, txn_count) || !read_u64(input, event_count)) {
    return single_error("truncated binary trace header");
  }

  if (version != kBinaryTraceVersion) {
    return single_error("unsupported binary trace version " +
                        std::to_string(version));
  }
  if ((flags & ~kBinaryTraceDenseIdsFlag) != 0) {
    return single_error("unsupported binary trace flags " +
                        std::to_string(flags));
  }
  const bool has_dense_ids = (flags & kBinaryTraceDenseIdsFlag) != 0;
  if (txn_count > std::numeric_limits<TxnId>::max()) {
    return single_error("binary trace has too many transactions");
  }
  if (event_count > std::numeric_limits<EventId>::max()) {
    return single_error("binary trace has too many events");
  }

  Trace trace;
  trace.has_raw_payload = !has_dense_ids;
  trace.transactions.reserve(static_cast<std::size_t>(txn_count));
  trace.events.reserve(static_cast<std::size_t>(event_count));
  trace.event_key_ids.reserve(static_cast<std::size_t>(event_count));
  trace.event_value_ids.reserve(static_cast<std::size_t>(event_count));

  DenseIdInterner<KeyId> key_interner;
  DenseIdInterner<ValueId> value_interner;

  DiagnosticList diagnostics;
  std::unordered_set<Timestamp> seen_commit_ts;
  std::optional<Timestamp> previous_commit_ts;
  std::uint64_t next_key_id   = 0;
  std::uint64_t next_value_id = 0;

  for (std::uint64_t txn_index = 0; txn_index < txn_count; ++txn_index) {
    std::uint32_t session_id      = 0;
    std::uint64_t start_ts        = 0;
    std::uint64_t commit_ts       = 0;
    std::uint32_t txn_event_count = 0;
    if (!read_u32(input, session_id) || !read_u64(input, start_ts) ||
        !read_u64(input, commit_ts) || !read_u32(input, txn_event_count)) {
      return single_error("truncated transaction record at index " +
                          std::to_string(txn_index));
    }

    if (previous_commit_ts.has_value() && options.require_commit_order &&
        commit_ts < *previous_commit_ts) {
      diagnostics.push_back(
          binary_error("transaction " + std::to_string(txn_index) +
                       " has commit_ts smaller than previous commit_ts"));
      return Result<Trace>::err(std::move(diagnostics));
    }
    if (txn_event_count > event_count - trace.events.size()) {
      return single_error("binary trace event count exceeds header at "
                          "transaction " +
                          std::to_string(txn_index));
    }
    if (options.reject_duplicate_commit_ts &&
        !seen_commit_ts.insert(commit_ts).second) {
      diagnostics.push_back(binary_error("transaction " +
                                         std::to_string(txn_index) +
                                         " duplicates an earlier commit_ts"));
      return Result<Trace>::err(std::move(diagnostics));
    }

    Transaction txn;
    txn.id         = static_cast<TxnId>(trace.transactions.size());
    txn.session_id = session_id;
    txn.start_ts   = start_ts;
    txn.commit_ts  = commit_ts;
    txn.event_ids.reserve(txn_event_count);

    std::optional<EventSeq> previous_seq;
    for (std::uint32_t event_index = 0; event_index < txn_event_count;
         ++event_index) {
      std::uint32_t seq        = 0;
      std::uint8_t op_byte     = 0;
      std::uint8_t event_flags = 0;
      std::uint16_t reserved   = 0;
      if (!read_u32(input, seq) || !read_u8(input, op_byte) ||
          !read_u8(input, event_flags) || !read_u16(input, reserved)) {
        return single_error("truncated event record at transaction " +
                            std::to_string(txn_index) + ", event " +
                            std::to_string(event_index));
      }

      if (reserved != 0) {
        return single_error(
            "event reserved field must be zero at transaction " +
            std::to_string(txn_index) + ", event " +
            std::to_string(event_index));
      }
      if ((event_flags & ~(kEventHasValue | kEventDelete | kEventMiss)) != 0) {
        return single_error("unknown event flags at transaction " +
                            std::to_string(txn_index) + ", event " +
                            std::to_string(event_index));
      }
      const auto op = decode_op(op_byte);
      if (!op.has_value()) {
        return single_error("invalid event op at transaction " +
                            std::to_string(txn_index) + ", event " +
                            std::to_string(event_index));
      }

      const bool has_value = (event_flags & kEventHasValue) != 0;
      Key key;
      std::optional<Value> value;
      KeyId dense_key_id     = 0;
      ValueId dense_value_id = kNullValueId;
      if (has_dense_ids) {
        if (!read_u32(input, dense_key_id) ||
            !read_u32(input, dense_value_id)) {
          return single_error("truncated dense IDs at transaction " +
                              std::to_string(txn_index) + ", event " +
                              std::to_string(event_index));
        }
        if (dense_key_id == std::numeric_limits<KeyId>::max() ||
            dense_key_id > next_key_id) {
          return single_error(
              "dense key IDs must be contiguous in first-seen order at "
              "transaction " +
              std::to_string(txn_index) + ", event " +
              std::to_string(event_index));
        }
        if (dense_key_id == next_key_id) {
          ++next_key_id;
        }
        if (has_value) {
          if (dense_value_id == kNullValueId ||
              dense_value_id > next_value_id) {
            return single_error(
                "dense value IDs must be contiguous in first-seen order at "
                "transaction " +
                std::to_string(txn_index) + ", event " +
                std::to_string(event_index));
          }
          if (dense_value_id == next_value_id) {
            ++next_value_id;
          }
        } else if (dense_value_id != kNullValueId) {
          return single_error(
              "dense event without a value must use the null value ID at "
              "transaction " +
              std::to_string(txn_index) + ", event " +
              std::to_string(event_index));
        }
      } else {
        std::string error;
        auto parsed_key = read_hex_value(input, error);
        if (!parsed_key.has_value()) {
          return single_error(error);
        }
        key = std::move(*parsed_key);

        if (has_value) {
          auto parsed_value = read_hex_value(input, error);
          if (!parsed_value.has_value()) {
            return single_error(error);
          }
          value = std::move(*parsed_value);
        }
      }

      Event event;
      event.id        = static_cast<EventId>(trace.events.size());
      event.txn_id    = txn.id;
      event.seq       = seq;
      event.op        = *op;
      event.key       = std::move(key);
      event.value     = std::move(value);
      event.is_delete = (event_flags & kEventDelete) != 0;
      event.is_miss   = (event_flags & kEventMiss) != 0;

      if (previous_seq.has_value() && event.seq <= *previous_seq) {
        diagnostics.push_back(binary_error(
            "event seq must be strictly increasing at transaction " +
            std::to_string(txn_index) + ", event " +
            std::to_string(event_index)));
        return Result<Trace>::err(std::move(diagnostics));
      }
      previous_seq = event.seq;

      if (event.is_miss && has_value) {
        diagnostics.push_back(binary_error(
            "read event cannot have both miss and value at transaction " +
            std::to_string(txn_index) + ", event " +
            std::to_string(event_index)));
        return Result<Trace>::err(std::move(diagnostics));
      }
      if (event.is_delete && has_value) {
        diagnostics.push_back(binary_error(
            "write event cannot have both delete and value at transaction " +
            std::to_string(txn_index) + ", event " +
            std::to_string(event_index)));
        return Result<Trace>::err(std::move(diagnostics));
      }
      if (event.op == EventOp::Read && event.is_delete) {
        diagnostics.push_back(
            binary_error("read event cannot have delete at transaction " +
                         std::to_string(txn_index) + ", event " +
                         std::to_string(event_index)));
        return Result<Trace>::err(std::move(diagnostics));
      }
      if (event.op == EventOp::Write && event.is_miss) {
        diagnostics.push_back(
            binary_error("write event cannot have miss at transaction " +
                         std::to_string(txn_index) + ", event " +
                         std::to_string(event_index)));
        return Result<Trace>::err(std::move(diagnostics));
      }

      trace.events.push_back(std::move(event));
      const auto &stored_event = trace.events.back();
      KeyId key_id       = dense_key_id;
      ValueId value_id   = dense_value_id;
      if (!has_dense_ids) {
        const auto interned_key = key_interner.intern(stored_event.key);
        if (!interned_key.has_value()) {
          return single_error("binary trace exceeds the dense key ID range");
        }
        key_id = *interned_key;
        if (stored_event.value.has_value()) {
          const auto interned_value =
              value_interner.intern(*stored_event.value);
          if (!interned_value.has_value()) {
            return single_error(
                "binary trace exceeds the dense value ID range");
          }
          value_id = *interned_value;
        }
      }

      txn.event_ids.push_back(stored_event.id);
      trace.event_key_ids.push_back(key_id);
      trace.event_value_ids.push_back(value_id);
    }

    trace.transactions.push_back(std::move(txn));
    previous_commit_ts = commit_ts;
  }

  if (trace.events.size() != event_count) {
    return single_error("binary trace event count does not match header");
  }
  if (input.peek() != std::char_traits<char>::eof()) {
    return single_error("binary trace has trailing data after declared records");
  }
  if (input.bad()) {
    return single_error("failed while checking the end of the binary trace");
  }

  trace.key_count = has_dense_ids ? static_cast<std::size_t>(next_key_id)
                                  : key_interner.size();
  trace.value_count = has_dense_ids ? static_cast<std::size_t>(next_value_id)
                                    : value_interner.size();

  return Result<Trace>::ok(std::move(trace));
}

Result<Trace> read_binary_trace_file(std::string_view path,
                                     ParseOptions options) {
  std::ifstream input{std::string(path), std::ios::binary};
  if (!input.is_open()) {
    return single_error("failed to open binary trace file `" +
                        std::string(path) + "`");
  }
  return read_binary_trace_stream(input, options);
}

bool write_binary_trace_stream(std::ostream &output, const Trace &trace,
                               std::string *error) {
  if (!trace.has_raw_payload) {
    if (error != nullptr) {
      *error = "cannot write a raw binary trace without raw key/value data";
    }
    return false;
  }
  if (!write_exact(output, kBinaryTraceMagic.data(),
                   kBinaryTraceMagic.size()) ||
      !write_u32(output, kBinaryTraceVersion) ||
      !write_u32(output, kBinaryTraceRawFlags) ||
      !write_u64(output,
                 static_cast<std::uint64_t>(trace.transactions.size())) ||
      !write_u64(output, static_cast<std::uint64_t>(trace.events.size()))) {
    if (error != nullptr) {
      *error = "failed to write binary trace header";
    }
    return false;
  }

  for (const auto &txn : trace.transactions) {
    if (txn.event_ids.size() > std::numeric_limits<std::uint32_t>::max()) {
      if (error != nullptr) {
        *error = "transaction has too many events";
      }
      return false;
    }
    if (!write_u32(output, txn.session_id) ||
        !write_u64(output, txn.start_ts) || !write_u64(output, txn.commit_ts) ||
        !write_u32(output, static_cast<std::uint32_t>(txn.event_ids.size()))) {
      if (error != nullptr) {
        *error = "failed to write transaction record";
      }
      return false;
    }

    for (const auto event_id : txn.event_ids) {
      const auto &event  = trace.getEvent(event_id);
      std::uint8_t flags = 0;
      if (event.value.has_value()) {
        flags |= kEventHasValue;
      }
      if (event.is_delete) {
        flags |= kEventDelete;
      }
      if (event.is_miss) {
        flags |= kEventMiss;
      }

      if (!write_u32(output, event.seq) ||
          !write_u8(output, encode_op(event.op)) || !write_u8(output, flags) ||
          !write_u16(output, 0)) {
        if (error != nullptr) {
          *error = "failed to write event record";
        }
        return false;
      }
      if (!write_hex_value(output, event.key, error)) {
        return false;
      }
      if (event.value.has_value() &&
          !write_hex_value(output, *event.value, error)) {
        return false;
      }
    }
  }

  return output.good();
}

bool write_binary_trace_file(std::string_view path, const Trace &trace,
                             std::string *error) {
  std::ofstream output{std::string(path), std::ios::binary};
  if (!output.is_open()) {
    if (error != nullptr) {
      *error = "failed to open binary trace file `" + std::string(path) + "`";
    }
    return false;
  }
  return write_binary_trace_stream(output, trace, error);
}

} // namespace common
