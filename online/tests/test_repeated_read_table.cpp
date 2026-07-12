#include "co_checker/online/repeated_read_table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
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
    test_randomized_scalar_and_simd_differential();
    test_reset_reuses_scratch_and_drops_borrowed_observations();
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
