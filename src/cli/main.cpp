#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "co_checker/checker/checker_registry.h"
#include "co_checker/diag/trace_dump.h"
#include "co_checker/trace/trace_index.h"
#include "co_checker/trace/trace_parser.h"

namespace {

struct CliConfig {
    std::string checker = "none";
    std::string dump;
    std::string trace_path;
    bool help = false;
};

void print_usage(std::ostream& os) {
    os << "usage: co-checker [--checker none|co-si|all] [--dump parsed|versions|all] "
          "<trace.yaml>\n";
}

bool parse_args(int argc, char** argv, CliConfig& config) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout);
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
        print_usage(std::cerr);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    CliConfig config;
    if (!parse_args(argc, argv, config)) {
        return 3;
    }
    if (config.help) {
        return 0;
    }

    const co_checker::TraceParser parser;
    auto trace = parser.parse_file(config.trace_path);
    if (!trace) {
        co_checker::print_diagnostics(std::cerr, trace.error());
        return 2;
    }

    co_checker::TraceIndexBuilder index_builder;
    auto index = index_builder.build(trace.value());
    if (!index) {
        co_checker::print_diagnostics(std::cerr, index.error());
        return 2;
    }

    if (config.dump == "parsed" || config.dump == "all") {
        co_checker::dump_parsed_trace(std::cout, trace.value());
    }
    if (config.dump == "versions" || config.dump == "all") {
        co_checker::dump_versions(std::cout, index.value());
    }
    if (!config.dump.empty() && config.dump != "parsed" && config.dump != "versions" &&
        config.dump != "all") {
        std::cerr << "unknown dump kind: " << config.dump << '\n';
        return 3;
    }

    auto registry = co_checker::make_default_checker_registry();
    std::vector<const co_checker::Checker*> selected;
    if (config.checker == "none") {
        if (config.dump.empty()) {
            std::cout << "trace: pass\n";
        }
        return 0;
    }

    if (config.checker == "all") {
        for (const auto& checker : registry.checkers()) {
            selected.push_back(checker.get());
        }
    } else if (const auto* checker = registry.find(config.checker)) {
        selected.push_back(checker);
    } else {
        std::cerr << "unknown checker: " << config.checker << '\n';
        return 3;
    }

    bool any_failed = false;
    co_checker::NullLogger logger;
    for (const auto* checker : selected) {
        co_checker::CheckerContext context{
            .trace = trace.value(),
            .index = index.value(),
            .logger = &logger,
        };
        auto result = checker->run(context);
        std::cout << checker->name() << ": " << co_checker::to_string(result.status)
                  << '\n';
        co_checker::print_diagnostics(std::cout, result.diagnostics);
        if (result.status == co_checker::CheckStatus::Fail) {
            any_failed = true;
        }
    }

    return any_failed ? 1 : 0;
}
