#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "common/trace/types.h"

namespace common {

template <typename Id, typename Hash = HexValueHash> class DenseIdInterner {
public:
  // References passed to intern() must remain valid until this interner is
  // destroyed. Trace parsers satisfy that contract by reserving the complete
  // event array before recording representatives.
  [[nodiscard]] std::optional<Id> intern(const HexValue &value) {
    if (slots_.empty()) {
      rehash(kInitialCapacity);
    }

    const auto hash       = avalanche(Hash{}(value));
    const auto first_slot = findSlot(value, hash);
    if (slots_[first_slot].id != kEmptyId) {
      return slots_[first_slot].id;
    }
    if (representatives_.size() >= std::numeric_limits<Id>::max()) {
      return std::nullopt;
    }

    std::size_t insertion_slot = first_slot;
    if ((representatives_.size() + 1U) * 10U > slots_.size() * 7U) {
      rehash(slots_.size() * 2U);
      insertion_slot = findSlot(value, hash);
    }

    const auto id = static_cast<Id>(representatives_.size());
    representatives_.push_back(&value);
    slots_[insertion_slot] = Slot{hash, id};
    return id;
  }

  [[nodiscard]] std::size_t size() const { return representatives_.size(); }

private:
  static constexpr Id kEmptyId = std::numeric_limits<Id>::max();
  static constexpr std::size_t kInitialCapacity = 16;

  struct Slot {
    std::uint64_t hash = 0;
    Id id              = kEmptyId;
  };

  [[nodiscard]] static std::uint64_t avalanche(std::uint64_t value) {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

  [[nodiscard]] std::size_t findSlot(const HexValue &value,
                                     const std::uint64_t hash) const {
    const auto mask = slots_.size() - 1U;
    auto slot       = hash & mask;
    while (slots_[slot].id != kEmptyId) {
      const auto &entry = slots_[slot];
      if (entry.hash == hash && *representatives_[entry.id] == value) {
        break;
      }
      slot = (slot + 1U) & mask;
    }
    return slot;
  }

  void rehash(const std::size_t capacity) {
    std::vector<Slot> replacement(capacity);
    const auto mask = capacity - 1U;
    for (const auto &entry : slots_) {
      if (entry.id == kEmptyId) {
        continue;
      }
      auto slot = entry.hash & mask;
      while (replacement[slot].id != kEmptyId) {
        slot = (slot + 1U) & mask;
      }
      replacement[slot] = entry;
    }
    slots_ = std::move(replacement);
  }

  std::vector<const HexValue *> representatives_;
  std::vector<Slot> slots_;
};

} // namespace common
