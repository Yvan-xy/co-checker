#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "checker/checker_registry.h"
#include "common/diag/trace_dump.h"
#include "common/trace/trace_index.h"
#include "common/trace/trace_parser.h"

namespace {

struct CliConfig {
  std::string checker = "none";
  std::string dump;
  std::string trace_path;
  bool help = false;
};

void print_checker_choices(std::ostream &os,
                           const checker::CheckerRegistry &registry) {
  os << "none";
  for (const auto name : registry.names()) {
    os << '|' << name;
  }
  os << "|all";
}

void print_usage(std::ostream &os, const checker::CheckerRegistry &registry) {
  os << "usage: co-checker [--checker ";
  print_checker_choices(os, registry);
  os << "] [--dump parsed|versions|all] <trace.yaml>\n";
}

bool parse_args(int argc, char **argv, CliConfig &config,
                const checker::CheckerRegistry &registry) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "-h" || arg == "--help") {
      print_usage(std::cout, registry);
      config.help = true;
      return true;
    }
    if (arg == "--checker") {
      if (i + 1 >= argc) {
        std::cerr << "--checker requires a value\n";
        return false;
      }
      config.checker = argv[++i];
      continue;
    }
    if (arg == "--dump") {
      if (i + 1 >= argc) {
        std::cerr << "--dump requires a value\n";
        return false;
      }
      config.dump = argv[++i];
      continue;
    }
    if (!arg.empty() && arg.front() == '-') {
      std::cerr << "unknown option: " << arg << '\n';
      return false;
    }
    if (!config.trace_path.empty()) {
      std::cerr << "multiple trace files provided\n";
      return false;
    }
    config.trace_path = std::string(arg);
  }

  if (config.trace_path.empty()) {
    print_usage(std::cerr, registry);
    return false;
  }
  return true;
}

double elapsed_ms(std::chrono::steady_clock::time_point start,
                  std::chrono::steady_clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

} // namespace

int main(int argc, char **argv) {
  auto registry = checker::make_default_checker_registry();

  CliConfig config;
  if (!parse_args(argc, argv, config, registry)) {
    return 3;
  }
  if (config.help) {
    return 0;
  }

  const common::TraceParser parser;
  auto trace = parser.parse_file(config.trace_path);
  if (!trace) {
    common::print_diagnostics(std::cerr, trace.error());
    return 2;
  }

  common::TraceIndexBuilder index_builder;
  auto index = index_builder.build(trace.value());
  if (!index) {
    common::print_diagnostics(std::cerr, index.error());
    return 2;
  }

  if (config.dump == "parsed" || config.dump == "all") {
    common::dump_parsed_trace(std::cout, trace.value());
  }
  if (config.dump == "versions" || config.dump == "all") {
    common::dump_versions(std::cout, index.value());
  }
  if (!config.dump.empty() && config.dump != "parsed" &&
      config.dump != "versions" && config.dump != "all") {
    std::cerr << "unknown dump kind: " << config.dump << '\n';
    return 3;
  }

  std::vector<const checker::Checker *> selected;
  if (config.checker == "none") {
    if (config.dump.empty()) {
      std::cout << "trace: pass\n";
    }
    return 0;
  }

  if (config.checker == "all") {
    for (const auto &checker : registry.checkers()) {
      selected.push_back(checker.get());
    }
  } else if (const auto *checker = registry.find(config.checker)) {
    selected.push_back(checker);
  } else {
    std::cerr << "unknown checker: " << config.checker << '\n';
    std::cerr << "available checkers: ";
    print_checker_choices(std::cerr, registry);
    std::cerr << '\n';
    return 3;
  }

  bool any_failed = false;
  common::NullLogger logger;
  for (const auto *checker : selected) {
    checker::CheckerContext context{
        .trace  = trace.value(),
        .index  = index.value(),
        .logger = &logger,
    };
    const auto start = std::chrono::steady_clock::now();
    auto result = checker->run(context);
    const auto end = std::chrono::steady_clock::now();

    std::cout << checker->name() << ": " << checker::to_string(result.status)
              << " (" << std::fixed << std::setprecision(3)
              << elapsed_ms(start, end) << " ms)\n";
    common::print_diagnostics(std::cout, result.diagnostics);
    if (result.status == checker::CheckStatus::Fail) {
      any_failed = true;
    }
  }

  return any_failed ? 1 : 0;
}
