#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace co_checker::online {

using ByteView    = std::span<const std::byte>;
using CommitSeq   = std::uint64_t;
using EventSeq    = std::uint32_t;
using ProducerId  = std::uint32_t;
using ProducerSeq = std::uint64_t;

struct RuntimeEpoch {
  std::array<std::uint64_t, 2> words{};

  friend bool operator==(const RuntimeEpoch &, const RuntimeEpoch &) = default;
};

struct EntityUid {
  std::array<std::uint64_t, 2> words{};

  friend bool operator==(const EntityUid &, const EntityUid &) = default;
};

using TransactionUid        = EntityUid;
using TransactionAttemptUid = EntityUid;

struct SessionUid {
  RuntimeEpoch runtime_epoch;
  ProducerId producer_id              = 0;
  std::uint64_t local_session_counter = 0;

  friend bool operator==(const SessionUid &, const SessionUid &) = default;
};

struct CommitOrderToken {
  std::uint64_t cluster_epoch = 0;
  std::uint64_t timestamp     = 0;
  std::optional<std::uint64_t> authoritative_tie_breaker;

  friend bool operator==(const CommitOrderToken &,
                         const CommitOrderToken &) = default;
};

struct OrderedTxnMeta {
  CommitSeq commit_seq = 0;
  TransactionUid transaction_id;
  TransactionAttemptUid attempt_id;
  SessionUid session_id;
  std::uint64_t session_seq = 0;
  CommitOrderToken commit_token;
  ProducerId producer_id   = 0;
  ProducerSeq producer_seq = 0;
};

enum class ValueState : std::uint8_t {
  Present = 1,
  Absent  = 2,
};

enum class ReadSource : std::uint8_t {
  Snapshot = 1,
  Local    = 2,
};

enum class EventOp : std::uint8_t {
  Read  = 1,
  Write = 2,
};

struct EventView {
  EventSeq sequence      = 0;
  EventOp op             = EventOp::Read;
  ValueState value_state = ValueState::Absent;
  ReadSource read_source = ReadSource::Snapshot;
  ByteView key;
  ByteView value;
};

} // namespace co_checker::online
