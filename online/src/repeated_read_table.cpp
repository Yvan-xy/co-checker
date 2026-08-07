#include "co_checker/online/repeated_read_table.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono>
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

constexpr std::uint8_t kEmpty                  = 0x80U;
constexpr std::size_t kShrinkRatio             = 8U;
constexpr std::size_t kShrinkGraceTransactions = 64U;

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
  Impl(const RepeatedReadProbeMode requested_mode, RepeatedReadHash hash,
       std::pmr::memory_resource *memory_resource)
      : memory_resource_(memory_resource == nullptr
                             ? std::pmr::get_default_resource()
                             : memory_resource),
        mode_(mode_supported(requested_mode) ? requested_mode
                                             : RepeatedReadProbeMode::Scalar),
        hash_(hash == nullptr ? repeated_read_hash : hash),
        group_width_(mode_ == RepeatedReadProbeMode::AVX2 ? 32U : 16U),
        control_(memory_resource_), slots_(memory_resource_),
        touched_(memory_resource_) {}

  void reset(const std::size_t expected) {
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
    ++reset_calls_;
    reset_expected_events_ += static_cast<std::uint64_t>(expected);
    max_reset_expected_events_ = std::max(max_reset_expected_events_, expected);
#endif
    clear();
    const auto required_capacity = next_capacity(expected, group_width_);
    if (capacity_ < required_capacity) {
      allocate(required_capacity, expected);
      reset_shrink_policy();
      return;
    }

    if (capacity_ > group_width_ &&
        required_capacity <= capacity_ / kShrinkRatio) {
      ++underutilized_resets_;
      shrink_window_max_required_capacity_ =
          std::max(shrink_window_max_required_capacity_, required_capacity);
      shrink_window_max_expected_ =
          std::max(shrink_window_max_expected_, expected);
      if (underutilized_resets_ == kShrinkGraceTransactions) {
        attempt_shrink();
        return;
      }
    } else {
      reset_shrink_policy();
    }
    touched_.reserve(expected);
  }

  void clear() noexcept {
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
    ++clear_calls_;
    clear_slot_visits_ += static_cast<std::uint64_t>(touched_.size());
    clear_occupied_slots_reset_ +=
        static_cast<std::uint64_t>(touched_.size());
#endif
    assert(touched_.size() == size_);
    if (touched_.empty()) {
      assert(size_ == 0);
      return;
    }

    for (const auto index : touched_) {
      assert(index < capacity_);
      assert(control_[index] != kEmpty);
      slots_[index] = Slot{};
      set_control(index, kEmpty);
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
      ++clear_control_bytes_filled_;
      if (index < group_width_) {
        ++clear_control_bytes_filled_;
      }
#endif
    }
    touched_.clear();
    size_ = 0;
  }

  [[nodiscard]] RepeatedReadInsertResult insert(const EventView &read) {
    if (capacity_ == 0) {
      grow();
    }
    const auto hash = hash_(read.key);
    return insert_with_hash(read, hash, true, true);
  }

  [[nodiscard]] RepeatedReadTableMetrics metrics() const noexcept {
    return RepeatedReadTableMetrics{
        .detailed_profiling_enabled = CO_CHECKER_ENABLE_DETAILED_PROFILING != 0,
        .size                       = size_,
        .capacity                   = capacity_,
        .touched_size               = touched_.size(),
        .touched_capacity           = touched_.capacity(),
        .probe_groups               = probe_groups_,
        .key_comparisons            = key_comparisons_,
        .resizes                    = resizes_,
        .shrinks                    = shrinks_,
        .shrink_attempts            = shrink_attempts_,
        .shrink_failures            = shrink_failures_,
        .max_shrink_rebuild_ns      = max_shrink_rebuild_ns_,
        .reset_calls                = reset_calls_,
        .reset_expected_events      = reset_expected_events_,
        .max_reset_expected_events  = max_reset_expected_events_,
        .clear_calls                = clear_calls_,
        .clear_slot_visits          = clear_slot_visits_,
        .clear_control_bytes_filled = clear_control_bytes_filled_,
        .clear_occupied_slots_reset = clear_occupied_slots_reset_,
        .mode                       = mode_,
    };
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
  [[nodiscard]] std::size_t maximum_load() const noexcept {
    return capacity_ - capacity_ / 8U;
  }

  [[nodiscard]] RepeatedReadInsertResult
  insert_with_hash(const EventView &read, const std::uint64_t hash,
                   const bool count_metrics, const bool allow_grow) {
    const auto fingerprint = static_cast<std::uint8_t>((hash >> 57U) & 0x7fU);
    auto start             = static_cast<std::size_t>(hash) & (capacity_ - 1U);
    for (;;) {
      const auto found = masks(start, fingerprint);
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
      if (count_metrics) {
        ++probe_groups_;
      }
#else
      (void)count_metrics;
#endif
      auto candidates = found.candidates;
      while (candidates != 0) {
        const auto lane =
            static_cast<std::size_t>(std::countr_zero(candidates));
        const auto slot_index = (start + lane) & (capacity_ - 1U);
        const auto &slot      = slots_[slot_index];
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
        if (count_metrics) {
          ++key_comparisons_;
        }
#endif
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
        if (allow_grow && size_ == maximum_load()) {
          grow();
          return insert_with_hash(read, hash, count_metrics, true);
        }
        const auto lane =
            static_cast<std::size_t>(std::countr_zero(found.empty));
        const auto slot_index = (start + lane) & (capacity_ - 1U);
        touched_.push_back(slot_index);
        slots_[slot_index] =
            Slot{hash, read.key, read.value_state, read.value, read.sequence};
        set_control(slot_index, fingerprint);
        ++size_;
        assert(touched_.size() == size_);
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

  void swap_storage(Impl &other) noexcept {
    assert(memory_resource_ == other.memory_resource_);
    std::swap(capacity_, other.capacity_);
    std::swap(size_, other.size_);
    control_.swap(other.control_);
    slots_.swap(other.slots_);
    touched_.swap(other.touched_);
  }

  void allocate(const std::size_t capacity,
                const std::size_t touched_reserve) {
    assert(size_ == 0);
    assert(touched_.empty());

    std::pmr::vector<std::uint8_t> new_control(memory_resource_);
    new_control.resize(capacity + group_width_, kEmpty);
    std::pmr::vector<Slot> new_slots(memory_resource_);
    new_slots.resize(capacity);
    std::pmr::vector<std::size_t> new_touched(memory_resource_);
    new_touched.reserve(touched_reserve);

    control_.swap(new_control);
    slots_.swap(new_slots);
    touched_.swap(new_touched);
    capacity_ = capacity;
    size_     = 0;
    ++resizes_;
  }

  void reset_shrink_policy() noexcept {
    underutilized_resets_                = 0;
    shrink_window_max_required_capacity_ = 0;
    shrink_window_max_expected_          = 0;
  }

  void attempt_shrink() noexcept {
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
    ++shrink_attempts_;
    const auto started = std::chrono::steady_clock::now();
#endif
    try {
      Impl replacement(mode_, hash_, memory_resource_);
      replacement.allocate(shrink_window_max_required_capacity_,
                           shrink_window_max_expected_);
      swap_storage(replacement);
      ++resizes_;
      ++shrinks_;
    } catch (const std::bad_alloc &) {
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
      ++shrink_failures_;
#endif
    } catch (const std::length_error &) {
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
      ++shrink_failures_;
#endif
    }
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    max_shrink_rebuild_ns_ =
        std::max(max_shrink_rebuild_ns_, static_cast<std::uint64_t>(elapsed));
#endif
    reset_shrink_policy();
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

    Impl replacement(mode_, hash_, memory_resource_);
    replacement.allocate(new_capacity, size_ + 1U);
    const auto old_size = size_;
    for (const auto index : touched_) {
      assert(index < old_capacity);
      assert(control_[index] != kEmpty);
      const auto &slot = slots_[index];
      const EventView read{slot.first_sequence,  EventOp::Read, slot.state,
                           ReadSource::Snapshot, slot.key,      slot.value};
      const auto inserted =
          replacement.insert_with_hash(read, slot.hash, false, false);
      if (inserted.kind != RepeatedReadInsertKind::Inserted) {
        throw std::logic_error(
            "repeated-read table rehash produced a duplicate");
      }
    }
    if (replacement.size_ != old_size ||
        replacement.touched_.size() != old_size) {
      throw std::logic_error("repeated-read table rehash lost an entry");
    }

    swap_storage(replacement);
    ++resizes_;
    reset_shrink_policy();
    assert(touched_.size() == size_);
  }

  std::pmr::memory_resource *memory_resource_;
  RepeatedReadProbeMode mode_;
  RepeatedReadHash hash_;
  std::size_t group_width_ = 16;
  std::size_t capacity_    = 0;
  std::size_t size_        = 0;
  std::pmr::vector<std::uint8_t> control_;
  std::pmr::vector<Slot> slots_;
  std::pmr::vector<std::size_t> touched_;
  std::uint64_t probe_groups_                      = 0;
  std::uint64_t key_comparisons_                   = 0;
  std::uint64_t resizes_                           = 0;
  std::uint64_t shrinks_                           = 0;
  std::uint64_t shrink_attempts_                   = 0;
  std::uint64_t shrink_failures_                   = 0;
  std::uint64_t max_shrink_rebuild_ns_             = 0;
  std::size_t underutilized_resets_                = 0;
  std::size_t shrink_window_max_required_capacity_ = 0;
  std::size_t shrink_window_max_expected_          = 0;
  std::uint64_t reset_calls_                       = 0;
  std::uint64_t reset_expected_events_             = 0;
  std::size_t max_reset_expected_events_           = 0;
  std::uint64_t clear_calls_                       = 0;
  std::uint64_t clear_slot_visits_                 = 0;
  std::uint64_t clear_control_bytes_filled_        = 0;
  std::uint64_t clear_occupied_slots_reset_        = 0;
};

RepeatedReadTable::RepeatedReadTable(
    const RepeatedReadProbeMode mode, RepeatedReadHash hash,
    std::pmr::memory_resource *memory_resource)
    : impl_(std::make_unique<Impl>(mode, hash, memory_resource)) {}

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
