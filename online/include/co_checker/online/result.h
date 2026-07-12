#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "co_checker/online/types.h"

namespace co_checker::online {

using OwnedBytes = std::vector<std::byte>;

enum class ViolationKind : std::uint8_t {
  NoCompatibleSnapshot,
  InconsistentRepeatedRead,
};

struct Violation {
  ViolationKind kind   = ViolationKind::NoCompatibleSnapshot;
  CommitSeq commit_seq = 0;
  TransactionUid transaction_id;
  SessionUid session_id;
  std::uint64_t session_seq = 0;
  std::optional<CommitSeq> session_lower_bound;
  std::optional<CommitSeq> write_write_lower_bound;
  std::optional<CommitSeq> effective_lower_bound;
  std::optional<EventSeq> first_event_sequence;
  std::optional<EventSeq> conflicting_event_sequence;
  std::vector<OwnedBytes> keys;

  friend bool operator==(const Violation &, const Violation &) = default;
};

enum class ConsistencyStatus : std::uint8_t {
  Pass,
  Violation,
};

struct ConsistencyVerdict {
  ConsistencyStatus status = ConsistencyStatus::Pass;
  std::optional<Violation> first_violation;

  friend bool operator==(const ConsistencyVerdict &,
                         const ConsistencyVerdict &) = default;
};

enum class EngineErrorCode : std::uint8_t {
  UnexpectedCommitSequence,
  NonIncreasingSessionSequence,
  NonIncreasingEventSequence,
  InvalidEventOperation,
  InvalidValueState,
  InvalidReadSource,
  AbsentValueHasPayload,
  RetainedStateLimitExceeded,
  ResourceExhausted,
};

struct EngineError {
  EngineErrorCode code = EngineErrorCode::UnexpectedCommitSequence;
  std::string message;
  std::optional<CommitSeq> commit_seq;
  std::optional<EventSeq> event_sequence;

  friend bool operator==(const EngineError &, const EngineError &) = default;
};

// Exactly one of verdict or error is populated. A result carrying an error
// never carries Pass, because malformed or incomplete evidence cannot justify
// a consistency verdict.
struct ConsumeResult {
  std::optional<ConsistencyVerdict> verdict;
  std::optional<Violation> transaction_violation;
  std::optional<EngineError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

} // namespace co_checker::online
