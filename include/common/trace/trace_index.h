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

struct TraceIndex {
  std::unordered_map<Key, std::vector<Version>, HexValueHash> versions_by_key;
  std::unordered_map<Timestamp, TxnId> txn_by_commit_ts;
  std::unordered_map<SessionId, std::vector<TxnId>> txns_by_session;
};

class TraceIndexBuilder {
public:
  [[nodiscard]] Result<TraceIndex> build(const Trace &trace) const;
};

} // namespace common
