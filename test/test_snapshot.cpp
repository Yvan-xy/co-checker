#include "test_harness.h"

#include <optional>
#include <string>

#include "common/trace/snapshot.h"

namespace test {
namespace {

void test_snapshot_copy_helpers_preserve_value_semantics() {
  const auto key_01   = common::HexValue::from_hex("01");
  const auto key_02   = common::HexValue::from_hex("02");
  const auto value_0a = common::HexValue::from_hex("0a");
  const auto value_0b = common::HexValue::from_hex("0b");
  CHECK(key_01.has_value());
  CHECK(key_02.has_value());
  CHECK(value_0a.has_value());
  CHECK(value_0b.has_value());

  common::Snapshot base;
  base.put(*key_01, *value_0a);

  common::Snapshot copied;
  copied.put(*key_02, *value_0b);
  copied.copy_from(base);

  CHECK(copied.get(*key_01) == value_0a);
  CHECK(!copied.get(*key_02).has_value());

  auto cloned = base.clone();
  base.set(*key_01, value_0b);

  CHECK(cloned.get(*key_01) == value_0a);
  CHECK(base.get(*key_01) == value_0b);
  CHECK(cloned.to_string().find("value_hex: 0a") != std::string::npos);
}

void test_snapshot_opt_prints_present_null_values() {
  const auto key_01   = common::HexValue::from_hex("01");
  const auto key_02   = common::HexValue::from_hex("02");
  const auto value_0a = common::HexValue::from_hex("0a");
  CHECK(key_01.has_value());
  CHECK(key_02.has_value());
  CHECK(value_0a.has_value());

  common::SnapshotOpt snapshot;
  snapshot.set(*key_01, value_0a);
  snapshot.set(*key_02, std::nullopt);

  const auto printed = snapshot.to_string();
  CHECK(printed.find("snapshot_opt:") != std::string::npos);
  CHECK(printed.find("key_hex: 01") != std::string::npos);
  CHECK(printed.find("value_hex: 0a") != std::string::npos);
  CHECK(printed.find("key_hex: 02") != std::string::npos);
  CHECK(printed.find("value: null") != std::string::npos);
}

} // namespace

void run_snapshot_tests() {
  test_snapshot_copy_helpers_preserve_value_semantics();
  test_snapshot_opt_prints_present_null_values();
}

} // namespace test
