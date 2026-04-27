#include "common/trace/trace_binary.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace common {

namespace {

constexpr std::uint32_t kBinaryTraceVersion = 1;
constexpr std::uint32_t kBinaryTraceFlags   = 0;

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
  if (flags != kBinaryTraceFlags) {
    return single_error("unsupported binary trace flags " +
                        std::to_string(flags));
  }
  if (txn_count > std::numeric_limits<TxnId>::max()) {
    return single_error("binary trace has too many transactions");
  }
  if (event_count > std::numeric_limits<EventId>::max()) {
    return single_error("binary trace has too many events");
  }

  Trace trace;
  trace.transactions.reserve(static_cast<std::size_t>(txn_count));
  trace.events.reserve(static_cast<std::size_t>(event_count));

  DiagnosticList diagnostics;
  std::unordered_set<Timestamp> seen_commit_ts;
  std::optional<Timestamp> previous_commit_ts;

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

      std::string error;
      auto key = read_hex_value(input, error);
      if (!key.has_value()) {
        return single_error(error);
      }

      std::optional<Value> value;
      if ((event_flags & kEventHasValue) != 0) {
        auto parsed_value = read_hex_value(input, error);
        if (!parsed_value.has_value()) {
          return single_error(error);
        }
        value = std::move(*parsed_value);
      }

      Event event;
      event.id        = static_cast<EventId>(trace.events.size());
      event.txn_id    = txn.id;
      event.seq       = seq;
      event.op        = *op;
      event.key       = std::move(*key);
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

      if (event.is_miss && event.value.has_value()) {
        diagnostics.push_back(binary_error(
            "read event cannot have both miss and value at transaction " +
            std::to_string(txn_index) + ", event " +
            std::to_string(event_index)));
        return Result<Trace>::err(std::move(diagnostics));
      }
      if (event.is_delete && event.value.has_value()) {
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

      txn.event_ids.push_back(event.id);
      trace.events.push_back(std::move(event));
    }

    trace.transactions.push_back(std::move(txn));
    previous_commit_ts = commit_ts;
  }

  if (trace.events.size() != event_count) {
    return single_error("binary trace event count does not match header");
  }

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
  if (!write_exact(output, kBinaryTraceMagic.data(),
                   kBinaryTraceMagic.size()) ||
      !write_u32(output, kBinaryTraceVersion) ||
      !write_u32(output, kBinaryTraceFlags) ||
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
