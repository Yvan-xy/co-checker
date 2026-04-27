#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace common {

using TxnId     = std::uint32_t;
using EventId   = std::uint32_t;
using SessionId = std::uint32_t;
using Timestamp = std::uint64_t;
using EventSeq  = std::uint32_t;

struct HexValue {
  std::uint32_t byte_len = 0;
  std::array<std::uint64_t, 2> inline_words{};
  std::vector<std::uint64_t> extra_words;

  static std::optional<HexValue> from_hex(std::string_view hex,
                                          std::string *error = nullptr);
  static HexValue from_bytes(std::span<const std::uint8_t> bytes);

  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;

  [[nodiscard]] bool operator==(const HexValue &rhs) const {
    return byte_len == rhs.byte_len && inline_words == rhs.inline_words &&
           extra_words == rhs.extra_words;
  }

  [[nodiscard]] bool operator!=(const HexValue &rhs) const {
    return !(*this == rhs);
  }
};

struct HexValueHash {
  [[nodiscard]] std::size_t operator()(const HexValue &value) const;
};

using Key   = HexValue;
using Value = HexValue;

} // namespace common
