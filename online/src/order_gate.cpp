#include "co_checker/online/order_gate.h"

#include <algorithm>
#include <limits>
#include <new>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace co_checker::online {

namespace {

std::string tokenText(const CommitOrderToken &token) {
  std::ostringstream out;
  out << token.cluster_epoch << ':' << token.timestamp;
  if (token.authoritative_tie_breaker.has_value()) {
    out << ':' << *token.authoritative_tie_breaker;
  }
  return out.str();
}

} // namespace

TokenRelation compareCommitOrderTokens(const CommitOrderToken &lhs,
                                       const CommitOrderToken &rhs) noexcept {
  if (lhs.cluster_epoch < rhs.cluster_epoch) {
    return TokenRelation::Less;
  }
  if (lhs.cluster_epoch > rhs.cluster_epoch) {
    return TokenRelation::Greater;
  }
  if (lhs.timestamp < rhs.timestamp) {
    return TokenRelation::Less;
  }
  if (lhs.timestamp > rhs.timestamp) {
    return TokenRelation::Greater;
  }

  if (!lhs.authoritative_tie_breaker.has_value() &&
      !rhs.authoritative_tie_breaker.has_value()) {
    return TokenRelation::Equal;
  }
  if (lhs.authoritative_tie_breaker.has_value() !=
      rhs.authoritative_tie_breaker.has_value()) {
    return TokenRelation::Ambiguous;
  }
  if (*lhs.authoritative_tie_breaker < *rhs.authoritative_tie_breaker) {
    return TokenRelation::Less;
  }
  if (*lhs.authoritative_tie_breaker > *rhs.authoritative_tie_breaker) {
    return TokenRelation::Greater;
  }
  return TokenRelation::Equal;
}

bool CommitOrderGate::TokenLess::operator()(
    const CommitOrderToken &lhs, const CommitOrderToken &rhs) const noexcept {
  if (lhs.cluster_epoch != rhs.cluster_epoch) {
    return lhs.cluster_epoch < rhs.cluster_epoch;
  }
  if (lhs.timestamp != rhs.timestamp) {
    return lhs.timestamp < rhs.timestamp;
  }
  return lhs.authoritative_tie_breaker < rhs.authoritative_tie_breaker;
}

bool CommitOrderGate::EntityLess::operator()(
    const EntityUid &lhs, const EntityUid &rhs) const noexcept {
  return lhs.words < rhs.words;
}

bool CommitOrderGate::SessionLess::operator()(
    const SessionUid &lhs, const SessionUid &rhs) const noexcept {
  return std::tie(lhs.runtime_epoch.words[0], lhs.runtime_epoch.words[1],
                  lhs.producer_id, lhs.local_session_counter) <
         std::tie(rhs.runtime_epoch.words[0], rhs.runtime_epoch.words[1],
                  rhs.producer_id, rhs.local_session_counter);
}

CommitOrderGate::CommitOrderGate(std::vector<ProducerId> required_producers) {
  for (const auto producer : required_producers) {
    producers_.try_emplace(producer);
  }
  if (producers_.empty()) {
    error_ =
        OrderGateError{OrderGateErrorCode::UnknownProducer,
                       "the commit-order gate requires at least one producer"};
  }
}

std::optional<OrderGateError>
CommitOrderGate::failLocked(const OrderGateErrorCode code,
                            std::string message) {
  if (!error_.has_value()) {
    error_ = OrderGateError{code, std::move(message)};
  }
  return error_;
}

std::optional<OrderGateError> CommitOrderGate::stoppedLocked() const {
  if (!error_.has_value()) {
    return std::nullopt;
  }
  return error_;
}

std::optional<OrderGateError>
CommitOrderGate::validateEpochLocked(const CommitOrderToken &token) {
  if (!cluster_epoch_.has_value()) {
    cluster_epoch_ = token.cluster_epoch;
    return std::nullopt;
  }
  if (*cluster_epoch_ != token.cluster_epoch) {
    return failLocked(OrderGateErrorCode::ClusterEpochMismatch,
                      "commit-order token changed cluster epoch");
  }
  return std::nullopt;
}

std::optional<OrderGateError> CommitOrderGate::validateNewActiveStartLocked(
    const ProducerId producer_id, const CommitOrderToken &start_token) {
  const auto producer_it = producers_.find(producer_id);
  if (producer_it == producers_.end()) {
    return failLocked(OrderGateErrorCode::UnknownProducer,
                      "active start belongs to an unregistered producer");
  }

  const auto validate_above =
      [&](const CommitOrderToken &closed_token,
          const std::string_view source) -> std::optional<OrderGateError> {
    const auto relation = compareCommitOrderTokens(closed_token, start_token);
    if (relation == TokenRelation::Ambiguous) {
      return failLocked(OrderGateErrorCode::AmbiguousToken,
                        "active start " + tokenText(start_token) +
                            " is ambiguously ordered with the " +
                            std::string(source) + " " +
                            tokenText(closed_token));
    }
    if (relation != TokenRelation::Less) {
      return failLocked(OrderGateErrorCode::LateActiveStart,
                        "active start " + tokenText(start_token) +
                            " is not above the " + std::string(source) + " " +
                            tokenText(closed_token));
    }
    return std::nullopt;
  };

  if (last_released_token_.has_value()) {
    if (auto error =
            validate_above(*last_released_token_, "last released token");
        error.has_value()) {
      return error;
    }
  }
  if (database_frontier_.has_value()) {
    if (auto error = validate_above(*database_frontier_, "database frontier");
        error.has_value()) {
      return error;
    }
  }
  if (producer_it->second.delivery_frontier.has_value()) {
    if (auto error = validate_above(*producer_it->second.delivery_frontier,
                                    "producer delivery frontier");
        error.has_value()) {
      return error;
    }
  }
  return std::nullopt;
}

std::optional<OrderGateError>
CommitOrderGate::registerActiveStart(const TransactionAttemptUid &attempt_id,
                                     const ProducerId producer_id,
                                     const CommitOrderToken &start_token) {
  std::scoped_lock lock(mutex_);
  if (auto stopped = stoppedLocked(); stopped.has_value()) {
    return stopped;
  }
  if (producers_.find(producer_id) == producers_.end()) {
    return failLocked(OrderGateErrorCode::UnknownProducer,
                      "active start belongs to an unregistered producer");
  }
  if (seen_attempt_ids_.contains(attempt_id) ||
      active_starts_.contains(attempt_id)) {
    return failLocked(OrderGateErrorCode::DuplicateActiveStart,
                      "transaction attempt reused an active-start identity");
  }
  if (auto epoch_error = validateEpochLocked(start_token);
      epoch_error.has_value()) {
    return epoch_error;
  }
  if (auto ordering_error =
          validateNewActiveStartLocked(producer_id, start_token);
      ordering_error.has_value()) {
    return ordering_error;
  }
  active_starts_.try_emplace(
      attempt_id,
      ActiveStartState{.token = start_token, .producer_id = producer_id});
  seen_attempt_ids_.insert(attempt_id);
  return std::nullopt;
}

std::optional<OrderGateError>
CommitOrderGate::abortActiveStart(const TransactionAttemptUid &attempt_id) {
  std::scoped_lock lock(mutex_);
  if (auto stopped = stoppedLocked(); stopped.has_value()) {
    return stopped;
  }
  if (active_starts_.erase(attempt_id) != 1U) {
    return failLocked(OrderGateErrorCode::MissingActiveStart,
                      "abort did not match an active transaction attempt");
  }
  return std::nullopt;
}

std::optional<OrderGateError>
CommitOrderGate::submitCommitted(PendingTransaction transaction) {
  std::scoped_lock lock(mutex_);
  if (auto stopped = stoppedLocked(); stopped.has_value()) {
    return stopped;
  }

  const auto active_it = active_starts_.find(transaction.meta.attempt_id);
  if (active_it == active_starts_.end()) {
    return failLocked(OrderGateErrorCode::MissingActiveStart,
                      "committed batch has no active-start registration");
  }
  if (auto epoch_error = validateEpochLocked(transaction.meta.commit_token);
      epoch_error.has_value()) {
    return epoch_error;
  }
  const auto start_relation = compareCommitOrderTokens(
      active_it->second.token, transaction.meta.commit_token);
  const auto valid_commit_token = transaction.read_only
                                      ? start_relation == TokenRelation::Equal
                                      : start_relation == TokenRelation::Less;
  if (!valid_commit_token) {
    return failLocked(
        OrderGateErrorCode::InvalidCommitToken,
        transaction.read_only
            ? "read-only commit token does not equal its registered start token"
            : "write commit token is not strictly above its registered start "
              "token");
  }

  const auto producer_it = producers_.find(transaction.meta.producer_id);
  if (producer_it == producers_.end()) {
    return failLocked(OrderGateErrorCode::UnknownProducer,
                      "batch belongs to an unregistered producer");
  }
  if (transaction.meta.session_id.producer_id != transaction.meta.producer_id) {
    return failLocked(
        OrderGateErrorCode::UnknownProducer,
        "session producer identity disagrees with batch producer");
  }
  if (active_it->second.producer_id != transaction.meta.producer_id) {
    return failLocked(
        OrderGateErrorCode::ActiveProducerMismatch,
        "committed batch producer disagrees with its active-start producer");
  }
  if (producer_it->second.last_received_sequence ==
      std::numeric_limits<ProducerSeq>::max()) {
    return failLocked(OrderGateErrorCode::ProducerSequenceGap,
                      "producer sequence overflow");
  }
  const auto expected_sequence =
      producer_it->second.last_received_sequence + ProducerSeq{1};
  if (transaction.meta.producer_seq != expected_sequence) {
    return failLocked(OrderGateErrorCode::ProducerSequenceGap,
                      "producer sequence is not contiguous: expected " +
                          std::to_string(expected_sequence) + ", received " +
                          std::to_string(transaction.meta.producer_seq));
  }
  if (last_released_token_.has_value()) {
    const auto relation = compareCommitOrderTokens(
        transaction.meta.commit_token, *last_released_token_);
    if (relation != TokenRelation::Greater) {
      return failLocked(relation == TokenRelation::Ambiguous
                            ? OrderGateErrorCode::AmbiguousToken
                            : OrderGateErrorCode::LateTransaction,
                        "commit token " +
                            tokenText(transaction.meta.commit_token) +
                            " is not above the last released token " +
                            tokenText(*last_released_token_));
    }
  }

  const auto base = std::pair{transaction.meta.commit_token.cluster_epoch,
                              transaction.meta.commit_token.timestamp};
  if (pending_.contains(transaction.meta.commit_token)) {
    return failLocked(OrderGateErrorCode::DuplicateToken,
                      "duplicate commit-order token " +
                          tokenText(transaction.meta.commit_token));
  }
  if (const auto count_it = pending_base_counts_.find(base);
      count_it != pending_base_counts_.end()) {
    const auto all_tied = pending_base_all_tied_.at(base);
    if (!all_tied ||
        !transaction.meta.commit_token.authoritative_tie_breaker.has_value()) {
      return failLocked(
          OrderGateErrorCode::AmbiguousToken,
          "equal commit timestamps lack authoritative tie-breakers");
    }
  }
  if (transaction_ids_.contains(transaction.meta.transaction_id)) {
    return failLocked(OrderGateErrorCode::DuplicateTransaction,
                      "logical transaction committed more than once");
  }
  if (committed_attempt_ids_.contains(transaction.meta.attempt_id)) {
    return failLocked(OrderGateErrorCode::DuplicateAttempt,
                      "transaction attempt committed more than once");
  }

  const auto session_it =
      latest_committed_by_session_.find(transaction.meta.session_id);
  if (session_it != latest_committed_by_session_.end()) {
    if (transaction.meta.session_seq <= session_it->second.session_seq) {
      return failLocked(OrderGateErrorCode::NonIncreasingSessionSequence,
                        "committed session sequence did not increase");
    }
    const auto relation = compareCommitOrderTokens(
        session_it->second.commit_token, transaction.meta.commit_token);
    if (relation == TokenRelation::Ambiguous) {
      return failLocked(
          OrderGateErrorCode::AmbiguousToken,
          "successive session commit tokens are ambiguously ordered");
    }
    if (relation != TokenRelation::Less) {
      return failLocked(OrderGateErrorCode::NonIncreasingSessionToken,
                        "committed session token did not increase");
    }
  }

  if (std::numeric_limits<std::size_t>::max() - pending_reserved_bytes_ <
      transaction.reserved_bytes) {
    return failLocked(OrderGateErrorCode::ByteAccountingOverflow,
                      "pending byte accounting overflow");
  }

  producer_it->second.last_received_sequence = transaction.meta.producer_seq;
  transaction_ids_.insert(transaction.meta.transaction_id);
  committed_attempt_ids_.insert(transaction.meta.attempt_id);
  latest_committed_by_session_.insert_or_assign(
      transaction.meta.session_id,
      SessionCommitState{.session_seq  = transaction.meta.session_seq,
                         .commit_token = transaction.meta.commit_token});
  pending_reserved_bytes_ += transaction.reserved_bytes;
  pending_base_counts_[base] += 1U;
  const auto has_tie =
      transaction.meta.commit_token.authoritative_tie_breaker.has_value();
  const auto [all_tied_it, inserted] =
      pending_base_all_tied_.try_emplace(base, has_tie);
  if (!inserted) {
    all_tied_it->second = all_tied_it->second && has_tie;
  }
  pending_.emplace(transaction.meta.commit_token, std::move(transaction));
  peak_pending_transactions_ =
      std::max(peak_pending_transactions_, pending_.size());
  peak_pending_reserved_bytes_ =
      std::max(peak_pending_reserved_bytes_, pending_reserved_bytes_);

  // Publication is now visible to the gate. Only after insertion may the
  // read-only/start guard be removed.
  active_starts_.erase(active_it);
  return std::nullopt;
}

std::optional<OrderGateError>
CommitOrderGate::updateDatabaseFrontier(const CommitOrderToken &frontier) {
  std::scoped_lock lock(mutex_);
  if (auto stopped = stoppedLocked(); stopped.has_value()) {
    return stopped;
  }
  if (auto epoch_error = validateEpochLocked(frontier);
      epoch_error.has_value()) {
    return epoch_error;
  }
  if (database_frontier_.has_value()) {
    const auto relation =
        compareCommitOrderTokens(*database_frontier_, frontier);
    if (relation == TokenRelation::Greater ||
        relation == TokenRelation::Ambiguous) {
      return failLocked(OrderGateErrorCode::FrontierRegression,
                        "database frontier regressed or became ambiguous");
    }
  }
  database_frontier_ = frontier;
  return std::nullopt;
}

std::optional<OrderGateError> CommitOrderGate::updateProducerDeliveryFrontier(
    const ProducerId producer_id, const CommitOrderToken &frontier,
    const ProducerSeq delivered_sequence) {
  std::scoped_lock lock(mutex_);
  if (auto stopped = stoppedLocked(); stopped.has_value()) {
    return stopped;
  }
  const auto producer_it = producers_.find(producer_id);
  if (producer_it == producers_.end()) {
    return failLocked(OrderGateErrorCode::UnknownProducer,
                      "delivery frontier belongs to an unknown producer");
  }
  if (auto epoch_error = validateEpochLocked(frontier);
      epoch_error.has_value()) {
    return epoch_error;
  }
  auto &producer = producer_it->second;
  if (delivered_sequence > producer.last_received_sequence) {
    return failLocked(OrderGateErrorCode::DeliveryCutoffMissing,
                      "delivery barrier overtook a missing producer sequence");
  }
  if (delivered_sequence < producer.delivery_cutoff) {
    return failLocked(OrderGateErrorCode::FrontierRegression,
                      "producer delivery cutoff regressed");
  }
  if (producer.delivery_frontier.has_value()) {
    const auto relation =
        compareCommitOrderTokens(*producer.delivery_frontier, frontier);
    if (relation == TokenRelation::Greater ||
        relation == TokenRelation::Ambiguous) {
      return failLocked(
          OrderGateErrorCode::FrontierRegression,
          "producer delivery frontier regressed or became ambiguous");
    }
  }
  producer.delivery_frontier = frontier;
  producer.delivery_cutoff   = delivered_sequence;
  return std::nullopt;
}

std::optional<CommitOrderToken> CommitOrderGate::effectiveFrontierLocked() {
  if (!database_frontier_.has_value()) {
    return std::nullopt;
  }
  auto result = *database_frontier_;
  for (const auto &[producer_id, producer] : producers_) {
    (void)producer_id;
    if (!producer.delivery_frontier.has_value()) {
      return std::nullopt;
    }
    const auto relation =
        compareCommitOrderTokens(*producer.delivery_frontier, result);
    if (relation == TokenRelation::Ambiguous) {
      (void)failLocked(
          OrderGateErrorCode::AmbiguousToken,
          "database and producer frontiers are ambiguously ordered");
      return std::nullopt;
    }
    if (relation == TokenRelation::Less) {
      result = *producer.delivery_frontier;
    }
  }
  return result;
}

bool CommitOrderGate::activeStartsPermitLocked(const CommitOrderToken &token,
                                               bool &ambiguous) const {
  ambiguous = false;
  for (const auto &[attempt, start] : active_starts_) {
    (void)attempt;
    const auto relation = compareCommitOrderTokens(token, start.token);
    if (relation == TokenRelation::Ambiguous) {
      ambiguous = true;
      return false;
    }
    if (relation != TokenRelation::Less) {
      return false;
    }
  }
  return true;
}

void CommitOrderGate::removePendingBaseLocked(const CommitOrderToken &token) {
  const auto base     = std::pair{token.cluster_epoch, token.timestamp};
  const auto count_it = pending_base_counts_.find(base);
  if (count_it == pending_base_counts_.end()) {
    return;
  }
  if (count_it->second <= 1U) {
    pending_base_counts_.erase(count_it);
    pending_base_all_tied_.erase(base);
    return;
  }
  count_it->second -= 1U;
}

std::vector<ReleasedTransaction> CommitOrderGate::releaseEligible() {
  std::scoped_lock lock(mutex_);
  std::vector<ReleasedTransaction> released;
  if (error_.has_value()) {
    return released;
  }
  const auto effective_frontier = effectiveFrontierLocked();
  if (!effective_frontier.has_value() || error_.has_value()) {
    return released;
  }

  // Validate the entire releasable prefix before mutating gate state. This
  // prevents a later ambiguous token or accounting failure from removing an
  // earlier owner and then returning no usable batch on the fatal path.
  std::size_t eligible_count = 0;
  auto remaining_bytes       = pending_reserved_bytes_;
  auto simulated_commit_seq  = next_commit_seq_;
  for (auto pending_it = pending_.begin(); pending_it != pending_.end();
       ++pending_it) {
    const auto frontier_relation =
        compareCommitOrderTokens(pending_it->first, *effective_frontier);
    if (frontier_relation == TokenRelation::Ambiguous) {
      (void)failLocked(
          OrderGateErrorCode::AmbiguousToken,
          "pending token and effective frontier are ambiguously ordered");
      return released;
    }
    if (frontier_relation == TokenRelation::Greater) {
      break;
    }

    bool active_ambiguous = false;
    if (!activeStartsPermitLocked(pending_it->first, active_ambiguous)) {
      if (active_ambiguous) {
        (void)failLocked(
            OrderGateErrorCode::AmbiguousToken,
            "pending token and active start are ambiguously ordered");
        return released;
      }
      break;
    }
    if (simulated_commit_seq == std::numeric_limits<CommitSeq>::max()) {
      (void)failLocked(OrderGateErrorCode::CommitSequenceExhausted,
                       "checker commit sequence is exhausted");
      return released;
    }
    if (pending_it->second.reserved_bytes > remaining_bytes) {
      (void)failLocked(OrderGateErrorCode::ProtocolStopped,
                       "pending byte accounting underflow");
      return released;
    }
    remaining_bytes -= pending_it->second.reserved_bytes;
    ++simulated_commit_seq;
    ++eligible_count;
  }

  try {
    released.reserve(eligible_count);
  } catch (const std::bad_alloc &) {
    (void)failLocked(OrderGateErrorCode::ResourceExhausted,
                     "could not allocate the released transaction batch");
    return released;
  }

  for (std::size_t index = 0; index < eligible_count; ++index) {
    auto pending_it           = pending_.begin();
    auto pending_transaction  = std::move(pending_it->second);
    const auto released_token = pending_it->first;
    pending_.erase(pending_it);
    removePendingBaseLocked(released_token);
    pending_reserved_bytes_ -= pending_transaction.reserved_bytes;

    pending_transaction.meta.commit_seq = next_commit_seq_;
    ++next_commit_seq_;
    last_released_token_ = released_token;
    released.push_back(ReleasedTransaction{
        std::move(pending_transaction.meta), pending_transaction.read_only,
        pending_transaction.reserved_bytes,
        std::move(pending_transaction.owner)});
  }
  return released;
}

std::optional<OrderGateError> CommitOrderGate::error() const {
  std::scoped_lock lock(mutex_);
  return error_;
}

OrderGateMetrics CommitOrderGate::metrics() const {
  std::scoped_lock lock(mutex_);
  return OrderGateMetrics{
      .pending_transactions        = pending_.size(),
      .pending_reserved_bytes      = pending_reserved_bytes_,
      .peak_pending_transactions   = peak_pending_transactions_,
      .peak_pending_reserved_bytes = peak_pending_reserved_bytes_,
      .active_starts               = active_starts_.size(),
      .next_commit_seq             = next_commit_seq_,
      .last_released_token         = last_released_token_,
  };
}

} // namespace co_checker::online
