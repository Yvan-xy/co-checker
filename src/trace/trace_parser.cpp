#include "common/trace/trace_parser.h"

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace common {

namespace {

Diagnostic parse_error(std::string code, std::string message) {
  Diagnostic diagnostic;
  diagnostic.severity = Severity::Error;
  diagnostic.checker  = "parser";
  diagnostic.code     = std::move(code);
  diagnostic.message  = std::move(message);
  return diagnostic;
}

Result<Trace> single_error(std::string code, std::string message) {
  DiagnosticList diagnostics;
  diagnostics.push_back(parse_error(std::move(code), std::move(message)));
  return Result<Trace>::err(std::move(diagnostics));
}

std::string
location(const std::size_t document_index,
         const std::optional<std::size_t> event_index = std::nullopt,
         const std::string_view field                 = {}) {
  std::ostringstream out;
  out << "document " << document_index;
  if (event_index.has_value()) {
    out << ", event " << *event_index;
  }
  if (!field.empty()) {
    out << ", field `" << field << '`';
  }
  return out.str();
}

bool require_map(const YAML::Node &node, const std::size_t document_index,
                 DiagnosticList &diagnostics) {
  if (!node.IsMap()) {
    diagnostics.push_back(
        parse_error("parse.invalid_document",
                    location(document_index) + " must be a mapping"));
    return false;
  }
  return true;
}

YAML::Node required_field(const YAML::Node &node, const std::string &field,
                          const std::size_t document_index,
                          DiagnosticList &diagnostics) {
  auto child = node[field];
  if (!child) {
    diagnostics.push_back(parse_error(
        "parse.missing_field",
        location(document_index, std::nullopt, field) + " is required"));
  }
  return child;
}

YAML::Node required_event_field(const YAML::Node &node,
                                const std::string &field,
                                const std::size_t document_index,
                                const std::size_t event_index,
                                DiagnosticList &diagnostics) {
  auto child = node[field];
  if (!child) {
    diagnostics.push_back(parse_error(
        "parse.missing_field",
        location(document_index, event_index, field) + " is required"));
  }
  return child;
}

template <typename T>
std::optional<T> parse_scalar(const YAML::Node &node, const std::string &field,
                              const std::size_t document_index,
                              DiagnosticList &diagnostics) {
  try {
    return node.as<T>();
  } catch (const YAML::Exception &error) {
    diagnostics.push_back(parse_error(
        "parse.invalid_type", location(document_index, std::nullopt, field) +
                                  " has invalid type: " + error.what()));
    return std::nullopt;
  }
}

template <typename T>
std::optional<T>
parse_event_scalar(const YAML::Node &node, const std::string &field,
                   const std::size_t document_index,
                   const std::size_t event_index, DiagnosticList &diagnostics) {
  try {
    return node.as<T>();
  } catch (const YAML::Exception &error) {
    diagnostics.push_back(parse_error(
        "parse.invalid_type", location(document_index, event_index, field) +
                                  " has invalid type: " + error.what()));
    return std::nullopt;
  }
}

std::optional<HexValue> parse_hex_field(const YAML::Node &node,
                                        const std::string &field,
                                        const std::size_t document_index,
                                        const std::size_t event_index,
                                        DiagnosticList &diagnostics) {
  if (!node.IsScalar()) {
    diagnostics.push_back(parse_error(
        "parse.invalid_type",
        location(document_index, event_index, field) + " must be a scalar"));
    return std::nullopt;
  }

  const auto hex = node.Scalar();
  std::string error;
  auto value = HexValue::from_hex(hex, &error);
  if (!value.has_value()) {
    diagnostics.push_back(parse_error(
        "parse.invalid_hex", location(document_index, event_index, field) +
                                 " is invalid: " + error));
    return std::nullopt;
  }
  return value;
}

std::optional<EventOp> parse_op(const YAML::Node &node,
                                const std::size_t document_index,
                                const std::size_t event_index,
                                DiagnosticList &diagnostics) {
  const auto op = parse_event_scalar<std::string>(node, "op", document_index,
                                                  event_index, diagnostics);
  if (!op.has_value()) {
    return std::nullopt;
  }
  if (*op == "R") {
    return EventOp::Read;
  }
  if (*op == "W") {
    return EventOp::Write;
  }
  diagnostics.push_back(parse_error(
      "parse.invalid_op",
      location(document_index, event_index, "op") + " must be `R` or `W`"));
  return std::nullopt;
}

} // namespace

TraceParser::TraceParser(ParseOptions options) : options_(options) {}

Result<Trace> TraceParser::parse_file(std::string_view path) const {
  std::ifstream input{std::string(path)};
  if (!input.is_open()) {
    return single_error("parse.open_failed", "failed to open trace file `" +
                                                 std::string(path) + "`");
  }
  return parse_stream(input);
}

Result<Trace> TraceParser::parse_stream(std::istream &input) const {
  std::ostringstream buffer;
  buffer << input.rdbuf();

  std::vector<YAML::Node> documents;
  try {
    documents = YAML::LoadAll(buffer.str());
  } catch (const YAML::Exception &error) {
    return single_error("parse.yaml_error", error.what());
  }

  Trace trace;
  DiagnosticList diagnostics;
  std::unordered_set<Timestamp> seen_commit_ts;
  std::optional<Timestamp> previous_commit_ts;

  for (std::size_t document_index = 0; document_index < documents.size();
       ++document_index) {
    const auto &document = documents[document_index];
    if (!document || document.IsNull()) {
      continue;
    }
    if (!require_map(document, document_index, diagnostics)) {
      continue;
    }

    const auto session_node =
        required_field(document, "session_id", document_index, diagnostics);
    const auto start_node =
        required_field(document, "start_ts", document_index, diagnostics);
    const auto commit_node =
        required_field(document, "commit_ts", document_index, diagnostics);
    const auto events_node =
        required_field(document, "events", document_index, diagnostics);
    if (!diagnostics.empty()) {
      break;
    }

    auto session_id = parse_scalar<SessionId>(session_node, "session_id",
                                              document_index, diagnostics);
    auto start_ts   = parse_scalar<Timestamp>(start_node, "start_ts",
                                              document_index, diagnostics);
    auto commit_ts  = parse_scalar<Timestamp>(commit_node, "commit_ts",
                                              document_index, diagnostics);
    if (!session_id.has_value() || !start_ts.has_value() ||
        !commit_ts.has_value()) {
      break;
    }

    if (!events_node.IsSequence()) {
      diagnostics.push_back(
          parse_error("parse.invalid_type",
                      location(document_index, std::nullopt, "events") +
                          " must be a sequence"));
      break;
    }

    if (previous_commit_ts.has_value() && options_.require_commit_order &&
        *commit_ts < *previous_commit_ts) {
      diagnostics.push_back(
          parse_error("parse.commit_order",
                      location(document_index, std::nullopt, "commit_ts") +
                          " is smaller than previous commit_ts"));
      break;
    }

    if (options_.reject_duplicate_commit_ts &&
        !seen_commit_ts.insert(*commit_ts).second) {
      diagnostics.push_back(
          parse_error("parse.duplicate_commit_ts",
                      location(document_index, std::nullopt, "commit_ts") +
                          " duplicates an earlier transaction"));
      break;
    }

    Transaction txn;
    txn.id         = static_cast<TxnId>(trace.transactions.size());
    txn.session_id = *session_id;
    txn.start_ts   = *start_ts;
    txn.commit_ts  = *commit_ts;

    std::optional<EventSeq> previous_seq;
    for (std::size_t event_index = 0; event_index < events_node.size();
         ++event_index) {
      const auto event_node = events_node[event_index];
      if (!event_node.IsMap()) {
        diagnostics.push_back(parse_error(
            "parse.invalid_event",
            location(document_index, event_index) + " must be a mapping"));
        break;
      }

      const auto seq_node = required_event_field(
          event_node, "seq", document_index, event_index, diagnostics);
      const auto op_node = required_event_field(
          event_node, "op", document_index, event_index, diagnostics);
      const auto key_node = required_event_field(
          event_node, "key_hex", document_index, event_index, diagnostics);
      if (!diagnostics.empty()) {
        break;
      }

      auto seq = parse_event_scalar<EventSeq>(seq_node, "seq", document_index,
                                              event_index, diagnostics);
      auto op  = parse_op(op_node, document_index, event_index, diagnostics);
      auto key = parse_hex_field(key_node, "key_hex", document_index,
                                 event_index, diagnostics);
      if (!seq.has_value() || !op.has_value() || !key.has_value()) {
        break;
      }

      if (previous_seq.has_value() && *seq <= *previous_seq) {
        diagnostics.push_back(parse_error(
            "parse.invalid_seq", location(document_index, event_index, "seq") +
                                     " must be strictly increasing"));
        break;
      }
      previous_seq = *seq;

      Event event;
      event.id     = static_cast<EventId>(trace.events.size());
      event.txn_id = txn.id;
      event.seq    = *seq;
      event.op     = *op;
      event.key    = std::move(*key);

      if (const auto value_node = event_node["value_hex"]) {
        auto value = parse_hex_field(value_node, "value_hex", document_index,
                                     event_index, diagnostics);
        if (!value.has_value()) {
          break;
        }
        event.value = std::move(*value);
      }

      if (const auto delete_node = event_node["delete"]) {
        auto is_delete = parse_event_scalar<bool>(
            delete_node, "delete", document_index, event_index, diagnostics);
        if (!is_delete.has_value()) {
          break;
        }
        event.is_delete = *is_delete;
      }

      if (const auto miss_node = event_node["miss"]) {
        auto is_miss = parse_event_scalar<bool>(
            miss_node, "miss", document_index, event_index, diagnostics);
        if (!is_miss.has_value()) {
          break;
        }
        event.is_miss = *is_miss;
      }

      if (event.is_miss && event.value.has_value()) {
        diagnostics.push_back(parse_error(
            "parse.invalid_read", location(document_index, event_index) +
                                      " cannot have both miss and value_hex"));
        break;
      }

      if (event.is_delete && event.value.has_value()) {
        diagnostics.push_back(
            parse_error("parse.invalid_write",
                        location(document_index, event_index) +
                            " cannot have both delete and value_hex"));
        break;
      }

      if (event.op == EventOp::Read && event.is_delete) {
        diagnostics.push_back(parse_error(
            "parse.invalid_read", location(document_index, event_index) +
                                      " read event cannot have delete"));
        break;
      }

      if (event.op == EventOp::Write && event.is_miss) {
        diagnostics.push_back(parse_error(
            "parse.invalid_write", location(document_index, event_index) +
                                       " write event cannot have miss"));
        break;
      }

      txn.event_ids.push_back(event.id);
      trace.events.push_back(std::move(event));
    }

    if (!diagnostics.empty()) {
      break;
    }

    trace.transactions.push_back(std::move(txn));
    previous_commit_ts = *commit_ts;
  }

  if (!diagnostics.empty()) {
    return Result<Trace>::err(std::move(diagnostics));
  }

  return Result<Trace>::ok(std::move(trace));
}

} // namespace common
