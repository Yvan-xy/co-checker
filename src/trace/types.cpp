#include "common/trace/types.h"

#include <algorithm>
#include <limits>

namespace common {

namespace {

[[nodiscard]] int hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

[[nodiscard]] std::uint8_t byte_at_word(const std::uint64_t word,
                                        const std::size_t byte_index) {
  const auto shift = static_cast<unsigned>((7 - byte_index) * 8);
  return static_cast<std::uint8_t>((word >> shift) & 0xffU);
}

void append_byte_hex(std::string &out, const std::uint8_t byte) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back(kHex[(byte >> 4U) & 0x0fU]);
  out.push_back(kHex[byte & 0x0fU]);
}

void hash_combine(std::size_t &seed, const std::uint64_t value) {
  seed ^= std::hash<std::uint64_t>{}(value) + 0x9e3779b97f4a7c15ULL +
          (seed << 6U) + (seed >> 2U);
}

} // namespace

std::optional<HexValue> HexValue::from_hex(std::string_view hex,
                                           std::string *error) {
  if (hex.size() % 2 != 0) {
    if (error != nullptr) {
      *error = "hex string has odd length";
    }
    return std::nullopt;
  }

  for (const char c : hex) {
    if (hex_digit(c) < 0) {
      if (error != nullptr) {
        *error = "hex string contains a non-hex character";
      }
      return std::nullopt;
    }
  }

  const auto byte_len = hex.size() / 2;
  if (byte_len > std::numeric_limits<std::uint32_t>::max()) {
    if (error != nullptr) {
      *error = "hex string is too large";
    }
    return std::nullopt;
  }

  HexValue value;
  value.byte_len = static_cast<std::uint32_t>(byte_len);

  const auto word_count = (byte_len + 7U) / 8U;
  if (word_count > value.inline_words.size()) {
    value.extra_words.assign(word_count - value.inline_words.size(), 0);
  }

  for (std::size_t byte_index = 0; byte_index < byte_len; ++byte_index) {
    const auto high        = hex_digit(hex[byte_index * 2]);
    const auto low         = hex_digit(hex[byte_index * 2 + 1]);
    const auto byte        = static_cast<std::uint64_t>((high << 4) | low);
    const auto word_index  = byte_index / 8U;
    const auto byte_offset = byte_index % 8U;
    const auto shift       = static_cast<unsigned>((7U - byte_offset) * 8U);

    if (word_index < value.inline_words.size()) {
      value.inline_words[word_index] |= byte << shift;
    } else {
      value.extra_words[word_index - value.inline_words.size()] |= byte
                                                                   << shift;
    }
  }

  return value;
}

std::string HexValue::to_hex() const {
  std::string out;
  out.reserve(static_cast<std::size_t>(byte_len) * 2U);

  for (std::size_t byte_index = 0; byte_index < byte_len; ++byte_index) {
    const auto word_index  = byte_index / 8U;
    const auto byte_offset = byte_index % 8U;
    std::uint64_t word     = 0;
    if (word_index < inline_words.size()) {
      word = inline_words[word_index];
    } else {
      const auto extra_index = word_index - inline_words.size();
      if (extra_index < extra_words.size()) {
        word = extra_words[extra_index];
      }
    }
    append_byte_hex(out, byte_at_word(word, byte_offset));
  }

  return out;
}

std::size_t HexValueHash::operator()(const HexValue &value) const {
  std::size_t seed = std::hash<std::uint32_t>{}(value.byte_len);
  for (const auto word : value.inline_words) {
    hash_combine(seed, word);
  }
  for (const auto word : value.extra_words) {
    hash_combine(seed, word);
  }
  return seed;
}

} // namespace common
