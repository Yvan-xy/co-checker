#include "co_checker/online/repeated_read_table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace co_checker::online;

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void check(const bool condition, const std::string_view expression,
           const char *file, const int line) {
  if (condition) {
    return;
  }
  std::ostringstream message;
  message << file << ':' << line << ": check failed: " << expression;
  throw TestFailure(message.str());
}

#define CHECK(expression)                                                      \
  check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value) {
  std::vector<std::byte> result(value.size());
  if (!value.empty()) {
    std::memcpy(result.data(), value.data(), value.size());
  }
  return result;
}

struct OwnedRead {
  EventSeq sequence = 0;
  ValueState state  = ValueState::Absent;
  std::vector<std::byte> key;
  std::vector<std::byte> value;

  [[nodiscard]] EventView view() const noexcept {
    return EventView{sequence,      EventOp::Read,  state, ReadSource::Snapshot,
                     ByteView(key), ByteView(value)};
  }
};

class FailingMemoryResource final : public std::pmr::memory_resource {
public:
  void fail_after(const std::size_t successful_allocations) noexcept {
    allocations_before_failure_ = successful_allocations;
    failure_enabled_            = true;
  }

  void disable_failure() noexcept { failure_enabled_ = false; }

private:
  void *do_allocate(const std::size_t bytes, const std::size_t alignment) override {
    if (failure_enabled_) {
      if (allocations_before_failure_ == 0) {
        throw std::bad_alloc();
      }
      --allocations_before_failure_;
    }
    return upstream_->allocate(bytes, alignment);
  }

  void do_deallocate(void *pointer, const std::size_t bytes,
                     const std::size_t alignment) override {
    upstream_->deallocate(pointer, bytes, alignment);
  }

  [[nodiscard]] bool
  do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    return this == &other;
  }

  std::pmr::memory_resource *upstream_ = std::pmr::get_default_resource();
  std::size_t allocations_before_failure_ = 0;
  bool failure_enabled_                    = false;
};
[[nodiscard]] std::uint64_t constant_hash(ByteView) noexcept { return 7; }

void test_tagged_equality_for_every_enabled_mode() {
  for (const auto mode : supported_repeated_read_probe_modes()) {
    RepeatedReadTable table(mode);
    table.reset(4);
    OwnedRead absent{1, ValueState::Absent, bytes("k"), {}};
    OwnedRead same_absent{2, ValueState::Absent, bytes("k"), {}};
    OwnedRead present_empty{3, ValueState::Present, bytes("k"), {}};

    CHECK(table.insert(absent.view()).kind == RepeatedReadInsertKind::Inserted);
    const auto same = table.insert(same_absent.view());
    CHECK(same.kind == RepeatedReadInsertKind::SameResult);
    CHECK(same.first_sequence == 1U);
    const auto conflict = table.insert(present_empty.view());
    CHECK(conflict.kind == RepeatedReadInsertKind::ConflictingResult);
    CHECK(conflict.first_sequence == 1U);
    CHECK(table.metrics().mode == mode);
  }
}

void test_constant_hash_collision_wraparound_and_growth() {
  RepeatedReadTable table(RepeatedReadProbeMode::Scalar, constant_hash);
  table.reset(1);
  std::vector<OwnedRead> reads;
  reads.reserve(700);
  for (std::uint32_t index = 0; index < 700; ++index) {
    reads.push_back(OwnedRead{index + 1U, ValueState::Present,
                              bytes("key-" + std::to_string(index)),
                              bytes("value-" + std::to_string(index))});
    CHECK(table.insert(reads.back().view()).kind ==
          RepeatedReadInsertKind::Inserted);
  }
  CHECK(table.metrics().size == reads.size());
  CHECK(table.metrics().resizes > 1U);

  for (const auto &read : reads) {
    const auto duplicate = table.insert(read.view());
    CHECK(duplicate.kind == RepeatedReadInsertKind::SameResult);
    CHECK(duplicate.first_sequence == read.sequence);
  }

  auto conflict     = reads[333];
  conflict.sequence = 1000;
  conflict.value    = bytes("different");
  const auto result = table.insert(conflict.view());
  CHECK(result.kind == RepeatedReadInsertKind::ConflictingResult);
  CHECK(result.first_sequence == reads[333].sequence);
}

struct ReferenceObservation {
  ValueState state = ValueState::Absent;
  std::string value;
  EventSeq first_sequence = 0;
};

[[nodiscard]] std::string string_bytes(const std::vector<std::byte> &value) {
  return std::string(reinterpret_cast<const char *>(value.data()),
                     value.size());
}

void test_randomized_scalar_and_simd_differential() {
  std::mt19937_64 random(0x5eeda11ULL);
  const auto modes = supported_repeated_read_probe_modes();
  for (std::size_t round = 0; round < 220; ++round) {
    std::vector<RepeatedReadTable> tables;
    for (const auto mode : modes) {
      tables.emplace_back(mode);
      tables.back().reset(round % 11U);
    }
    std::unordered_map<std::string, ReferenceObservation> reference;
    std::vector<OwnedRead> lifetime;
    lifetime.reserve(320);

    const auto count = 40U + static_cast<unsigned>(random() % 280U);
    for (unsigned index = 0; index < count; ++index) {
      const auto key_id   = random() % 79U;
      const auto present  = (random() & 3U) != 0;
      const auto value_id = random() % 23U;
      lifetime.push_back(OwnedRead{
          static_cast<EventSeq>(index + 1U),
          present ? ValueState::Present : ValueState::Absent,
          bytes("k" + std::to_string(key_id)),
          present ? bytes((random() & 7U) == 0 ? std::string{}
                                               : "v" + std::to_string(value_id))
                  : std::vector<std::byte>{},
      });
      const auto &read = lifetime.back();
      const auto key   = string_bytes(read.key);
      const auto value = string_bytes(read.value);
      RepeatedReadInsertResult expected;
      const auto found = reference.find(key);
      if (found == reference.end()) {
        reference.emplace(
            key, ReferenceObservation{read.state, value, read.sequence});
        expected = {RepeatedReadInsertKind::Inserted, read.sequence};
      } else {
        const auto same =
            found->second.state == read.state &&
            (read.state == ValueState::Absent || found->second.value == value);
        expected = {same ? RepeatedReadInsertKind::SameResult
                         : RepeatedReadInsertKind::ConflictingResult,
                    found->second.first_sequence};
      }
      for (auto &table : tables) {
        const auto actual = table.insert(read.view());
        CHECK(actual.kind == expected.kind);
        CHECK(actual.first_sequence == expected.first_sequence);
      }
    }
    for (const auto &table : tables) {
      CHECK(table.metrics().size == reference.size());
      CHECK(table.metrics().capacity >= table.metrics().size);
    }
  }
}

void test_reset_reuses_scratch_and_drops_borrowed_observations() {
  RepeatedReadTable table;
  table.reset(500);
  const auto original_capacity = table.metrics().capacity;
  {
    OwnedRead read{1, ValueState::Present, bytes("old"), bytes("value")};
    CHECK(table.insert(read.view()).kind == RepeatedReadInsertKind::Inserted);
  }
  table.reset(2);
  CHECK(table.metrics().size == 0U);
  CHECK(table.metrics().capacity == original_capacity);
  OwnedRead replacement{1, ValueState::Absent, bytes("old"), {}};
  CHECK(table.insert(replacement.view()).kind ==
        RepeatedReadInsertKind::Inserted);
}

void test_reset_and_clear_work_metrics_are_cumulative() {
  RepeatedReadTable table(RepeatedReadProbeMode::Scalar);
  table.reset(20);
  const auto capacity = table.metrics().capacity;
  OwnedRead first{1, ValueState::Absent, bytes("first"), {}};
  OwnedRead second{2, ValueState::Present, bytes("second"), bytes("value")};
  CHECK(table.insert(first.view()).kind == RepeatedReadInsertKind::Inserted);
  CHECK(table.insert(second.view()).kind == RepeatedReadInsertKind::Inserted);

  table.clear();
  const auto after_clear = table.metrics();
  CHECK(after_clear.detailed_profiling_enabled ==
        (CO_CHECKER_ENABLE_DETAILED_PROFILING != 0));
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
  CHECK(after_clear.reset_calls == 1U);
  CHECK(after_clear.reset_expected_events == 20U);
  CHECK(after_clear.max_reset_expected_events == 20U);
  CHECK(after_clear.clear_calls == 2U);
  CHECK(after_clear.clear_slot_visits == 2U);
  CHECK(after_clear.clear_control_bytes_filled >= 2U);
  CHECK(after_clear.clear_control_bytes_filled <= 4U);
  CHECK(after_clear.clear_occupied_slots_reset == 2U);
#else
  CHECK(after_clear.reset_calls == 0U);
  CHECK(after_clear.clear_calls == 0U);
  CHECK(after_clear.clear_slot_visits == 0U);
  CHECK(after_clear.clear_occupied_slots_reset == 0U);
#endif

  table.reset(2);
  const auto after_reset = table.metrics();
  CHECK(after_reset.capacity == capacity);
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
  CHECK(after_reset.reset_calls == 2U);
  CHECK(after_reset.reset_expected_events == 22U);
  CHECK(after_reset.max_reset_expected_events == 20U);
  CHECK(after_reset.clear_calls == 3U);
  CHECK(after_reset.clear_slot_visits == 2U);
  CHECK(after_reset.clear_occupied_slots_reset == 2U);
#else
  CHECK(after_reset.reset_calls == 0U);
  CHECK(after_reset.clear_calls == 0U);
  CHECK(after_reset.clear_slot_visits == 0U);
  CHECK(after_reset.clear_occupied_slots_reset == 0U);
#endif
}

void test_duplicate_at_load_threshold_does_not_grow() {
  for (const auto mode : supported_repeated_read_probe_modes()) {
    RepeatedReadTable table(mode, constant_hash);
    table.reset(1);
    const auto capacity    = table.metrics().capacity;
    const auto maximum_load = capacity - capacity / 8U;
    std::vector<OwnedRead> reads;
    reads.reserve(maximum_load);
    for (std::size_t index = 0; index < maximum_load; ++index) {
      reads.push_back(OwnedRead{
          static_cast<EventSeq>(index + 1U),
          ValueState::Present,
          bytes("threshold-" + std::to_string(index)),
          bytes("value-" + std::to_string(index)),
      });
      CHECK(table.insert(reads.back().view()).kind ==
            RepeatedReadInsertKind::Inserted);
    }

    const auto duplicate = table.insert(reads.front().view());
    CHECK(duplicate.kind == RepeatedReadInsertKind::SameResult);
    CHECK(duplicate.first_sequence == reads.front().sequence);
    CHECK(table.metrics().capacity == capacity);
    table.clear();
  }
}

#if CO_CHECKER_ENABLE_DETAILED_PROFILING
void test_post_outlier_cleanup_work_tracks_occupied_slots() {
  RepeatedReadTable table(RepeatedReadProbeMode::Scalar);
  table.reset(2'400'331U);
  CHECK(table.metrics().capacity == 4'194'304U);
  table.clear();

  const auto before = table.metrics();
  table.reset(1);
  OwnedRead read{1, ValueState::Present, bytes("key"), bytes("value")};
  CHECK(table.insert(read.view()).kind == RepeatedReadInsertKind::Inserted);
  table.clear();
  table.clear();

  const auto after = table.metrics();
  CHECK(after.clear_calls - before.clear_calls == 3U);
  CHECK(after.clear_slot_visits - before.clear_slot_visits == 1U);
  const auto control_writes =
      after.clear_control_bytes_filled - before.clear_control_bytes_filled;
  CHECK(control_writes >= 1U);
  CHECK(control_writes <= 2U);
  CHECK(after.clear_occupied_slots_reset -
            before.clear_occupied_slots_reset ==
        1U);
}
#endif
void test_touched_append_failure_publishes_no_borrowed_view() {
  FailingMemoryResource resource;
  RepeatedReadTable table(RepeatedReadProbeMode::Scalar, repeated_read_hash,
                          &resource);
  table.reset(0);
  OwnedRead read{1, ValueState::Present, bytes("key"), bytes("value")};

  resource.fail_after(0);
  bool rejected = false;
  try {
    (void)table.insert(read.view());
  } catch (const std::bad_alloc &) {
    rejected = true;
  }
  CHECK(rejected);
  CHECK(table.metrics().capacity == 16U);
  CHECK(table.metrics().size == 0U);
  CHECK(table.metrics().touched_size == 0U);

  resource.disable_failure();
  CHECK(table.insert(read.view()).kind == RepeatedReadInsertKind::Inserted);
  table.clear();
  CHECK(table.metrics().size == 0U);
  CHECK(table.metrics().touched_size == 0U);
}

void test_empty_rebuild_failures_leave_old_layout_reusable() {
  for (std::size_t failure_point = 0; failure_point < 3U; ++failure_point) {
    FailingMemoryResource resource;
    RepeatedReadTable table(RepeatedReadProbeMode::Scalar, repeated_read_hash,
                            &resource);
    table.reset(4);
    const auto original_capacity = table.metrics().capacity;

    resource.fail_after(failure_point);
    bool rejected = false;
    try {
      table.reset(100);
    } catch (const std::bad_alloc &) {
      rejected = true;
    }
    CHECK(rejected);
    CHECK(table.metrics().capacity == original_capacity);
    CHECK(table.metrics().size == 0U);
    CHECK(table.metrics().touched_size == 0U);

    resource.disable_failure();
    OwnedRead replacement{
        1, ValueState::Absent,
        bytes("replacement-" + std::to_string(failure_point)), {}};
    CHECK(table.insert(replacement.view()).kind ==
          RepeatedReadInsertKind::Inserted);
    table.clear();
  }
}

void test_active_growth_failures_leave_old_table_clearable() {
  for (std::size_t failure_point = 0; failure_point < 3U; ++failure_point) {
    FailingMemoryResource resource;
    RepeatedReadTable table(RepeatedReadProbeMode::Scalar, repeated_read_hash,
                            &resource);
    table.reset(14);
    std::vector<OwnedRead> reads;
    reads.reserve(15);
    for (std::size_t index = 0; index < 14U; ++index) {
      reads.push_back(OwnedRead{
          static_cast<EventSeq>(index + 1U), ValueState::Present,
          bytes("grow-" + std::to_string(index)),
          bytes("value-" + std::to_string(index)),
      });
      CHECK(table.insert(reads.back().view()).kind ==
            RepeatedReadInsertKind::Inserted);
    }
    CHECK(table.metrics().capacity == 16U);
    CHECK(table.metrics().size == 14U);
    CHECK(table.metrics().touched_size == 14U);

    reads.push_back(OwnedRead{15, ValueState::Absent, bytes("grow-new"), {}});
    resource.fail_after(failure_point);
    bool rejected = false;
    try {
      (void)table.insert(reads.back().view());
    } catch (const std::bad_alloc &) {
      rejected = true;
    }
    CHECK(rejected);
    CHECK(table.metrics().capacity == 16U);
    CHECK(table.metrics().size == 14U);
    CHECK(table.metrics().touched_size == 14U);

    resource.disable_failure();
    const auto duplicate = table.insert(reads.front().view());
    CHECK(duplicate.kind == RepeatedReadInsertKind::SameResult);
    CHECK(duplicate.first_sequence == reads.front().sequence);
    table.clear();
    CHECK(table.metrics().size == 0U);
    CHECK(table.metrics().touched_size == 0U);
  }
}
void test_sustained_small_demand_shrinks_once() {
  RepeatedReadTable table(RepeatedReadProbeMode::Scalar);
  table.reset(1'000U);
  const auto retained = table.metrics();
  CHECK(retained.capacity == 2'048U);
  CHECK(retained.touched_capacity >= 1'000U);

  for (std::size_t index = 0; index < 63U; ++index) {
    table.reset(23U);
    CHECK(table.metrics().capacity == retained.capacity);
  }
  table.reset(23U);

  const auto shrunk = table.metrics();
  CHECK(shrunk.capacity == 32U);
  CHECK(shrunk.touched_capacity >= 23U);
  CHECK(shrunk.touched_capacity < retained.touched_capacity);
  CHECK(shrunk.shrinks == retained.shrinks + 1U);
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
  CHECK(shrunk.shrink_attempts == retained.shrink_attempts + 1U);
  CHECK(shrunk.shrink_failures == retained.shrink_failures);
  CHECK(shrunk.max_shrink_rebuild_ns > 0U);
#else
  CHECK(shrunk.shrink_attempts == 0U);
  CHECK(shrunk.shrink_failures == 0U);
  CHECK(shrunk.max_shrink_rebuild_ns == 0U);
#endif

  for (std::size_t index = 0; index < 128U; ++index) {
    table.reset(23U);
  }
  CHECK(table.metrics().capacity == 32U);
  CHECK(table.metrics().shrinks == shrunk.shrinks);
}

void test_alternating_demand_resets_shrink_streak() {
  RepeatedReadTable table(RepeatedReadProbeMode::Scalar);
  table.reset(1'000U);
  const auto retained = table.metrics();

  for (std::size_t cycle = 0; cycle < 4U; ++cycle) {
    for (std::size_t index = 0; index < 63U; ++index) {
      table.reset(23U);
    }
    table.reset(300U);
  }

  const auto after = table.metrics();
  CHECK(after.capacity == retained.capacity);
  CHECK(after.shrinks == retained.shrinks);
}

void test_optional_shrink_failures_leave_old_layout_reusable() {
  for (std::size_t failure_point = 0; failure_point < 3U; ++failure_point) {
    FailingMemoryResource resource;
    RepeatedReadTable table(RepeatedReadProbeMode::Scalar, repeated_read_hash,
                            &resource);
    table.reset(1'000U);
    const auto retained = table.metrics();
    for (std::size_t index = 0; index < 63U; ++index) {
      table.reset(23U);
    }

    resource.fail_after(failure_point);
    table.reset(23U);
    const auto after_failure = table.metrics();
    CHECK(after_failure.capacity == retained.capacity);
    CHECK(after_failure.size == 0U);
    CHECK(after_failure.touched_size == 0U);
    CHECK(after_failure.shrinks == retained.shrinks);
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
    CHECK(after_failure.shrink_attempts == retained.shrink_attempts + 1U);
    CHECK(after_failure.shrink_failures == retained.shrink_failures + 1U);
#else
    CHECK(after_failure.shrink_attempts == 0U);
    CHECK(after_failure.shrink_failures == 0U);
#endif

    resource.disable_failure();
    OwnedRead read{1, ValueState::Present, bytes("after-shrink-failure"),
                   bytes("value")};
    CHECK(table.insert(read.view()).kind == RepeatedReadInsertKind::Inserted);
    table.clear();
    CHECK(table.metrics().size == 0U);
  }
}

void test_capacity_overflow_leaves_table_clearable_and_reusable() {
  RepeatedReadTable table(RepeatedReadProbeMode::Scalar);
  table.reset(4);
  OwnedRead original{1, ValueState::Present, bytes("old"), bytes("value")};
  CHECK(table.insert(original.view()).kind == RepeatedReadInsertKind::Inserted);

  bool rejected = false;
  try {
    table.reset(std::numeric_limits<std::size_t>::max());
  } catch (const std::length_error &) {
    rejected = true;
  }
  CHECK(rejected);
  CHECK(table.metrics().size == 0U);

  OwnedRead replacement{2, ValueState::Absent, bytes("new"), {}};
  CHECK(table.insert(replacement.view()).kind ==
        RepeatedReadInsertKind::Inserted);
  table.clear();
  CHECK(table.metrics().size == 0U);
}

} // namespace

int run_repeated_read_table_tests() {
  try {
    test_tagged_equality_for_every_enabled_mode();
    test_constant_hash_collision_wraparound_and_growth();
#if CO_CHECKER_ENABLE_DETAILED_PROFILING
    test_post_outlier_cleanup_work_tracks_occupied_slots();
#endif
    test_duplicate_at_load_threshold_does_not_grow();
    test_randomized_scalar_and_simd_differential();
    test_reset_reuses_scratch_and_drops_borrowed_observations();
    test_reset_and_clear_work_metrics_are_cumulative();
    test_touched_append_failure_publishes_no_borrowed_view();
    test_empty_rebuild_failures_leave_old_layout_reusable();
    test_active_growth_failures_leave_old_table_clearable();
    test_sustained_small_demand_shrinks_once();
    test_alternating_demand_resets_shrink_streak();
    test_optional_shrink_failures_leave_old_layout_reusable();
    test_capacity_overflow_leaves_table_clearable_and_reusable();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "repeated-read table tests failed: " << error.what() << '\n';
    return 1;
  }
}

#ifdef CO_CHECKER_REPEATED_READ_STANDALONE_TEST_MAIN
int main() { return run_repeated_read_table_tests(); }
#endif
