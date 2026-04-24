#pragma once

#include <optional>
#include <vector>

#include "common/trace/types.h"

namespace common {

enum class EventOp {
  Read,
  Write,
};

struct Event {
  EventId id   = 0;
  TxnId txn_id = 0;
  EventSeq seq = 0;
  EventOp op   = EventOp::Read;
  Key key;
  std::optional<Value> value;
  bool is_delete = false;
  bool is_miss   = false;
};

struct Transaction {
  TxnId id             = 0;
  SessionId session_id = 0;
  Timestamp start_ts   = 0;
  Timestamp commit_ts  = 0;
  std::vector<EventId> event_ids;
};

struct Trace {
  std::vector<Transaction> transactions;
  std::vector<Event> events;
};

} // namespace common
