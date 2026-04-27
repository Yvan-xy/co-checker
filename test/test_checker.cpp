#include "test_harness.h"

#include <filesystem>

#include "common/trace/trace_parser.h"
#include "common/util/check_result.h"

namespace test {
namespace {

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

  auto result = run_basic_checker(trace);
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

  auto result = run_basic_checker(trace);
  CHECK(result.status == checker::CheckStatus::Fail);
  CHECK(!result.diagnostics.empty());
  CHECK(result.diagnostics[0].message ==
        "read value does not match latest visible committed version");
}

void test_so_checker_sets_fail_status_when_diagnostics_exist() {
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

  auto result = run_so_checker(trace);
  CHECK(result.status == checker::CheckStatus::Fail);
  CHECK(!result.diagnostics.empty());
}

void test_generated_trace_passes_basic_checker() {
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

  auto result = run_basic_checker(trace.value());
  CHECK(result.status == checker::CheckStatus::Pass);
}

} // namespace

void run_checker_tests() {
  test_co_snapshot_checker_passes();
  test_co_snapshot_checker_fails_stale_read();
  test_so_checker_sets_fail_status_when_diagnostics_exist();
  test_generated_trace_passes_basic_checker();
}

} // namespace test
