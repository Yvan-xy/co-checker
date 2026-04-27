#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/trace/types.h"

namespace common {

struct Snapshot {
  std::unordered_map<Key, Value, HexValueHash> data;

  void copy_from(const Snapshot &snapshot) {
    if (this == &snapshot) {
      return;
    }
    data.clear();
    data.reserve(snapshot.data.size());
    data.insert(snapshot.data.begin(), snapshot.data.end());
  }

  [[nodiscard]] Snapshot clone() const {
    Snapshot snapshot;
    snapshot.copy_from(*this);
    return snapshot;
  }

  [[nodiscard]] bool has_key(const Key &key) const {
    return data.find(key) != data.end();
  }

  [[nodiscard]] std::optional<Value> get(const Key &key) const {
    auto it = data.find(key);
    if (it == data.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  void put(const Key &key, const Value &value) { data[key] = value; }

  void erase(const Key &key) { data.erase(key); }

  void set(const Key &key, const std::optional<Value> &value) {
    if (value.has_value()) {
      data[key] = *value;
    } else {
      data.erase(key);
    }
  }

  [[nodiscard]] std::string to_string(std::string_view indent = "") const;
  void print(std::ostream &os, std::string_view indent = "") const;
  void print() const;
};

typedef std::vector<Snapshot> SnapshotVector;
template <typename T, typename H = std::hash<T>>
using SnapshotMap = std::unordered_map<T, Snapshot, H>;
template <typename T, typename H = std::hash<T>>
using SnapshotVectorMap = std::unordered_map<T, SnapshotVector, H>;

namespace detail {

template <typename T>
[[nodiscard]] std::string snapshot_owner_to_string(const T &owner) {
  std::ostringstream out;
  out << owner;
  return out.str();
}

} // namespace detail

template <typename T, typename H>
void print_snapshot_map(std::ostream &os, const SnapshotMap<T, H> &snapshots) {
  std::vector<std::pair<std::string, const Snapshot *>> entries;
  entries.reserve(snapshots.size());
  for (const auto &[owner, snapshot] : snapshots) {
    entries.emplace_back(detail::snapshot_owner_to_string(owner), &snapshot);
  }
  std::sort(
      entries.begin(), entries.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

  os << "snapshot_map:\n";
  for (const auto &[owner, snapshot] : entries) {
    os << "  - owner: " << owner << '\n';
    snapshot->print(os, "    ");
  }
}

template <typename T, typename H>
void print_snapshot_vector_map(
    std::ostream &os, const SnapshotVectorMap<T, H> &snapshot_vectors) {
  std::vector<std::pair<std::string, const SnapshotVector *>> entries;
  entries.reserve(snapshot_vectors.size());
  for (const auto &[owner, snapshots] : snapshot_vectors) {
    entries.emplace_back(detail::snapshot_owner_to_string(owner), &snapshots);
  }
  std::sort(
      entries.begin(), entries.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

  os << "snapshot_vector_map:\n";
  for (const auto &[owner, snapshots] : entries) {
    os << "  - owner: " << owner << '\n';
    os << "    snapshots:\n";
    for (std::size_t i = 0; i < snapshots->size(); ++i) {
      os << "      - index: " << i << '\n';
      (*snapshots)[i].print(os, "        ");
    }
  }
}

// to keep the record of Read(null)/Write(null) in Snapshot
struct SnapshotOpt {
  std::unordered_map<Key, std::optional<Value>, HexValueHash> data;

  [[nodiscard]] SnapshotOpt clone() const {
    SnapshotOpt snapshot;
    snapshot.data.reserve(data.size());
    snapshot.data.insert(data.begin(), data.end());
    return snapshot;
  }

  [[nodiscard]] std::optional<Value> get(const Key &key) const {
    auto it = data.find(key);
    if (it == data.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  [[nodiscard]] bool has_key(const Key &key) const {
    return data.find(key) != data.end();
  }

  void set(const Key &key, const std::optional<Value> &value) {
    data[key] = value;
  }

  [[nodiscard]] std::string to_string(std::string_view indent = "") const;
  void print(std::ostream &os, std::string_view indent = "") const;
  void print() const;
};

} // namespace common
