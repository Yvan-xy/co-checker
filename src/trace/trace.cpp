#include "common/trace/trace.h"
#include "common/trace/snapshot.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

namespace common {

std::string Event::to_string(std::string_view indent) const {
  std::ostringstream out;
  out << indent << "- id: " << id << '\n';
  out << indent << "  txn_id: " << txn_id << '\n';
  out << indent << "  seq: " << seq << '\n';
  out << indent << "  op: " << (op == EventOp::Read ? "R" : "W") << '\n';
  out << indent << "  key_hex: " << key.to_hex() << '\n';
  if (value.has_value()) {
    out << indent << "  value_hex: " << value->to_hex() << '\n';
  }
  if (is_delete) {
    out << indent << "  delete: true\n";
  }
  if (is_miss) {
    out << indent << "  miss: true\n";
  }
  return out.str();
}

void Event::print(std::ostream &os, std::string_view indent) const {
  os << to_string(indent);
}

void Event::print() const { print(std::cout); }

std::string Transaction::to_string(std::string_view indent) const {
  std::ostringstream out;
  out << indent << "- id: " << id << '\n';
  out << indent << "  session_id: " << session_id << '\n';
  out << indent << "  start_ts: " << start_ts << '\n';
  out << indent << "  commit_ts: " << commit_ts << '\n';
  out << indent << "  event_ids:";
  for (const auto event_id : event_ids) {
    out << ' ' << event_id;
  }
  out << '\n';
  return out.str();
}

void Transaction::print(std::ostream &os, std::string_view indent) const {
  os << to_string(indent);
}

void Transaction::print() const { print(std::cout); }

std::string Snapshot::to_string(std::string_view indent) const {
  std::vector<std::pair<std::string, const Value *>> entries;
  entries.reserve(data.size());
  for (const auto &[key, value] : data) {
    entries.emplace_back(key.to_hex(), &value);
  }
  std::sort(
      entries.begin(), entries.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

  std::ostringstream out;
  out << indent << "snapshot:\n";
  for (const auto &[key_hex, value] : entries) {
    out << indent << "  - key_hex: " << key_hex << '\n';
    out << indent << "    value_hex: " << value->to_hex() << '\n';
  }
  return out.str();
}

void Snapshot::print(std::ostream &os, std::string_view indent) const {
  os << to_string(indent);
}

void Snapshot::print() const { print(std::cout); }

std::string SnapshotOpt::to_string(std::string_view indent) const {
  std::vector<std::pair<std::string, const std::optional<Value> *>> entries;
  entries.reserve(data.size());
  for (const auto &[key, value] : data) {
    entries.emplace_back(key.to_hex(), &value);
  }
  std::sort(
      entries.begin(), entries.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

  std::ostringstream out;
  out << indent << "snapshot_opt:\n";
  for (const auto &[key_hex, value] : entries) {
    out << indent << "  - key_hex: " << key_hex << '\n';
    if (value->has_value()) {
      out << indent << "    value_hex: " << (*value)->to_hex() << '\n';
    } else {
      out << indent << "    value: null\n";
    }
  }
  return out.str();
}

void SnapshotOpt::print(std::ostream &os, std::string_view indent) const {
  os << to_string(indent);
}

void SnapshotOpt::print() const { print(std::cout); }

} // namespace common
