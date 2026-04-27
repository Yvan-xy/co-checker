#include "test_harness.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

#include "checker/basic/co_snapshot_checker.h"
#include "checker/so_only/so_only_checker.h"
#include "common/diag/logger.h"
#include "common/trace/trace_parser.h"

namespace test {

int failures = 0;

void check(bool condition, const char *expression, const char *file, int line) {
  if (!condition) {
    std::cerr << file << ':' << line << ": check failed: " << expression
              << '\n';
    ++failures;
  }
}

common::Trace parse_or_die(const std::string &text) {
  std::istringstream input(text);
  common::TraceParser parser;
  auto result = parser.parse_stream(input);
  if (!result) {
    for (const auto &diagnostic : result.error()) {
      std::cerr << diagnostic.message << '\n';
    }
    std::abort();
  }
  return std::move(result.value());
}

common::TraceIndex build_index_or_die(const common::Trace &trace) {
  common::TraceIndexBuilder builder;
  auto index = builder.build(trace);
  if (!index) {
    for (const auto &diagnostic : index.error()) {
      std::cerr << diagnostic.message << '\n';
    }
    std::abort();
  }
  return std::move(index.value());
}

checker::CheckResult run_basic_checker(const common::Trace &trace) {
  auto index = build_index_or_die(trace);

  checker::CoSnapshotBasicChecker checker;
  common::NullLogger logger;
  checker::CheckerContext context{
      .trace  = trace,
      .index  = index,
      .logger = &logger,
  };
  return checker.run(context);
}

checker::CheckResult run_so_checker(const common::Trace &trace) {
  auto index = build_index_or_die(trace);

  checker::SnapshotSOChecker checker;
  common::NullLogger logger;
  checker::CheckerContext context{
      .trace  = trace,
      .index  = index,
      .logger = &logger,
  };
  return checker.run(context);
}

} // namespace test
