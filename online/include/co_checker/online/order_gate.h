#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "co_checker/online/types.h"

namespace co_checker::online {

enum class TokenRelation : std::uint8_t {
  Less,
  Equal,
  Greater,
  Ambiguous,
};

[[nodiscard]] TokenRelation
compareCommitOrderTokens(const CommitOrderToken &lhs,
                         const CommitOrderToken &rhs) noexcept;

enum class OrderGateErrorCode : std::uint8_t {
  UnknownProducer,
  ProducerSequenceGap,
  DuplicateToken,
  AmbiguousToken,
  LateTransaction,
  DuplicateTransaction,
  DuplicateAttempt,
  DuplicateActiveStart,
  MissingActiveStart,
  LateActiveStart,
  ActiveProducerMismatch,
  InvalidCommitToken,
  NonIncreasingSessionSequence,
  NonIncreasingSessionToken,
  ClusterEpochMismatch,
  FrontierRegression,
  DeliveryCutoffMissing,
  ByteAccountingOverflow,
  CommitSequenceExhausted,
  ResourceExhausted,
  ProtocolStopped,
};

struct OrderGateError {
  OrderGateErrorCode code = OrderGateErrorCode::ProtocolStopped;
  std::string message;

  friend bool operator==(const OrderGateError &,
                         const OrderGateError &) = default;
};

struct PendingTransaction {
  OrderedTxnMeta meta;
  bool read_only             = false;
  std::size_t reserved_bytes = 0;
  std::shared_ptr<const void> owner;
};

struct ReleasedTransaction {
  OrderedTxnMeta meta;
  bool read_only             = false;
  std::size_t reserved_bytes = 0;
  std::shared_ptr<const void> owner;
};

struct OrderGateMetrics {
  std::size_t pending_transactions        = 0;
  std::size_t pending_reserved_bytes      = 0;
  std::size_t peak_pending_transactions   = 0;
  std::size_t peak_pending_reserved_bytes = 0;
  std::size_t active_starts               = 0;
  CommitSeq next_commit_seq               = 0;
  std::optional<CommitOrderToken> last_released_token;
};

// Thread-safe commit-order closure gate. All producer control messages still
// need to be delivered losslessly; the mutex provides the acknowledged
// linearization point required by active-start registration.
class CommitOrderGate {
public:
  explicit CommitOrderGate(std::vector<ProducerId> required_producers);

  // Registration is an acknowledged linearization point. A registration that
  // follows an already-announced database or same-producer delivery frontier
  // must be strictly above it. A registration that linearized first may be
  // overtaken by later frontier announcements; its active guard then blocks
  // release until commit insertion or abort.
  [[nodiscard]] std::optional<OrderGateError>
  registerActiveStart(const TransactionAttemptUid &attempt_id,
                      ProducerId producer_id,
                      const CommitOrderToken &start_token);

  [[nodiscard]] std::optional<OrderGateError>
  abortActiveStart(const TransactionAttemptUid &attempt_id);

  // Inserts a committed batch before removing its active-start guard in one
  // critical section. This ordering is required for read-only transactions.
  [[nodiscard]] std::optional<OrderGateError>
  submitCommitted(PendingTransaction transaction);

  [[nodiscard]] std::optional<OrderGateError>
  updateDatabaseFrontier(const CommitOrderToken &frontier);

  [[nodiscard]] std::optional<OrderGateError>
  updateProducerDeliveryFrontier(ProducerId producer_id,
                                 const CommitOrderToken &frontier,
                                 ProducerSeq delivered_sequence);

  [[nodiscard]] std::vector<ReleasedTransaction> releaseEligible();

  [[nodiscard]] std::optional<OrderGateError> error() const;
  [[nodiscard]] OrderGateMetrics metrics() const;

private:
  struct TokenLess {
    [[nodiscard]] bool operator()(const CommitOrderToken &lhs,
                                  const CommitOrderToken &rhs) const noexcept;
  };

  struct EntityLess {
    [[nodiscard]] bool operator()(const EntityUid &lhs,
                                  const EntityUid &rhs) const noexcept;
  };

  struct SessionLess {
    [[nodiscard]] bool operator()(const SessionUid &lhs,
                                  const SessionUid &rhs) const noexcept;
  };

  struct ActiveStartState {
    CommitOrderToken token;
    ProducerId producer_id = 0;
  };

  struct SessionCommitState {
    std::uint64_t session_seq = 0;
    CommitOrderToken commit_token;
  };

  struct ProducerState {
    ProducerSeq last_received_sequence = 0;
    ProducerSeq delivery_cutoff        = 0;
    std::optional<CommitOrderToken> delivery_frontier;
  };

  [[nodiscard]] std::optional<OrderGateError>
  failLocked(OrderGateErrorCode code, std::string message);
  [[nodiscard]] std::optional<OrderGateError>
  validateEpochLocked(const CommitOrderToken &token);
  [[nodiscard]] std::optional<OrderGateError> stoppedLocked() const;
  [[nodiscard]] std::optional<OrderGateError>
  validateNewActiveStartLocked(ProducerId producer_id,
                               const CommitOrderToken &start_token);
  [[nodiscard]] std::optional<CommitOrderToken> effectiveFrontierLocked();
  [[nodiscard]] bool activeStartsPermitLocked(const CommitOrderToken &token,
                                              bool &ambiguous) const;
  void removePendingBaseLocked(const CommitOrderToken &token);

  mutable std::mutex mutex_;
  std::map<ProducerId, ProducerState> producers_;
  std::map<TransactionAttemptUid, ActiveStartState, EntityLess> active_starts_;
  std::map<CommitOrderToken, PendingTransaction, TokenLess> pending_;
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t>
      pending_base_counts_;
  std::map<std::pair<std::uint64_t, std::uint64_t>, bool>
      pending_base_all_tied_;
  std::set<TransactionUid, EntityLess> transaction_ids_;
  std::set<TransactionAttemptUid, EntityLess> committed_attempt_ids_;
  std::set<TransactionAttemptUid, EntityLess> seen_attempt_ids_;
  std::map<SessionUid, SessionCommitState, SessionLess>
      latest_committed_by_session_;
  std::optional<CommitOrderToken> database_frontier_;
  std::optional<CommitOrderToken> last_released_token_;
  std::optional<std::uint64_t> cluster_epoch_;
  std::optional<OrderGateError> error_;
  CommitSeq next_commit_seq_               = 0;
  std::size_t pending_reserved_bytes_      = 0;
  std::size_t peak_pending_transactions_   = 0;
  std::size_t peak_pending_reserved_bytes_ = 0;
};

} // namespace co_checker::online
