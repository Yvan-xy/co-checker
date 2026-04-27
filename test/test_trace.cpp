#include "test_harness.h"

#include <filesystem>
#include <sstream>
#include <string>

#include "common/trace/trace_parser.h"
#include "common/trace/types.h"

namespace test {
namespace {

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

void test_trace_print_helpers() {
  auto trace = parse_or_die(R"yaml(
---
session_id: 7
start_ts: 10
commit_ts: 20
events:
  - seq: 1
    op: W
    key_hex: 01
    value_hex: 0a
)yaml");

  std::ostringstream out;
  out << trace.transactions[0].to_string();
  trace.events[0].print(out);

  const auto printed = out.str();
  CHECK(printed.find("session_id: 7") != std::string::npos);
  CHECK(printed.find("event_ids: 0") != std::string::npos);
  CHECK(printed.find("op: W") != std::string::npos);
  CHECK(printed.find("key_hex: 01") != std::string::npos);
  CHECK(printed.find("value_hex: 0a") != std::string::npos);
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
  CHECK(result.error()[0].message.find("is invalid") != std::string::npos);
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
  CHECK(result.error()[0].message.find("duplicates an earlier transaction") !=
        std::string::npos);
}

void test_generated_trace_parses() {
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
}

} // namespace

void run_trace_tests() {
  test_hex_value();
  test_parser_layout();
  test_trace_print_helpers();
  test_parser_rejects_bad_hex();
  test_parser_rejects_duplicate_commit_ts();
  test_generated_trace_parses();
}

} // namespace test
