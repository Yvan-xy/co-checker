#include "co_checker/online/repeated_read_table.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace co_checker::online {
namespace {

constexpr std::uint8_t kEmpty = 0x80U;

struct ProbeMasks {
  std::uint32_t candidates = 0;
  std::uint32_t empty      = 0;
};

[[nodiscard]] ProbeMasks scalar_masks(const std::uint8_t *control,
                                      const std::size_t width,
                                      const std::uint8_t fingerprint) noexcept {
  ProbeMasks result;
  for (std::size_t lane = 0; lane < width; ++lane) {
    const auto bit = std::uint32_t{1} << static_cast<unsigned>(lane);
    if (control[lane] == fingerprint) {
      result.candidates |= bit;
    }
    if (control[lane] == kEmpty) {
      result.empty |= bit;
    }
  }
  return result;
}

#if defined(__x86_64__) || defined(__i386__)
[[nodiscard]] ProbeMasks sse2_masks(const std::uint8_t *control,
                                    const std::uint8_t fingerprint) noexcept {
  const auto values = _mm_loadu_si128(
      reinterpret_cast<const __m128i *>(static_cast<const void *>(control)));
  const auto wanted = _mm_set1_epi8(static_cast<char>(fingerprint));
  const auto empty  = _mm_set1_epi8(static_cast<char>(kEmpty));
  return ProbeMasks{static_cast<std::uint32_t>(
                        _mm_movemask_epi8(_mm_cmpeq_epi8(values, wanted))),
                    static_cast<std::uint32_t>(
                        _mm_movemask_epi8(_mm_cmpeq_epi8(values, empty)))};
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
ProbeMasks
avx2_masks(const std::uint8_t *control,
           const std::uint8_t fingerprint) noexcept {
  const auto values = _mm256_loadu_si256(
      reinterpret_cast<const __m256i *>(static_cast<const void *>(control)));
  const auto wanted = _mm256_set1_epi8(static_cast<char>(fingerprint));
  const auto empty  = _mm256_set1_epi8(static_cast<char>(kEmpty));
  return ProbeMasks{static_cast<std::uint32_t>(_mm256_movemask_epi8(
                        _mm256_cmpeq_epi8(values, wanted))),
                    static_cast<std::uint32_t>(_mm256_movemask_epi8(
                        _mm256_cmpeq_epi8(values, empty)))};
}
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
[[nodiscard]] std::uint32_t neon_movemask(const uint8x16_t bytes) noexcept {
  alignas(16) std::uint8_t lanes[16];
  vst1q_u8(lanes, bytes);
  std::uint32_t result = 0;
  for (std::size_t lane = 0; lane < 16; ++lane) {
    if (lanes[lane] != 0) {
      result |= std::uint32_t{1} << static_cast<unsigned>(lane);
    }
  }
  return result;
}

[[nodiscard]] ProbeMasks neon_masks(const std::uint8_t *control,
                                    const std::uint8_t fingerprint) noexcept {
  const auto values = vld1q_u8(control);
  return ProbeMasks{neon_movemask(vceqq_u8(values, vdupq_n_u8(fingerprint))),
                    neon_movemask(vceqq_u8(values, vdupq_n_u8(kEmpty)))};
}
#endif

[[nodiscard]] bool bytes_equal(const ByteView lhs,
                               const ByteView rhs) noexcept {
  return lhs.size() == rhs.size() &&
         (lhs.empty() || std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0);
}

[[nodiscard]] bool result_equal(const ValueState lhs_state,
                                const ByteView lhs_value,
                                const ValueState rhs_state,
                                const ByteView rhs_value) noexcept {
  if (lhs_state != rhs_state) {
    return false;
  }
  return lhs_state == ValueState::Absent || bytes_equal(lhs_value, rhs_value);
}

[[nodiscard]] std::size_t next_capacity(const std::size_t expected,
                                        const std::size_t group_width) {
  // The table keeps its load at or below 7/8 and capacities are powers of
  // two. Compute ceil(expected * 8 / 7) without overflowing the
  // multiplication, and reject a request whose next power of two would not be
  // representable. std::bit_ceil has a representability precondition, so do
  // not delegate this check to it.
  constexpr auto kHighestPowerOfTwo =
      std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1U);
  constexpr auto kMaximumExpected = (kHighestPowerOfTwo / 8U) * 7U;
  if (expected > kMaximumExpected) {
    throw std::length_error("repeated-read table capacity overflow");
  }
  const auto quotient  = expected / 7U;
  const auto remainder = expected % 7U;
  const auto required  = quotient * 8U + (remainder * 8U + 6U) / 7U;
  const auto minimum   = std::max(group_width, required);
  return std::bit_ceil(minimum);
}

[[nodiscard]] bool mode_supported(const RepeatedReadProbeMode mode) noexcept {
  switch (mode) {
  case RepeatedReadProbeMode::Scalar:
    return true;
  case RepeatedReadProbeMode::SSE2:
#if defined(__x86_64__)
    return true;
#elif defined(__i386__) && (defined(__GNUC__) || defined(__clang__))
    return __builtin_cpu_supports("sse2");
#else
    return false;
#endif
  case RepeatedReadProbeMode::AVX2:
#if (defined(__x86_64__) || defined(__i386__)) &&                              \
    (defined(__GNUC__) || defined(__clang__))
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
  case RepeatedReadProbeMode::NEON:
#if defined(__aarch64__) || defined(__ARM_NEON)
    return true;
#else
    return false;
#endif
  }
  return false;
}

} // namespace

std::uint64_t repeated_read_hash(const ByteView bytes) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const auto byte : bytes) {
    hash ^= std::to_integer<std::uint8_t>(byte);
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::vector<RepeatedReadProbeMode> supported_repeated_read_probe_modes() {
  std::vector<RepeatedReadProbeMode> result{RepeatedReadProbeMode::Scalar};
  for (const auto mode :
       {RepeatedReadProbeMode::SSE2, RepeatedReadProbeMode::AVX2,
        RepeatedReadProbeMode::NEON}) {
    if (mode_supported(mode)) {
      result.push_back(mode);
    }
  }
  return result;
}

RepeatedReadProbeMode default_repeated_read_probe_mode() noexcept {
  if (mode_supported(RepeatedReadProbeMode::SSE2)) {
    return RepeatedReadProbeMode::SSE2;
  }
  if (mode_supported(RepeatedReadProbeMode::AVX2)) {
    return RepeatedReadProbeMode::AVX2;
  }
  if (mode_supported(RepeatedReadProbeMode::NEON)) {
    return RepeatedReadProbeMode::NEON;
  }
  return RepeatedReadProbeMode::Scalar;
}

class RepeatedReadTable::Impl {
public:
  Impl(const RepeatedReadProbeMode requested_mode, RepeatedReadHash hash)
      : mode_(mode_supported(requested_mode) ? requested_mode
                                             : RepeatedReadProbeMode::Scalar),
        hash_(hash == nullptr ? repeated_read_hash : hash),
        group_width_(mode_ == RepeatedReadProbeMode::AVX2 ? 32U : 16U) {}

  void reset(const std::size_t expected) {
    clear();
    const auto required_capacity = next_capacity(expected, group_width_);
    if (capacity_ < required_capacity) {
      allocate(required_capacity);
    }
  }

  void clear() noexcept {
    for (std::size_t index = 0; index < capacity_; ++index) {
      if (control_[index] != kEmpty) {
        slots_[index] = Slot{};
      }
    }
    std::fill(control_.begin(), control_.end(), kEmpty);
    size_ = 0;
  }

  [[nodiscard]] RepeatedReadInsertResult insert(const EventView &read) {
    if (capacity_ == 0 || size_ >= capacity_ - capacity_ / 8U) {
      grow();
    }
    const auto hash = hash_(read.key);
    return insert_with_hash(read, hash, true);
  }

  [[nodiscard]] RepeatedReadTableMetrics metrics() const noexcept {
    return RepeatedReadTableMetrics{
        size_, capacity_, probe_groups_, key_comparisons_, resizes_, mode_};
  }

private:
  struct Slot {
    std::uint64_t hash = 0;
    ByteView key;
    ValueState state = ValueState::Absent;
    ByteView value;
    EventSeq first_sequence = 0;
  };

  [[nodiscard]] ProbeMasks
  masks(const std::size_t start,
        const std::uint8_t fingerprint) const noexcept {
    const auto *bytes = control_.data() + start;
    switch (mode_) {
    case RepeatedReadProbeMode::AVX2:
#if defined(__x86_64__) || defined(__i386__)
      return avx2_masks(bytes, fingerprint);
#else
      break;
#endif
    case RepeatedReadProbeMode::SSE2:
#if defined(__x86_64__) || defined(__i386__)
      return sse2_masks(bytes, fingerprint);
#else
      break;
#endif
    case RepeatedReadProbeMode::NEON:
#if defined(__aarch64__) || defined(__ARM_NEON)
      return neon_masks(bytes, fingerprint);
#else
      break;
#endif
    case RepeatedReadProbeMode::Scalar:
      break;
    }
    return scalar_masks(bytes, group_width_, fingerprint);
  }

  [[nodiscard]] RepeatedReadInsertResult
  insert_with_hash(const EventView &read, const std::uint64_t hash,
                   const bool count_metrics) {
    const auto fingerprint = static_cast<std::uint8_t>((hash >> 57U) & 0x7fU);
    auto start             = static_cast<std::size_t>(hash) & (capacity_ - 1U);
    for (;;) {
      const auto found = masks(start, fingerprint);
      if (count_metrics) {
        ++probe_groups_;
      }
      auto candidates = found.candidates;
      while (candidates != 0) {
        const auto lane =
            static_cast<std::size_t>(std::countr_zero(candidates));
        const auto slot_index = (start + lane) & (capacity_ - 1U);
        const auto &slot      = slots_[slot_index];
        if (count_metrics) {
          ++key_comparisons_;
        }
        if (slot.hash == hash && bytes_equal(slot.key, read.key)) {
          return RepeatedReadInsertResult{
              result_equal(slot.state, slot.value, read.value_state, read.value)
                  ? RepeatedReadInsertKind::SameResult
                  : RepeatedReadInsertKind::ConflictingResult,
              slot.first_sequence};
        }
        candidates &= candidates - 1U;
      }
      if (found.empty != 0) {
        const auto lane =
            static_cast<std::size_t>(std::countr_zero(found.empty));
        const auto slot_index = (start + lane) & (capacity_ - 1U);
        slots_[slot_index] =
            Slot{hash, read.key, read.value_state, read.value, read.sequence};
        set_control(slot_index, fingerprint);
        ++size_;
        return RepeatedReadInsertResult{RepeatedReadInsertKind::Inserted,
                                        read.sequence};
      }
      start = (start + group_width_) & (capacity_ - 1U);
    }
  }

  void set_control(const std::size_t index, const std::uint8_t value) noexcept {
    control_[index] = value;
    if (index < group_width_) {
      control_[capacity_ + index] = value;
    }
  }

  void allocate(const std::size_t capacity) {
    // Allocate both arrays before changing any member. In particular, an
    // allocation failure must not leave capacity_ describing larger arrays
    // than control_/slots_; clear() is called by the engine's unwind guard.
    std::vector<std::uint8_t> new_control(capacity + group_width_, kEmpty);
    std::vector<Slot> new_slots(capacity);
    control_  = std::move(new_control);
    slots_    = std::move(new_slots);
    capacity_ = capacity;
    size_     = 0;
    ++resizes_;
  }

  void grow() {
    const auto old_capacity = capacity_;
    constexpr auto kHighestPowerOfTwo =
        std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1U);
    if (old_capacity >= kHighestPowerOfTwo) {
      throw std::length_error("repeated-read table capacity overflow");
    }
    const auto new_capacity =
        old_capacity == 0 ? group_width_ : old_capacity * 2U;

    // Establish the potentially throwing allocations before moving the old
    // table out. This gives grow() the strong guarantee for allocation
    // failures and keeps borrowed slots clearable during stack unwinding.
    std::vector<std::uint8_t> new_control(new_capacity + group_width_, kEmpty);
    std::vector<Slot> new_slots(new_capacity);
    auto old_control    = std::move(control_);
    auto old_slots      = std::move(slots_);
    const auto old_size = size_;
    control_            = std::move(new_control);
    slots_              = std::move(new_slots);
    capacity_           = new_capacity;
    size_               = 0;
    ++resizes_;
    for (std::size_t index = 0; index < old_capacity; ++index) {
      if (old_control[index] == kEmpty) {
        continue;
      }
      const auto &slot = old_slots[index];
      const EventView read{slot.first_sequence,  EventOp::Read, slot.state,
                           ReadSource::Snapshot, slot.key,      slot.value};
      (void)insert_with_hash(read, slot.hash, false);
    }
    if (size_ != old_size) {
      throw std::logic_error("repeated-read table rehash lost an entry");
    }
  }

  RepeatedReadProbeMode mode_;
  RepeatedReadHash hash_;
  std::size_t group_width_ = 16;
  std::size_t capacity_    = 0;
  std::size_t size_        = 0;
  std::vector<std::uint8_t> control_;
  std::vector<Slot> slots_;
  std::uint64_t probe_groups_    = 0;
  std::uint64_t key_comparisons_ = 0;
  std::uint64_t resizes_         = 0;
};

RepeatedReadTable::RepeatedReadTable(const RepeatedReadProbeMode mode,
                                     RepeatedReadHash hash)
    : impl_(std::make_unique<Impl>(mode, hash)) {}

RepeatedReadTable::~RepeatedReadTable()                             = default;
RepeatedReadTable::RepeatedReadTable(RepeatedReadTable &&) noexcept = default;
RepeatedReadTable &
RepeatedReadTable::operator=(RepeatedReadTable &&) noexcept = default;

void RepeatedReadTable::reset(const std::size_t expected) {
  impl_->reset(expected);
}

void RepeatedReadTable::clear() noexcept { impl_->clear(); }

RepeatedReadInsertResult RepeatedReadTable::insert(const EventView &read) {
  return impl_->insert(read);
}

RepeatedReadTableMetrics RepeatedReadTable::metrics() const noexcept {
  return impl_->metrics();
}

} // namespace co_checker::online
