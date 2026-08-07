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

void test_co_snapshot_checker_fails_overlapping_same_key_writes() {
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
start_ts: 30
commit_ts: 50
events:
  - seq: 1
    op: W
    key_hex: 02
    value_hex: 0b
---
session_id: 2
start_ts: 40
commit_ts: 60
events:
  - seq: 1
    op: R
    key_hex: 01
    value_hex: 0a
  - seq: 2
    op: W
    key_hex: 02
    value_hex: 0c
)yaml");

  auto result = run_basic_checker(trace);
  CHECK(result.status == checker::CheckStatus::Fail);
  CHECK(!result.diagnostics.empty());
  CHECK(result.diagnostics[0].message.find("write-write conflict") !=
        std::string::npos);
  CHECK(result.diagnostics[0].key.has_value());
  CHECK(result.diagnostics[0].key->to_hex() == "02");
}

void test_co_snapshot_checker_allows_nonoverlapping_same_key_writes() {
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
session_id: 2
start_ts: 21
commit_ts: 30
events:
  - seq: 1
    op: W
    key_hex: 01
    value_hex: 0b
)yaml");

  auto result = run_basic_checker(trace);
  CHECK(result.status == checker::CheckStatus::Pass);
  CHECK(result.diagnostics.empty());
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

void test_so_cl_checker_uses_final_write_per_key() {
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
    key_hex: 01
    value_hex: 0b
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

  auto result = run_so_cl_checker(trace);
  CHECK(result.status == checker::CheckStatus::Pass);
  CHECK(result.diagnostics.empty());
}

void test_so_cl_checker_ignores_unread_keys_while_rolling_back() {
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
    value_hex: 0b
---
session_id: 2
start_ts: 21
commit_ts: 30
events:
  - seq: 1
    op: W
    key_hex: 02
    value_hex: 0c
---
session_id: 1
start_ts: 31
commit_ts: 40
events:
  - seq: 1
    op: R
    key_hex: 01
    value_hex: 0a
)yaml");

  auto result = run_so_cl_checker(trace);
  CHECK(result.status == checker::CheckStatus::Pass);
  CHECK(result.diagnostics.empty());
}

void test_so_cl_checker_does_not_rollback_before_session_predecessor() {
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
    op: R
    key_hex: 01
    miss: true
)yaml");

  auto result = run_so_cl_checker(trace);
  CHECK(result.status == checker::CheckStatus::Fail);
  CHECK(!result.diagnostics.empty());
}

void test_so_cl_checker_passes_overwrite_chain_trace() {
  const auto path = std::filesystem::path(CO_CHECKER_SOURCE_DIR) / "traces" /
                    "1s-4t-18e-overwrite-chain.yaml";
  if (!std::filesystem::exists(path)) {
    return;
  }

  common::TraceParser parser;
  auto trace = parser.parse_file(path.string());
  CHECK(trace.has_value());
  if (!trace) {
    return;
  }

  auto result = run_so_cl_checker(trace.value());
  CHECK(result.status == checker::CheckStatus::Pass);
  CHECK(result.diagnostics.empty());
}

void test_so_ww_checker_rolls_back_unrelated_writes_before_ww_bound() {
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
session_id: 2
start_ts: 21
commit_ts: 30
events:
  - seq: 1
    op: W
    key_hex: 02
    value_hex: 0b
---
session_id: 3
start_ts: 31
commit_ts: 40
events:
  - seq: 1
    op: R
    key_hex: 02
    miss: true
  - seq: 2
    op: W
    key_hex: 01
    value_hex: 0c
)yaml");

  auto result = run_so_ww_checker(trace);
  CHECK(result.status == checker::CheckStatus::Pass);
  CHECK(result.diagnostics.empty());
}

void test_so_ww_checker_does_not_rollback_before_ww_bound() {
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
session_id: 2
start_ts: 21
commit_ts: 30
events:
  - seq: 1
    op: W
    key_hex: 02
    value_hex: 0b
---
session_id: 3
start_ts: 31
commit_ts: 40
events:
  - seq: 1
    op: R
    key_hex: 01
    miss: true
  - seq: 2
    op: W
    key_hex: 01
    value_hex: 0c
)yaml");

  auto result = run_so_ww_checker(trace);
  CHECK(result.status == checker::CheckStatus::Fail);
  CHECK(!result.diagnostics.empty());
  CHECK(result.diagnostics[0].key.has_value());
  CHECK(result.diagnostics[0].key->to_hex() == "01");
}

void test_so_ww_checker_requires_dense_ids() {
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
)yaml");
  trace.event_key_ids.clear();

  auto result = run_so_ww_checker(trace);
  CHECK(result.status == checker::CheckStatus::Inconclusive);
  CHECK(result.diagnostics.size() == 1);
  CHECK(result.diagnostics[0].message ==
        "trace is missing dense key/value IDs");
}

void test_serializable_checker_allows_read_own_write() {
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
session_id: 2
start_ts: 21
commit_ts: 30
events:
  - seq: 1
    op: W
    key_hex: 01
    value_hex: 0b
  - seq: 2
    op: R
    key_hex: 01
    value_hex: 0b
)yaml");

  auto result = run_serializable_checker(trace);
  CHECK(result.status == checker::CheckStatus::Pass);
  CHECK(result.diagnostics.empty());
}

void test_serializable_checker_requires_explicit_miss_for_absent_key() {
  auto trace = parse_or_die(R"yaml(
---
session_id: 1
start_ts: 10
commit_ts: 20
events:
  - seq: 1
    op: R
    key_hex: 01
)yaml");

  auto result = run_serializable_checker(trace);
  CHECK(result.status == checker::CheckStatus::Fail);
  CHECK(!result.diagnostics.empty());
}

void test_serializable_checker_allows_read_miss_after_local_delete() {
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
session_id: 2
start_ts: 21
commit_ts: 30
events:
  - seq: 1
    op: W
    key_hex: 01
    delete: true
  - seq: 2
    op: R
    key_hex: 01
    miss: true
)yaml");

  auto result = run_serializable_checker(trace);
  CHECK(result.status == checker::CheckStatus::Pass);
  CHECK(result.diagnostics.empty());
}

void test_serializable_checker_rejects_commit_order_stale_read() {
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
session_id: 2
start_ts: 21
commit_ts: 30
events:
  - seq: 1
    op: R
    key_hex: 01
    miss: true
)yaml");

  auto result = run_serializable_checker(trace);
  CHECK(result.status == checker::CheckStatus::Fail);
  CHECK(!result.diagnostics.empty());
  CHECK(result.diagnostics[0].message ==
        "read value does not match the commit-order serial snapshot");
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
  test_co_snapshot_checker_fails_overlapping_same_key_writes();
  test_co_snapshot_checker_allows_nonoverlapping_same_key_writes();
  test_so_checker_sets_fail_status_when_diagnostics_exist();
  test_so_cl_checker_uses_final_write_per_key();
  test_so_cl_checker_ignores_unread_keys_while_rolling_back();
  test_so_cl_checker_does_not_rollback_before_session_predecessor();
  test_so_cl_checker_passes_overwrite_chain_trace();
  test_so_ww_checker_rolls_back_unrelated_writes_before_ww_bound();
  test_so_ww_checker_does_not_rollback_before_ww_bound();
  test_so_ww_checker_requires_dense_ids();
  test_serializable_checker_allows_read_own_write();
  test_serializable_checker_requires_explicit_miss_for_absent_key();
  test_serializable_checker_allows_read_miss_after_local_delete();
  test_serializable_checker_rejects_commit_order_stale_read();
  test_generated_trace_passes_basic_checker();
}

} // namespace test
