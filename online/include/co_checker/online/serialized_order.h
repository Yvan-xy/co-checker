#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "co_checker/online/packed_batch.h"
#include "co_checker/online/types.h"

namespace co_checker::online {

enum class SerializedOrderErrorCode : std::uint8_t {
  EpochMismatch,
  InvalidPublicationSequence,
  PublicationSequenceExhausted,
  InvalidToken,
  InvalidTokenKind,
  InvalidIdentity,
  DuplicateAttempt,
  DuplicateTransaction,
  SessionOrderRegression,
  CommitSequenceExhausted,
};

struct SerializedOrderError {
  SerializedOrderErrorCode code =
      SerializedOrderErrorCode::InvalidPublicationSequence;
  std::string message;

  friend bool operator==(const SerializedOrderError &,
                         const SerializedOrderError &) = default;
};

struct SerializedOrderResult {
  std::optional<OrderedTxnMeta> ordered_meta;
  std::optional<SerializedOrderError> error;

  [[nodiscard]] bool ok() const noexcept {
    return ordered_meta.has_value();
  }
};

struct SerializedOrderMetrics {
  PublicationSeq next_publication_seq = 1;
  CommitSeq next_commit_seq            = 0;
  std::uint64_t accepted_transactions  = 0;
  std::optional<CommitOrderToken> last_token;
};

class SerializedOrderValidator final {
public:
  SerializedOrderValidator(RuntimeEpoch expected_runtime_epoch,
                           std::uint64_t expected_cluster_epoch,
                           PublicationSeq first_publication_seq = 1,
                           CommitSeq first_commit_seq = 0);

  [[nodiscard]] SerializedOrderResult
  accept(const OrderedTxnMeta &captured_meta,
         std::size_t normalized_write_count);

  [[nodiscard]] SerializedOrderMetrics metrics() const noexcept;

private:
  struct EntityLess {
    [[nodiscard]] bool operator()(const EntityUid &lhs,
                                  const EntityUid &rhs) const noexcept {
      return lhs.words < rhs.words;
    }
  };

  struct SessionLess {
    [[nodiscard]] bool operator()(const SessionUid &lhs,
                                  const SessionUid &rhs) const noexcept;
  };

  [[nodiscard]] static SerializedOrderResult
  failure(SerializedOrderErrorCode code, std::string message);

  RuntimeEpoch expected_runtime_epoch_;
  std::uint64_t expected_cluster_epoch_ = 0;
  PublicationSeq next_publication_seq_  = 1;
  CommitSeq next_commit_seq_            = 0;
  bool publication_sequence_exhausted_  = false;
  std::optional<CommitOrderToken> last_token_;
  std::set<EntityUid, EntityLess> accepted_attempts_;
  std::set<EntityUid, EntityLess> accepted_transactions_;
  std::map<SessionUid, std::uint64_t, SessionLess> last_session_seq_;
};

} // namespace co_checker::online
