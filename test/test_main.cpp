#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "checker/basic/co_snapshot_checker.h"
#include "common/trace/trace_index.h"
#include "common/trace/trace_parser.h"

namespace {

int failures = 0;

void check(bool condition, const char *expression, const char *file, int line) {
  if (!condition) {
    std::cerr << file << ':' << line << ": check failed: " << expression
              << '\n';
    ++failures;
  }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

common::Trace parse_or_die(const std::string &text) {
  std::istringstream input(text);
  common::TraceParser parser;
  auto result = parser.parse_stream(input);
  if (!result) {
    for (const auto &diagnostic : result.error()) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    std::abort();
  }
  return std::move(result.value());
}

checker::CheckResult run_checker(const common::Trace &trace) {
  common::TraceIndexBuilder builder;
  auto index = builder.build(trace);
  if (!index) {
    std::abort();
  }

  checker::CoSnapshotBasicChecker checker;
  common::NullLogger logger;
  checker::CheckerContext context{
      .trace  = trace,
      .index  = index.value(),
      .logger = &logger,
  };
  return checker.run(context);
}

common::TraceIndex build_index_or_die(const common::Trace &trace) {
  common::TraceIndexBuilder builder;
  auto index = builder.build(trace);
  if (!index) {
    for (const auto &diagnostic : index.error()) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    std::abort();
  }
  return std::move(index.value());
}

void test_hex_value() {
  std::string error;
  auto value = common::HexValue::from_hex("80000100000002010064", &error);
  CHECK(value.has_value());
  CHECK(value->byte_len == 10);
  CHECK(value->extra_words.empty());
  CHECK(value->to_hex() == "80000100000002010064");

  auto upper = common::HexValue::from_hex("80000100000002010064", &error);
  CHECK(upper.has_value());
  CHECK(*value == *upper);
  CHECK(!(*value != *upper));
  CHECK(common::HexValueHash{}(*value) == common::HexValueHash{}(*upper));

  auto long_value =
      common::HexValue::from_hex("7480000000000000725f6980000000000000010380000"
                                 "00000000051038000000000000008",
                                 &error);
  CHECK(long_value.has_value());
  CHECK(long_value->byte_len == 37);
  CHECK(!long_value->extra_words.empty());
  CHECK(long_value->to_hex() == "7480000000000000725f69800000000000000103800000"
                                "0000000051038000000000000008");

  auto bad = common::HexValue::from_hex("abc", &error);
  CHECK(!bad.has_value());
}

void test_parser_layout() {
  auto trace = parse_or_die(R"yaml(
---
session_id: 1
start_ts: 10
commit_ts: 20
events:
  - seq: 1
    op: W
    key_hex: 01
    value_hex: 0A
---
session_id: 1
start_ts: 21
commit_ts: 22
events:
  - seq: 1
    op: R
    key_hex: 01
    value_hex: 0a
    source: snapshot
)yaml");

  CHECK(trace.transactions.size() == 2);
  CHECK(trace.events.size() == 2);
  CHECK(trace.transactions[0].event_ids.size() == 1);
  CHECK(trace.transactions[0].event_ids[0] == 0);
  CHECK(trace.transactions[1].event_ids[0] == 1);
  CHECK(trace.events[0].id == 0);
  CHECK(trace.events[0].txn_id == 0);
  CHECK(trace.events[1].txn_id == 1);
  CHECK(trace.events[0].key == trace.events[1].key);
  CHECK(trace.events[0].value == trace.events[1].value);
}

void test_parser_rejects_bad_hex() {
  std::istringstream input(R"yaml(
---
session_id: 1
start_ts: 10
commit_ts: 20
events:
  - seq: 1
    op: W
    key_hex: xyz
)yaml");

  common::TraceParser parser;
  auto result = parser.parse_stream(input);
  CHECK(!result);
  CHECK(!result.error().empty());
  CHECK(result.error()[0].code == "parse.invalid_hex");
}

void test_parser_rejects_duplicate_commit_ts() {
  std::istringstream input(R"yaml(
---
session_id: 1
start_ts: 10
commit_ts: 20
events: []
---
session_id: 1
start_ts: 11
commit_ts: 20
events: []
)yaml");

  common::TraceParser parser;
  auto result = parser.parse_stream(input);
  CHECK(!result);
  CHECK(!result.error().empty());
  CHECK(result.error()[0].code == "parse.duplicate_commit_ts");
}

void test_co_snapshot_checker_passes() {
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
commit_ts: 22
events:
  - seq: 1
    op: R
    key_hex: 01
    value_hex: 0a
)yaml");

  auto result = run_checker(trace);
  CHECK(result.status == checker::CheckStatus::Pass);
  CHECK(result.diagnostics.empty());
}

void test_co_snapshot_checker_fails_stale_read() {
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
commit_ts: 22
events:
  - seq: 1
    op: R
    key_hex: 01
    value_hex: 0b
)yaml");

  auto result = run_checker(trace);
  CHECK(result.status == checker::CheckStatus::Fail);
  CHECK(!result.diagnostics.empty());
  CHECK(result.diagnostics[0].code == "co_si.stale_read");
}

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

  auto result = run_checker(trace);
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

  auto result = run_checker(trace);
  CHECK(result.status == checker::CheckStatus::Pass);
}

void test_generated_trace_parses_and_passes() {
  const auto path = std::filesystem::path(CO_CHECKER_SOURCE_DIR) / "traces" /
                    "2s-4t-10e-mutual.yaml";
  if (!std::filesystem::exists(path)) {
    return;
  }

  common::TraceParser parser;
  auto trace = parser.parse_file(path.string());
  CHECK(trace.has_value());
  if (!trace) {
    return;
  }
  CHECK(trace.value().transactions.size() == 4);
  CHECK(trace.value().events.size() == 10);

  auto result = run_checker(trace.value());
  CHECK(result.status == checker::CheckStatus::Pass);
}

} // namespace

int main() {
  test_hex_value();
  test_parser_layout();
  test_parser_rejects_bad_hex();
  test_parser_rejects_duplicate_commit_ts();
  test_co_snapshot_checker_passes();
  test_co_snapshot_checker_fails_stale_read();
  test_index_keeps_last_write_per_key_per_txn();
  test_index_keeps_final_delete_per_key_per_txn();
  test_index_keeps_final_value_after_delete_per_key_per_txn();
  test_generated_trace_parses_and_passes();

  if (failures != 0) {
    std::cerr << failures << " test failure(s)\n";
    return 1;
  }
  return 0;
}
