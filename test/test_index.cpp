#include "test_harness.h"

namespace test {
namespace {

void test_index_keeps_last_write_per_key_per_txn() {
  auto trace = parse_or_die(R"yaml(
---
session_id: 1
start_ts: 10
commit_ts: 20
events:
  - seq: 1
    op: W
    key_hex: 01
    value_hex: 0a
  - seq: 2
    op: W
    key_hex: 02
    value_hex: 0c
  - seq: 3
    op: W
    key_hex: 01
    value_hex: 0b
---
session_id: 1
start_ts: 21
commit_ts: 30
events:
  - seq: 1
    op: W
    key_hex: 01
    value_hex: 0d
)yaml");

  const auto index = build_index_or_die(trace);
  auto key_01      = common::HexValue::from_hex("01");
  auto key_02      = common::HexValue::from_hex("02");
  CHECK(key_01.has_value());
  CHECK(key_02.has_value());

  const auto versions_01 = index.versions_by_key.find(*key_01);
  CHECK(versions_01 != index.versions_by_key.end());
  CHECK(versions_01->second.size() == 2);
  CHECK(versions_01->second[0].write_event == 2);
  CHECK(versions_01->second[0].value.has_value());
  CHECK(versions_01->second[0].value->to_hex() == "0b");
  CHECK(versions_01->second[1].write_event == 3);
  CHECK(versions_01->second[1].value.has_value());
  CHECK(versions_01->second[1].value->to_hex() == "0d");

  const auto versions_02 = index.versions_by_key.find(*key_02);
  CHECK(versions_02 != index.versions_by_key.end());
  CHECK(versions_02->second.size() == 1);
  CHECK(versions_02->second[0].write_event == 1);
}

void test_index_keeps_final_delete_per_key_per_txn() {
  auto trace = parse_or_die(R"yaml(
---
session_id: 1
start_ts: 10
commit_ts: 20
events:
  - seq: 1
    op: W
    key_hex: 01
    value_hex: 0a
---
session_id: 1
start_ts: 21
commit_ts: 30
events:
  - seq: 1
    op: W
    key_hex: 01
    value_hex: 0b
  - seq: 2
    op: W
    key_hex: 01
    delete: true
---
session_id: 1
start_ts: 31
commit_ts: 40
events:
  - seq: 1
    op: R
    key_hex: 01
    miss: true
)yaml");

  const auto index = build_index_or_die(trace);
  auto key         = common::HexValue::from_hex("01");
  CHECK(key.has_value());
  const auto versions = index.versions_by_key.find(*key);
  CHECK(versions != index.versions_by_key.end());
  CHECK(versions->second.size() == 2);
  CHECK(versions->second[1].write_event == 2);
  CHECK(versions->second[1].is_delete);

  auto result = run_basic_checker(trace);
  CHECK(result.status == checker::CheckStatus::Pass);
}

void test_index_keeps_final_value_after_delete_per_key_per_txn() {
  auto trace = parse_or_die(R"yaml(
---
session_id: 1
start_ts: 10
commit_ts: 20
events:
  - seq: 1
    op: W
    key_hex: 01
    value_hex: 0a
---
session_id: 1
start_ts: 21
commit_ts: 30
events:
  - seq: 1
    op: W
    key_hex: 01
    delete: true
  - seq: 2
    op: W
    key_hex: 01
    value_hex: 0b
---
session_id: 1
start_ts: 31
commit_ts: 40
events:
  - seq: 1
    op: R
    key_hex: 01
    value_hex: 0b
)yaml");

  const auto index = build_index_or_die(trace);
  auto key         = common::HexValue::from_hex("01");
  CHECK(key.has_value());
  const auto versions = index.versions_by_key.find(*key);
  CHECK(versions != index.versions_by_key.end());
  CHECK(versions->second.size() == 2);
  CHECK(versions->second[1].write_event == 2);
  CHECK(!versions->second[1].is_delete);
  CHECK(versions->second[1].value.has_value());
  CHECK(versions->second[1].value->to_hex() == "0b");

  auto result = run_basic_checker(trace);
  CHECK(result.status == checker::CheckStatus::Pass);
}

} // namespace

void run_index_tests() {
  test_index_keeps_last_write_per_key_per_txn();
  test_index_keeps_final_delete_per_key_per_txn();
  test_index_keeps_final_value_after_delete_per_key_per_txn();
}

} // namespace test
