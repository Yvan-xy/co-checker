#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace {

constexpr std::string_view kCommittedStatus = "committed";

struct Event {
  std::uint32_t seq = 0;
  std::string op;
  std::string key_hex;
  std::optional<std::string> value_hex;
  bool is_delete = false;
  bool is_miss   = false;
};

struct Transaction {
  std::size_t original_index = 0;
  std::uint64_t session_id   = 0;
  std::uint64_t start_ts     = 0;
  std::uint64_t commit_ts    = 0;
  std::vector<Event> events;
};

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
                           "\nusage: trace-preprocess [-o output] [--rename] "
                           "<raw-trace.yaml>");
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
  if (config.rename && !config.output.has_value()) {
    usage_error("--rename requires -o/--output");
  }
  return config;
}

template <typename T>
std::optional<T> optional_scalar(const YAML::Node &node,
                                 const std::string &field) {
  const auto value = node[field];
  if (!value) {
    return std::nullopt;
  }
  return value.as<T>();
}

std::vector<Event> normalize_events(const YAML::Node &events_node) {
  std::vector<Event> events;
  if (!events_node || !events_node.IsSequence()) {
    return events;
  }
  events.reserve(events_node.size());

  for (const auto &event_node : events_node) {
    const auto source = optional_scalar<std::string>(event_node, "source");
    const auto op     = optional_scalar<std::string>(event_node, "op");
    if (op.has_value() && *op == "R" && source.has_value() &&
        *source == "mem_buffer") {
      continue;
    }

    Event event;
    event.seq     = event_node["seq"].as<std::uint32_t>();
    event.op      = event_node["op"].as<std::string>();
    event.key_hex = event_node["key_hex"].as<std::string>();

    if (const auto value_node = event_node["value_hex"]) {
      event.value_hex = value_node.as<std::string>();
    }
    if (const auto delete_node = event_node["delete"]) {
      event.is_delete = delete_node.as<bool>();
    }
    if (const auto miss_node = event_node["miss"]) {
      event.is_miss = miss_node.as<bool>();
    }

    events.push_back(std::move(event));
  }
  return events;
}

std::vector<Transaction> read_committed_transactions(std::istream &input) {
  const auto documents = YAML::LoadAll(input);
  std::vector<Transaction> transactions;
  transactions.reserve(documents.size());

  for (std::size_t index = 0; index < documents.size(); ++index) {
    const auto &document = documents[index];
    if (!document || document.IsNull()) {
      continue;
    }
    if (!document.IsMap()) {
      throw std::runtime_error("document " + std::to_string(index + 1) +
                               ": expected a mapping");
    }

    const auto status = optional_scalar<std::string>(document, "status");
    if (status.has_value() && *status != kCommittedStatus) {
      continue;
    }

    const auto commit_ts =
        optional_scalar<std::uint64_t>(document, "commit_ts");
    if (!commit_ts.has_value()) {
      if (status.has_value() && *status == kCommittedStatus) {
        throw std::runtime_error("document " + std::to_string(index + 1) +
                                 ": committed transaction is missing "
                                 "commit_ts");
      }
      continue;
    }

    const auto session_id =
        optional_scalar<std::uint64_t>(document, "session_id");
    if (!session_id.has_value()) {
      continue;
    }

    Transaction transaction;
    transaction.original_index = index;
    transaction.session_id     = *session_id;
    transaction.start_ts       = document["start_ts"].as<std::uint64_t>();
    transaction.commit_ts      = *commit_ts;
    transaction.events         = normalize_events(document["events"]);
    transactions.push_back(std::move(transaction));
  }

  std::stable_sort(transactions.begin(), transactions.end(),
                   [](const Transaction &lhs, const Transaction &rhs) {
                     if (lhs.commit_ts == rhs.commit_ts) {
                       return lhs.original_index < rhs.original_index;
                     }
                     return lhs.commit_ts < rhs.commit_ts;
                   });
  return transactions;
}

void normalize_session_ids(std::vector<Transaction> &transactions) {
  std::set<std::uint64_t> original_ids;
  for (const auto &transaction : transactions) {
    original_ids.insert(transaction.session_id);
  }

  std::unordered_map<std::uint64_t, std::uint64_t> remap;
  std::uint64_t next_id = 1;
  for (const auto session_id : original_ids) {
    remap.emplace(session_id, next_id++);
  }

  for (auto &transaction : transactions) {
    transaction.session_id = remap.at(transaction.session_id);
  }
}

TraceStats trace_stats(const std::vector<Transaction> &transactions) {
  std::set<std::uint64_t> sessions;
  std::size_t events = 0;
  for (const auto &transaction : transactions) {
    sessions.insert(transaction.session_id);
    events += transaction.events.size();
  }
  return TraceStats{.sessions     = sessions.size(),
                    .transactions = transactions.size(),
                    .events       = events};
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
  return path.parent_path() / (stem + path.extension().string());
}

void emit_trace(std::ostream &output,
                const std::vector<Transaction> &transactions) {
  YAML::Emitter emitter;
  emitter.SetIndent(2);
  for (const auto &transaction : transactions) {
    emitter << YAML::BeginDoc;
    emitter << YAML::BeginMap;
    emitter << YAML::Key << "session_id" << YAML::Value
            << transaction.session_id;
    emitter << YAML::Key << "start_ts" << YAML::Value << transaction.start_ts;
    emitter << YAML::Key << "commit_ts" << YAML::Value << transaction.commit_ts;
    emitter << YAML::Key << "events" << YAML::Value << YAML::BeginSeq;
    for (const auto &event : transaction.events) {
      emitter << YAML::BeginMap;
      emitter << YAML::Key << "seq" << YAML::Value << event.seq;
      emitter << YAML::Key << "op" << YAML::Value << event.op;
      emitter << YAML::Key << "key_hex" << YAML::Value << event.key_hex;
      if (event.value_hex.has_value()) {
        emitter << YAML::Key << "value_hex" << YAML::Value << *event.value_hex;
      }
      if (event.is_delete) {
        emitter << YAML::Key << "delete" << YAML::Value << true;
      }
      if (event.is_miss) {
        emitter << YAML::Key << "miss" << YAML::Value << true;
      }
      emitter << YAML::EndMap;
    }
    emitter << YAML::EndSeq;
    emitter << YAML::EndMap;
  }
  output << emitter.c_str() << '\n';
}

} // namespace

int main(const int argc, char **argv) {
  try {
    const auto config = parse_args(argc, argv);

    std::ifstream input{config.input};
    if (!input.is_open()) {
      throw std::runtime_error("failed to open input `" +
                               config.input.string() + "`");
    }

    auto transactions = read_committed_transactions(input);
    normalize_session_ids(transactions);

    if (!config.output.has_value()) {
      emit_trace(std::cout, transactions);
      return 0;
    }

    auto output_path = *config.output;
    if (config.rename) {
      output_path = path_with_stats(output_path, trace_stats(transactions));
    }

    if (!output_path.parent_path().empty()) {
      std::filesystem::create_directories(output_path.parent_path());
    }
    const auto temp_path = output_path.string() + ".tmp";
    {
      std::ofstream output{temp_path};
      if (!output.is_open()) {
        throw std::runtime_error("failed to open output `" + temp_path + "`");
      }
      emit_trace(output, transactions);
    }
    std::filesystem::rename(temp_path, output_path);
    if (config.rename) {
      std::cout << output_path.string() << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
