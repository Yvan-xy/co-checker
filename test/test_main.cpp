#include "test_harness.h"

#include <iostream>

int main() {
  test::run_trace_tests();
  test::run_snapshot_tests();
  test::run_checker_tests();
  test::run_index_tests();
  test::run_timekiller_json_tests();

  if (test::failures != 0) {
    std::cerr << test::failures << " test failure(s)\n";
    return 1;
  }
  return 0;
}
