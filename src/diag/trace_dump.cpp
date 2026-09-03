#include "common/diag/trace_dump.h"

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace common {

const char *to_string(Severity severity) {
  switch (severity) {
  case Severity::Info:
    return "info";
  case Severity::Warning:
    return "warning";
  case Severity::Error:
    return "error";
  }
  return "unknown";
}

void print_diagnostic(std::ostream &os, const Diagnostic &diagnostic) {
  os << to_string(diagnostic.severity) << ": ";
  if (!diagnostic.checker.empty()) {
    os << diagnostic.checker << ": ";
  }
  os << diagnostic.message;

  if (diagnostic.txn_id.has_value()) {
    os << " txn=" << *diagnostic.txn_id;
  }
  if (diagnostic.event_id.has_value()) {
    os << " event=" << *diagnostic.event_id;
  }
  if (diagnostic.event_seq.has_value()) {
    os << " seq=" << *diagnostic.event_seq;
  }
  if (diagnostic.key.has_value()) {
    os << " key=" << diagnostic.key->to_hex();
  }
  if (diagnostic.key_id.has_value()) {
    os << " key_id=" << *diagnostic.key_id;
  }
  os << '\n';
}

void print_diagnostics(std::ostream &os, const DiagnosticList &diagnostics) {
  for (const auto &diagnostic : diagnostics) {
    print_diagnostic(os, diagnostic);
  }
}

void dump_parsed_trace(std::ostream &os, const Trace &trace) {
  os << "transactions:\n";
  for (const auto &txn : trace.transactions) {
    txn.print(os, "  ");
  }

  os << "events:\n";
  for (const auto &event : trace.events) {
    event.print(os, "  ");
  }
}

void dump_versions(std::ostream &os, const TraceIndex &index) {
  std::vector<std::pair<std::string, const std::vector<Version> *>> keys;
  keys.reserve(index.versions_by_key.size());
  for (const auto &[key, versions] : index.versions_by_key) {
    keys.emplace_back(key.to_hex(), &versions);
  }
  std::sort(keys.begin(), keys.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.first < rhs.first;
  });

  os << "versions:\n";
  for (const auto &[key_hex, versions] : keys) {
    os << "  - key_hex: " << key_hex << '\n';
    os << "    versions:\n";
    for (const auto &version : *versions) {
      os << "      - writer: " << version.writer << '\n';
      os << "        write_event: " << version.write_event << '\n';
      os << "        commit_ts: " << version.commit_ts << '\n';
      if (version.is_delete) {
        os << "        delete: true\n";
      } else if (version.value.has_value()) {
        os << "        value_hex: " << version.value->to_hex() << '\n';
      }
    }
  }
}

} // namespace common
