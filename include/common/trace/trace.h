#pragma once

#include <cassert>
#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
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

  [[nodiscard]] std::string to_string(std::string_view indent = "") const;
  void print(std::ostream &os, std::string_view indent = "") const;
  void print() const;
};

struct Transaction {
  TxnId id             = 0;
  SessionId session_id = 0;
  Timestamp start_ts   = 0;
  Timestamp commit_ts  = 0;
  std::vector<EventId> event_ids;

  [[nodiscard]] std::string to_string(std::string_view indent = "") const;
  void print(std::ostream &os, std::string_view indent = "") const;
  void print() const;
};

struct Trace {
  std::vector<Transaction> transactions;
  std::vector<Event> events;
  std::vector<KeyId> event_key_ids;
  std::vector<ValueId> event_value_ids;
  std::size_t key_count   = 0;
  std::size_t value_count = 0;

  const Transaction &getTxn(const TxnId txn_id) const {
    assert(txn_id < transactions.size());
    return transactions[txn_id];
  }

  const Event &getEvent(const EventId event_id) const {
    assert(event_id < events.size());
    return events[event_id];
  }

  [[nodiscard]] KeyId getEventKeyId(const EventId event_id) const {
    assert(event_id < event_key_ids.size());
    return event_key_ids[event_id];
  }

  [[nodiscard]] ValueId getEventValueId(const EventId event_id) const {
    assert(event_id < event_value_ids.size());
    return event_value_ids[event_id];
  }
};

inline bool isRead(const Event &event) { return event.op == EventOp::Read; }

inline bool isWrite(const Event &event) { return event.op == EventOp::Write; }

inline bool readSameKey(const Event &e1, const Event &e2) {
  return isRead(e1) && isRead(e2) && e1.key == e2.key;
}

inline bool isMiss(const Event &event) {
  assert(isRead(event));
  return event.is_miss;
}

inline bool readSameValue(const Event &e1, const Event &e2) {
  bool res = isRead(e1) && isRead(e2);
  // both miss or both not miss
  res = res && !(isMiss(e1) ^ isMiss(e2));
  // if not miss, values must be equal
  res = res && (isMiss(e1) || e1.value == e2.value);
  return res;
}

inline bool isDelete(const Event &event) {
  assert(isWrite(event));
  return event.is_delete;
}

inline bool writeSameKey(const Event &e1, const Event &e2) {
  return isWrite(e1) && isWrite(e2) && e1.key == e2.key;
}

} // namespace common
