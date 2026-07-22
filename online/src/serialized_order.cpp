#include "co_checker/online/serialized_order.h"

#include <limits>
#include <tuple>
#include <utility>

#include "co_checker/online/order_gate.h"

namespace co_checker::online {
namespace {

[[nodiscard]] bool is_zero(const EntityUid &uid) noexcept {
  return uid.words[0] == 0 && uid.words[1] == 0;
}

} // namespace

SerializedOrderValidator::SerializedOrderValidator(
    RuntimeEpoch expected_runtime_epoch,
    const std::uint64_t expected_cluster_epoch,
    const PublicationSeq first_publication_seq,
    const CommitSeq first_commit_seq)
    : expected_runtime_epoch_(expected_runtime_epoch),
      expected_cluster_epoch_(expected_cluster_epoch),
      next_publication_seq_(first_publication_seq),
      next_commit_seq_(first_commit_seq) {}

bool SerializedOrderValidator::SessionLess::operator()(
    const SessionUid &lhs, const SessionUid &rhs) const noexcept {
  return std::tie(lhs.runtime_epoch.words[0], lhs.runtime_epoch.words[1],
                  lhs.producer_id, lhs.local_session_counter) <
         std::tie(rhs.runtime_epoch.words[0], rhs.runtime_epoch.words[1],
                  rhs.producer_id, rhs.local_session_counter);
}

SerializedOrderResult SerializedOrderValidator::failure(
    const SerializedOrderErrorCode code, std::string message) {
  return SerializedOrderResult{
      std::nullopt, SerializedOrderError{code, std::move(message)}};
}

SerializedOrderResult SerializedOrderValidator::accept(
    const OrderedTxnMeta &captured_meta,
    const std::size_t normalized_write_count) {
  if (captured_meta.commit_seq != kUnassignedCommitSeq) {
    return failure(
        SerializedOrderErrorCode::InvalidIdentity,
        "serialized input already has an assigned checker CommitSeq");
  }
  if (captured_meta.session_id.runtime_epoch != expected_runtime_epoch_) {
    return failure(SerializedOrderErrorCode::EpochMismatch,
                   "serialized input runtime epoch does not match");
  }
  if (captured_meta.commit_token.cluster_epoch != expected_cluster_epoch_) {
    return failure(SerializedOrderErrorCode::EpochMismatch,
                   "serialized input cluster epoch does not match");
  }
  if (publication_sequence_exhausted_) {
    return failure(
        SerializedOrderErrorCode::PublicationSequenceExhausted,
        "serialized PublicationSeq space is exhausted");
  }
  if (captured_meta.publication_seq != next_publication_seq_) {
    return failure(
        SerializedOrderErrorCode::InvalidPublicationSequence,
        "serialized PublicationSeq is not the next contiguous value");
  }
  if (captured_meta.commit_token.timestamp == 0) {
    return failure(SerializedOrderErrorCode::InvalidToken,
                   "serialized order token must be nonzero");
  }
  if (last_token_.has_value() &&
      compareCommitOrderTokens(*last_token_, captured_meta.commit_token) !=
          TokenRelation::Less) {
    return failure(
        SerializedOrderErrorCode::InvalidToken,
        "serialized order token must increase strictly");
  }
  if (captured_meta.order_token_kind == OrderTokenKind::ReadOnlyStart) {
    return failure(
        SerializedOrderErrorCode::InvalidTokenKind,
        "serialized protocol does not accept read-only start tokens");
  }
  if ((captured_meta.order_token_kind ==
           OrderTokenKind::ReadOnlyCompletion ||
       captured_meta.order_token_kind ==
           OrderTokenKind::NoWriteCompletion) &&
      normalized_write_count != 0) {
    return failure(
        SerializedOrderErrorCode::InvalidTokenKind,
        "serialized completion token carries a normalized final write");
  }
  if (captured_meta.order_token_kind != OrderTokenKind::WriteCommit &&
      captured_meta.order_token_kind !=
          OrderTokenKind::ReadOnlyCompletion &&
      captured_meta.order_token_kind != OrderTokenKind::NoWriteCompletion) {
    return failure(SerializedOrderErrorCode::InvalidTokenKind,
                   "serialized order token kind is invalid");
  }
  if (captured_meta.session_seq == 0 ||
      captured_meta.session_id.local_session_counter == 0 ||
      captured_meta.session_id.producer_id != captured_meta.producer_id ||
      is_zero(captured_meta.transaction_id) ||
      is_zero(captured_meta.attempt_id)) {
    return failure(SerializedOrderErrorCode::InvalidIdentity,
                   "serialized transaction identity is invalid");
  }
  if (accepted_attempts_.contains(captured_meta.attempt_id)) {
    return failure(SerializedOrderErrorCode::DuplicateAttempt,
                   "serialized attempt identity was accepted twice");
  }
  if (accepted_transactions_.contains(captured_meta.transaction_id)) {
    return failure(SerializedOrderErrorCode::DuplicateTransaction,
                   "serialized logical transaction committed twice");
  }
  const auto session_it = last_session_seq_.find(captured_meta.session_id);
  if (session_it != last_session_seq_.end() &&
      captured_meta.session_seq <= session_it->second) {
    return failure(SerializedOrderErrorCode::SessionOrderRegression,
                   "serialized committed session order did not increase");
  }
  if (next_commit_seq_ == kUnassignedCommitSeq) {
    return failure(SerializedOrderErrorCode::CommitSequenceExhausted,
                   "serialized checker CommitSeq space is exhausted");
  }

  // A failure to grow any of these containers is fatal to the runtime. It may
  // leave validator state partially updated, but no later input is accepted
  // after the runtime records that resource failure.
  accepted_attempts_.insert(captured_meta.attempt_id);
  accepted_transactions_.insert(captured_meta.transaction_id);
  last_session_seq_[captured_meta.session_id] = captured_meta.session_seq;

  auto ordered_meta       = captured_meta;
  ordered_meta.commit_seq = next_commit_seq_;
  ++next_commit_seq_;
  last_token_ = captured_meta.commit_token;
  if (next_publication_seq_ ==
      std::numeric_limits<PublicationSeq>::max()) {
    publication_sequence_exhausted_ = true;
  } else {
    ++next_publication_seq_;
  }

  return SerializedOrderResult{std::move(ordered_meta), std::nullopt};
}

SerializedOrderMetrics SerializedOrderValidator::metrics() const noexcept {
  return SerializedOrderMetrics{
      .next_publication_seq = next_publication_seq_,
      .next_commit_seq      = next_commit_seq_,
      .accepted_transactions =
          static_cast<std::uint64_t>(accepted_transactions_.size()),
      .last_token = last_token_,
  };
}

} // namespace co_checker::online
