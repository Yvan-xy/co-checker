#include <filesystem>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "common/diag/trace_dump.h"
#include "common/trace/trace_binary.h"
#include "common/trace/trace_parser.h"

namespace {

struct TraceStats {
  std::size_t sessions     = 0;
  std::size_t transactions = 0;
  std::size_t events       = 0;
};

struct Config {
  std::filesystem::path input;
  std::optional<std::filesystem::path> output;
  bool rename = false;
};

[[noreturn]] void usage_error(std::string message) {
  throw std::runtime_error(std::move(message) +
                           "\nusage: trace-convert [-o output.trace] "
                           "[--rename] <trace.yaml>");
}

Config parse_args(const int argc, char **argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "-o" || arg == "--output") {
      if (++i >= argc) {
        usage_error("missing output path after " + std::string(arg));
      }
      config.output = std::filesystem::path{argv[i]};
    } else if (arg == "--rename") {
      config.rename = true;
    } else if (!arg.empty() && arg.front() == '-') {
      usage_error("unknown option " + std::string(arg));
    } else if (config.input.empty()) {
      config.input = std::filesystem::path{argv[i]};
    } else {
      usage_error("multiple input paths provided");
    }
  }

  if (config.input.empty()) {
    usage_error("missing input path");
  }
  return config;
}

TraceStats trace_stats(const common::Trace &trace) {
  std::set<common::SessionId> sessions;
  for (const auto &txn : trace.transactions) {
    sessions.insert(txn.session_id);
  }
  return TraceStats{.sessions     = sessions.size(),
                    .transactions = trace.transactions.size(),
                    .events       = trace.events.size()};
}

std::string trace_name_suffix(const std::string &stem) {
  static const std::regex trace_name_re{R"(^\d+s-\d+t-\d+e(?:-(.+))?$)"};
  static const std::regex session_prefix_re{R"(^\d+s-(.+)$)"};

  std::smatch match;
  if (std::regex_match(stem, match, trace_name_re)) {
    if (match.size() >= 2) {
      return match[1].str();
    }
    return {};
  }
  if (std::regex_match(stem, match, session_prefix_re)) {
    return match[1].str();
  }
  return stem;
}

std::filesystem::path path_with_stats(const std::filesystem::path &path,
                                      const TraceStats &stats) {
  auto stem = std::to_string(stats.sessions) + "s-" +
              std::to_string(stats.transactions) + "t-" +
              std::to_string(stats.events) + "e";
  const auto suffix = trace_name_suffix(path.stem().string());
  if (!suffix.empty()) {
    stem += "-" + suffix;
  }
  return path.parent_path() /
         (stem + std::string(common::kBinaryTraceExtension));
}

std::filesystem::path default_output_path(const std::filesystem::path &input) {
  auto output = input;
  output.replace_extension(std::string(common::kBinaryTraceExtension));
  return output;
}

} // namespace

int main(const int argc, char **argv) {
  try {
    const auto config = parse_args(argc, argv);

    const common::TraceParser parser;
    auto trace = parser.parse_file(config.input.string());
    if (!trace) {
      common::print_diagnostics(std::cerr, trace.error());
      return 2;
    }

    auto output_path =
        config.output.value_or(default_output_path(config.input));
    if (config.rename) {
      output_path = path_with_stats(output_path, trace_stats(trace.value()));
    }

    if (!output_path.parent_path().empty()) {
      std::filesystem::create_directories(output_path.parent_path());
    }

    const auto temp_path = output_path.string() + ".tmp";
    std::string error;
    if (!common::write_binary_trace_file(temp_path, trace.value(), &error)) {
      throw std::runtime_error(error);
    }
    std::filesystem::rename(temp_path, output_path);

    std::cout << output_path.string() << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
