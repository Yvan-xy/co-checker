#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "co_checker/checker/co_snapshot_checker.h"
#include "co_checker/trace/trace_index.h"
#include "co_checker/trace/trace_parser.h"

namespace {

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

co_checker::Trace parse_or_die(const std::string& text) {
    std::istringstream input(text);
    co_checker::TraceParser parser;
    auto result = parser.parse_stream(input);
    if (!result) {
        for (const auto& diagnostic : result.error()) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
        std::abort();
    }
    return std::move(result.value());
}

co_checker::CheckResult run_checker(const co_checker::Trace& trace) {
    co_checker::TraceIndexBuilder builder;
    auto index = builder.build(trace);
    if (!index) {
        std::abort();
    }

    co_checker::CoSnapshotChecker checker;
    co_checker::NullLogger logger;
    co_checker::CheckerContext context{
        .trace = trace,
        .index = index.value(),
        .logger = &logger,
    };
    return checker.run(context);
}

void test_hex_value() {
    std::string error;
    auto value = co_checker::HexValue::from_hex("80000100000002010064", &error);
    CHECK(value.has_value());
    CHECK(value->byte_len == 10);
    CHECK(value->extra_words.empty());
    CHECK(value->to_hex() == "80000100000002010064");

    auto upper = co_checker::HexValue::from_hex("80000100000002010064", &error);
    CHECK(upper.has_value());
    CHECK(*value == *upper);
    CHECK(!(*value != *upper));
    CHECK(co_checker::HexValueHash{}(*value) == co_checker::HexValueHash{}(*upper));

    auto long_value = co_checker::HexValue::from_hex(
        "7480000000000000725f698000000000000001038000000000000051038000000000000008",
        &error);
    CHECK(long_value.has_value());
    CHECK(long_value->byte_len == 37);
    CHECK(!long_value->extra_words.empty());
    CHECK(long_value->to_hex() ==
          "7480000000000000725f698000000000000001038000000000000051038000000000000008");

    auto bad = co_checker::HexValue::from_hex("abc", &error);
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

    co_checker::TraceParser parser;
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

    co_checker::TraceParser parser;
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
    CHECK(result.status == co_checker::CheckStatus::Pass);
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
    CHECK(result.status == co_checker::CheckStatus::Fail);
    CHECK(!result.diagnostics.empty());
    CHECK(result.diagnostics[0].code == "co_si.stale_read");
}

void test_generated_trace_parses_and_passes() {
    const auto path = std::filesystem::path(CO_CHECKER_SOURCE_DIR) /
                      "traces" / "2s-4t-10e-mutual.yaml";
    if (!std::filesystem::exists(path)) {
        return;
    }

    co_checker::TraceParser parser;
    auto trace = parser.parse_file(path.string());
    CHECK(trace.has_value());
    if (!trace) {
        return;
    }
    CHECK(trace.value().transactions.size() == 4);
    CHECK(trace.value().events.size() == 10);

    auto result = run_checker(trace.value());
    CHECK(result.status == co_checker::CheckStatus::Pass);
}

}  // namespace

int main() {
    test_hex_value();
    test_parser_layout();
    test_parser_rejects_bad_hex();
    test_parser_rejects_duplicate_commit_ts();
    test_co_snapshot_checker_passes();
    test_co_snapshot_checker_fails_stale_read();
    test_generated_trace_parses_and_passes();

    if (failures != 0) {
        std::cerr << failures << " test failure(s)\n";
        return 1;
    }
    return 0;
}
