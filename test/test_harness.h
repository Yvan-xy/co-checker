#pragma once

#include <string>

#include "common/trace/trace.h"
#include "common/trace/trace_index.h"
#include "common/util/check_result.h"

namespace test {

extern int failures;

void check(bool condition, const char *expression, const char *file, int line);

common::Trace parse_or_die(const std::string &text);
common::TraceIndex build_index_or_die(const common::Trace &trace);
checker::CheckResult run_basic_checker(const common::Trace &trace);
checker::CheckResult run_so_checker(const common::Trace &trace);
checker::CheckResult run_so_cl_checker(const common::Trace &trace);
checker::CheckResult run_so_ww_checker(const common::Trace &trace,
                                       bool stat = false);
checker::CheckResult run_serializable_checker(const common::Trace &trace);

void run_trace_tests();
void run_snapshot_tests();
void run_checker_tests();
void run_index_tests();
void run_timekiller_json_tests();

} // namespace test

#define CHECK(expr) test::check((expr), #expr, __FILE__, __LINE__)
