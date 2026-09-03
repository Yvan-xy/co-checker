#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "common/diag/trace_dump.h"
#include "common/trace/trace_binary.h"

namespace {

struct Config {
  std::filesystem::path input;
  std::filesystem::path output;
  std::optional<std::filesystem::path> stats;
};

class TemporaryFile {
public:
  explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}

  TemporaryFile(const TemporaryFile &)            = delete;
  TemporaryFile &operator=(const TemporaryFile &) = delete;

  ~TemporaryFile() {
    if (!committed_) {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

  void commit_to(const std::filesystem::path &destination) {
    std::filesystem::rename(path_, destination);
    committed_ = true;
  }

private:
  std::filesystem::path path_;
  bool committed_ = false;
};

[[noreturn]] void usage_error(std::string message) {
  throw std::runtime_error(
      std::move(message) +
      "\nusage: cotrace-densify [--stats stats.json] "
      "<input.trace> <output.trace>");
}

Config parse_args(const int argc, char **argv) {
  Config config;
  bool positional_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (!positional_only && arg == "--") {
      positional_only = true;
    } else if (!positional_only && arg == "--stats") {
      if (++i >= argc) {
        usage_error("missing path after --stats");
      }
      config.stats = std::filesystem::path{argv[i]};
    } else if (!positional_only && (arg == "-h" || arg == "--help")) {
      std::cout << "usage: cotrace-densify [--stats stats.json] "
                   "<input.trace> <output.trace>\n";
      std::exit(0);
    } else if (!positional_only && !arg.empty() && arg.front() == '-') {
      usage_error("unknown option " + std::string(arg));
    } else if (config.input.empty()) {
      config.input = std::filesystem::path{argv[i]};
    } else if (config.output.empty()) {
      config.output = std::filesystem::path{argv[i]};
    } else {
      usage_error("too many positional paths");
    }
  }

  if (config.input.empty()) {
    usage_error("missing input path");
  }
  if (config.output.empty()) {
    usage_error("missing output path");
  }
  if (config.output.extension() != common::kBinaryTraceExtension) {
    usage_error("output path must have a .trace extension");
  }

  const auto normalized_input =
      std::filesystem::absolute(config.input).lexically_normal();
  const auto normalized_output =
      std::filesystem::absolute(config.output).lexically_normal();
  if (normalized_input == normalized_output) {
    usage_error("input and output paths must differ");
  }
  if (config.stats.has_value()) {
    const auto normalized_stats =
        std::filesystem::absolute(*config.stats).lexically_normal();
    if (normalized_stats == normalized_input ||
        normalized_stats == normalized_output) {
      usage_error("stats path must differ from input and output paths");
    }
  }
  return config;
}

void create_parent_directories(const std::filesystem::path &path) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
}

std::filesystem::path temporary_path_for(const std::filesystem::path &path) {
  std::random_device random;
  for (int attempt = 0; attempt < 128; ++attempt) {
    auto temporary = path;
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    temporary +=
        ".tmp." + std::to_string(timestamp) + "." + std::to_string(random());
    if (!std::filesystem::exists(temporary)) {
      return temporary;
    }
  }
  throw std::runtime_error("failed to choose a unique temporary path for '" +
                           path.string() + "'");
}

void write_stats(const std::filesystem::path &path,
                 const common::DenseBinaryTraceStats &stats) {
  create_parent_directories(path);
  TemporaryFile temporary(temporary_path_for(path));
  std::ofstream output(temporary.path(), std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error("failed to open stats file '" + path.string() +
                             "'");
  }
  output << "{\n"
         << "  \"transactions\": " << stats.transactions << ",\n"
         << "  \"events\": " << stats.events << ",\n"
         << "  \"keys\": " << stats.keys << ",\n"
         << "  \"values\": " << stats.values << ",\n"
         << "  \"output_bytes\": " << stats.output_bytes << "\n"
         << "}\n";
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write stats file '" + path.string() +
                             "'");
  }
  temporary.commit_to(path);
}

} // namespace

int main(const int argc, char **argv) {
  try {
    const auto config = parse_args(argc, argv);
    create_parent_directories(config.output);

    std::ifstream input(config.input, std::ios::binary);
    if (!input.is_open()) {
      throw std::runtime_error("failed to open input trace '" +
                               config.input.string() + "'");
    }

    TemporaryFile temporary(temporary_path_for(config.output));
    std::ofstream output(temporary.path(), std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      throw std::runtime_error("failed to open output trace '" +
                               config.output.string() + "'");
    }

    auto result = common::write_dense_binary_trace(input, output);
    if (!result) {
      common::print_diagnostics(std::cerr, result.error());
      return 2;
    }
    output.close();
    if (!output) {
      throw std::runtime_error("failed to close output trace '" +
                               config.output.string() + "'");
    }

    const auto &stats      = result.value();
    const auto actual_size = std::filesystem::file_size(temporary.path());
    if (actual_size != stats.output_bytes) {
      throw std::runtime_error("output byte count does not match file size");
    }

    temporary.commit_to(config.output);
    if (config.stats.has_value()) {
      write_stats(*config.stats, stats);
    }

    std::cout << "transactions=" << stats.transactions
              << " events=" << stats.events << " keys=" << stats.keys
              << " values=" << stats.values
              << " output_bytes=" << stats.output_bytes << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
