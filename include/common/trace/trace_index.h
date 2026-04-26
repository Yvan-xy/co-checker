#pragma once

#include <unordered_map>
#include <vector>

#include "common/diag/result.h"
#include "common/trace/trace.h"

namespace common {

struct Version {
  TxnId writer        = 0;
  EventId write_event = 0;
  Timestamp commit_ts = 0;
  Key key;
  std::optional<Value> value;
  bool is_delete = false;
};

struct Snapshot {
  std::unordered_map<Key, Value, HexValueHash> data;
  std::optional<Value> operator[](const Key &key) const {
    auto it = data.find(key);
    if (it == data.end()) {
      return std::nullopt;
    }
    return it->second;
  }
};

struct TraceIndex {
  std::unordered_map<Key, std::vector<Version>, HexValueHash> versions_by_key;
  std::unordered_map<Timestamp, TxnId> txn_by_commit_ts;
};

class TraceIndexBuilder {
public:
  [[nodiscard]] Result<TraceIndex> build(const Trace &trace) const;
};

typedef std::vector<Snapshot> SnapshotVector;
template <typename T, typename H = std::hash<T>>
using SnapshotMap = std::unordered_map<T, Snapshot, H>;
template <typename T, typename H = std::hash<T>>
using SnapshotVectorMap = std::unordered_map<T, SnapshotVector, H>;

} // namespace common
