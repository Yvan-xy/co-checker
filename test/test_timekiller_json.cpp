#include "test_harness.h"

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

#include "common/trace/timekiller_json.h"
#include "common/trace/trace_binary.h"

namespace test {
namespace {

std::string binary_trace(const common::Trace &trace) {
  std::ostringstream output;
  std::string error;
  CHECK(common::write_binary_trace_stream(output, trace, &error));
  CHECK(error.empty());
  return output.str();
}

void test_timekiller_json_mapping_and_determinism() {
  const auto trace = parse_or_die(R"yaml(
---
session_id: 9
start_ts: 1
commit_ts: 2
events: []
---
session_id: 7
start_ts: 10
commit_ts: 20
events:
  - seq: 2
    op: W
    key_hex: 00ff
    value_hex: aa
  - seq: 5
    op: R
    key_hex: 00ff
    value_hex: aa
  - seq: 9
    op: W
    key_hex: 01
    value_hex: bb
---
session_id: 8
start_ts: 21
commit_ts: 30
events:
  - seq: 1
    op: R
    key_hex: 01
    value_hex: bb
  - seq: 4
    op: W
    key_hex: 02
    value_hex: aa
  - seq: 6
    op: W
    key_hex: 00ff
    delete: true
  - seq: 8
    op: R
    key_hex: 03
    miss: true
)yaml");

  const auto binary = binary_trace(trace);
  std::istringstream first_input(binary);
  std::ostringstream first_output;
  auto first = common::write_timekiller_json(first_input, first_output);
  CHECK(first.has_value());
  if (!first) {
    return;
  }

  const std::string expected =
      "[{\"tid\":0,\"sid\":9,\"sts\":{\"p\":1,\"l\":0},"
      "\"cts\":{\"p\":2,\"l\":0},\"ops\":[]},"
      "{\"tid\":1,\"sid\":7,\"sts\":{\"p\":10,\"l\":0},"
      "\"cts\":{\"p\":20,\"l\":0},\"ops\":["
      "{\"t\":\"w\",\"k\":0,\"v\":0},"
      "{\"t\":\"r\",\"k\":0,\"v\":0},"
      "{\"t\":\"w\",\"k\":1,\"v\":1}]},"
      "{\"tid\":2,\"sid\":8,\"sts\":{\"p\":21,\"l\":0},"
      "\"cts\":{\"p\":30,\"l\":0},\"ops\":["
      "{\"t\":\"r\",\"k\":1,\"v\":1},"
      "{\"t\":\"w\",\"k\":2,\"v\":0},"
      "{\"t\":\"w\",\"k\":0,\"v\":null},"
      "{\"t\":\"r\",\"k\":3,\"v\":null}]}]";

  CHECK(first_output.str() == expected);
  CHECK(first.value().transactions == 3);
  CHECK(first.value().events == 7);
  CHECK(first.value().keys == 4);
  CHECK(first.value().values == 2);
  CHECK(first.value().output_bytes == expected.size());

  std::istringstream second_input(binary);
  std::ostringstream second_output;
  auto second = common::write_timekiller_json(second_input, second_output);
  CHECK(second.has_value());
  CHECK(second_output.str() == first_output.str());
}

void test_timekiller_json_rejects_java_long_timestamp_overflow() {
  auto trace = parse_or_die(R"yaml(
---
session_id: 1
start_ts: 10
commit_ts: 20
events: []
)yaml");
  trace.transactions[0].start_ts =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
  trace.transactions[0].commit_ts = trace.transactions[0].start_ts;

  std::istringstream input(binary_trace(trace));
  std::ostringstream output;
  auto result = common::write_timekiller_json(input, output);
  CHECK(!result);
  if (!result) {
    CHECK(!result.error().empty());
    CHECK(result.error()[0].message.find("does not fit signed Java long") !=
          std::string::npos);
  }
}

void test_timekiller_json_rejects_trailing_data() {
  const auto trace = parse_or_die(R"yaml(
---
session_id: 1
start_ts: 10
commit_ts: 20
events: []
)yaml");
  auto binary      = binary_trace(trace);
  binary.push_back('x');

  std::istringstream input(binary);
  std::ostringstream output;
  auto result = common::write_timekiller_json(input, output);
  CHECK(!result);
  if (!result) {
    CHECK(!result.error().empty());
    CHECK(result.error()[0].message.find("trailing data") != std::string::npos);
  }
}

} // namespace

void run_timekiller_json_tests() {
  test_timekiller_json_mapping_and_determinism();
  test_timekiller_json_rejects_java_long_timestamp_overflow();
  test_timekiller_json_rejects_trailing_data();
}

} // namespace test
