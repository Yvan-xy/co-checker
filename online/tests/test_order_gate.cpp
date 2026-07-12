#include "co_checker/online/order_gate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace co_checker::online;
using namespace std::chrono_literals;

int failures = 0;

void check(const bool condition, const std::string_view expression,
           const std::string_view file, const int line) {
  if (condition) {
    return;
  }
  std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
  ++failures;
}

#define CHECK(expression)                                                      \
  check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

[[nodiscard]] EntityUid uid(const std::uint64_t value) {
  return EntityUid{{0, value}};
}

[[nodiscard]] CommitOrderToken
token(const std::uint64_t timestamp,
      const std::optional<std::uint64_t> tie_breaker = std::nullopt,
      const std::uint64_t epoch                      = 1) {
  return CommitOrderToken{epoch, timestamp, tie_breaker};
}

[[nodiscard]] SessionUid session(const std::uint64_t value,
                                 const ProducerId producer = 1) {
  return SessionUid{RuntimeEpoch{{10, 20}}, producer, value};
}

[[nodiscard]] PendingTransaction
transaction(const std::uint64_t transaction_id, const std::uint64_t attempt_id,
            const std::uint64_t session_id, const std::uint64_t session_seq,
            const CommitOrderToken commit_token, const ProducerSeq producer_seq,
            const bool read_only = false, const ProducerId producer = 1,
            const std::size_t reserved_bytes  = 64,
            std::shared_ptr<const void> owner = {}) {
  OrderedTxnMeta meta;
  meta.transaction_id = uid(transaction_id);
  meta.attempt_id     = uid(attempt_id);
  meta.session_id     = session(session_id, producer);
  meta.session_seq    = session_seq;
  meta.commit_token   = commit_token;
  meta.producer_id    = producer;
  meta.producer_seq   = producer_seq;
  return PendingTransaction{meta, read_only, reserved_bytes, std::move(owner)};
}

[[nodiscard]] PendingTransaction
transaction(const std::uint64_t transaction_id, const std::uint64_t attempt_id,
            const std::uint64_t session_id, const std::uint64_t session_seq,
            const std::uint64_t commit_timestamp,
            const ProducerSeq producer_seq, const bool read_only = false,
            const ProducerId producer         = 1,
            const std::size_t reserved_bytes  = 64,
            std::shared_ptr<const void> owner = {}) {
  return transaction(transaction_id, attempt_id, session_id, session_seq,
                     token(commit_timestamp), producer_seq, read_only, producer,
                     reserved_bytes, std::move(owner));
}

void expectNoError(const std::optional<OrderGateError> &error) {
  if (!error.has_value()) {
    return;
  }
  std::cerr << "unexpected gate error: " << error->message << '\n';
  ++failures;
}

void expectError(const std::optional<OrderGateError> &error,
                 const OrderGateErrorCode code) {
  CHECK(error.has_value());
  if (error.has_value()) {
    CHECK(error->code == code);
  }
}

struct ReferenceRelease {
  std::uint64_t timestamp = 0;
  CommitSeq commit_seq    = 0;
};

// A small reference invariant for plain, unique tokens. It stores active and
// pending transactions in vectors, sorts pending tokens on every release, and
// directly applies the closed-prefix definition instead of mirroring the
// production gate's ordered maps.
class ClosedPrefixReference {
public:
  ClosedPrefixReference(std::vector<std::uint64_t> transaction_tokens,
                        const std::vector<ProducerId> &producers)
      : tokens_(std::move(transaction_tokens)), active_(tokens_.size(), true),
        submitted_(tokens_.size(), false), released_(tokens_.size(), false) {
    for (const auto producer : producers) {
      producer_frontiers_.try_emplace(producer);
    }
  }

  void submit(const std::size_t index) {
    CHECK(index < tokens_.size());
    CHECK(active_[index]);
    active_[index]    = false;
    submitted_[index] = true;
  }

  void setDatabaseFrontier(const std::uint64_t frontier) {
    CHECK(!database_frontier_.has_value() || frontier >= *database_frontier_);
    database_frontier_ = frontier;
  }

  void setProducerFrontier(const ProducerId producer,
                           const std::uint64_t frontier) {
    auto &stored = producer_frontiers_.at(producer);
    CHECK(!stored.has_value() || frontier >= *stored);
    stored = frontier;
  }

  [[nodiscard]] std::vector<ReferenceRelease> releaseEligible() {
    std::vector<ReferenceRelease> result;
    if (!database_frontier_.has_value()) {
      return result;
    }
    auto effective_frontier = *database_frontier_;
    for (const auto &[producer, frontier] : producer_frontiers_) {
      (void)producer;
      if (!frontier.has_value()) {
        return result;
      }
      effective_frontier = std::min(effective_frontier, *frontier);
    }

    while (true) {
      std::optional<std::size_t> smallest_pending;
      for (std::size_t index = 0; index < tokens_.size(); ++index) {
        if (!submitted_[index] || released_[index]) {
          continue;
        }
        if (!smallest_pending.has_value() ||
            tokens_[index] < tokens_[*smallest_pending]) {
          smallest_pending = index;
        }
      }
      if (!smallest_pending.has_value() ||
          tokens_[*smallest_pending] > effective_frontier) {
        break;
      }

      bool blocked = false;
      for (std::size_t index = 0; index < tokens_.size(); ++index) {
        if (active_[index] && tokens_[*smallest_pending] >= tokens_[index]) {
          blocked = true;
          break;
        }
      }
      if (blocked) {
        break;
      }

      released_[*smallest_pending] = true;
      result.push_back(ReferenceRelease{
          .timestamp  = tokens_[*smallest_pending],
          .commit_seq = next_commit_seq_,
      });
      ++next_commit_seq_;
    }
    return result;
  }

private:
  std::vector<std::uint64_t> tokens_;
  std::vector<bool> active_;
  std::vector<bool> submitted_;
  std::vector<bool> released_;
  std::optional<std::uint64_t> database_frontier_;
  std::map<ProducerId, std::optional<std::uint64_t>> producer_frontiers_;
  CommitSeq next_commit_seq_ = 0;
};

void compareRelease(CommitOrderGate &gate, ClosedPrefixReference &reference) {
  const auto actual   = gate.releaseEligible();
  const auto expected = reference.releaseEligible();
  CHECK(!gate.error().has_value());
  CHECK(actual.size() == expected.size());
  const auto count = std::min(actual.size(), expected.size());
  for (std::size_t index = 0; index < count; ++index) {
    CHECK(actual[index].meta.commit_token.timestamp ==
          expected[index].timestamp);
    CHECK(actual[index].meta.commit_seq == expected[index].commit_seq);
  }
}

void test_token_comparison_and_authoritative_ties() {
  CHECK(compareCommitOrderTokens(token(10), token(20)) == TokenRelation::Less);
  CHECK(compareCommitOrderTokens(token(20), token(10)) ==
        TokenRelation::Greater);
  CHECK(compareCommitOrderTokens(token(10), token(10)) == TokenRelation::Equal);
  CHECK(compareCommitOrderTokens(token(10), token(10, 1)) ==
        TokenRelation::Ambiguous);
  CHECK(compareCommitOrderTokens(token(10, 1), token(10, 2)) ==
        TokenRelation::Less);
  CHECK(compareCommitOrderTokens(token(10, 2), token(10, 1)) ==
        TokenRelation::Greater);
  CHECK(compareCommitOrderTokens(token(100, std::nullopt, 1),
                                 token(1, std::nullopt, 2)) ==
        TokenRelation::Less);

  CommitOrderGate gate({1});
  expectNoError(gate.registerActiveStart(uid(1), 1, token(10)));
  expectNoError(gate.registerActiveStart(uid(2), 1, token(11)));
  expectNoError(
      gate.submitCommitted(transaction(1, 1, 1, 1, token(30, 2), 1, false)));
  expectNoError(
      gate.submitCommitted(transaction(2, 2, 2, 1, token(30, 1), 2, false)));
  expectNoError(gate.updateDatabaseFrontier(token(30, 2)));
  expectNoError(gate.updateProducerDeliveryFrontier(1, token(30, 1), 2));

  auto first = gate.releaseEligible();
  CHECK(first.size() == 1);
  if (first.size() == 1) {
    CHECK(first[0].meta.commit_token.authoritative_tie_breaker == 1);
    CHECK(first[0].meta.commit_seq == 0);
  }

  expectNoError(gate.updateProducerDeliveryFrontier(1, token(30, 2), 2));
  auto second = gate.releaseEligible();
  CHECK(second.size() == 1);
  if (second.size() == 1) {
    CHECK(second[0].meta.commit_token.authoritative_tie_breaker == 2);
    CHECK(second[0].meta.commit_seq == 1);
  }
}

void test_frontier_and_active_registration_linearization() {
  // Registration linearizes first. Both frontiers may subsequently overtake
  // it; the active guard blocks release. Completion inserts the batch and
  // removes the guard atomically, so the old cutoff is still safe.
  CommitOrderGate guarded({1});
  expectNoError(guarded.registerActiveStart(uid(1), 1, token(10)));
  expectNoError(guarded.updateDatabaseFrontier(token(100)));
  expectNoError(guarded.updateProducerDeliveryFrontier(1, token(100), 0));
  CHECK(guarded.releaseEligible().empty());
  expectNoError(guarded.submitCommitted(transaction(1, 1, 1, 1, 10, 1, true)));
  auto released = guarded.releaseEligible();
  CHECK(released.size() == 1);
  if (released.size() == 1) {
    CHECK(released[0].meta.commit_token.timestamp == 10);
  }

  // A frontier that linearizes first cannot later be contradicted by a newly
  // registered start at or below it.
  CommitOrderGate late({1});
  expectNoError(late.updateDatabaseFrontier(token(100)));
  expectNoError(late.updateProducerDeliveryFrontier(1, token(100), 0));
  expectError(late.registerActiveStart(uid(2), 1, token(10)),
              OrderGateErrorCode::LateActiveStart);

  CommitOrderGate above({1});
  expectNoError(above.updateDatabaseFrontier(token(100)));
  expectNoError(above.updateProducerDeliveryFrontier(1, token(120), 0));
  expectNoError(above.registerActiveStart(uid(3), 1, token(130)));
  expectNoError(above.abortActiveStart(uid(3)));
}

void test_all_required_frontiers_and_active_minimum_gate_release() {
  CommitOrderGate gate({1, 2});
  expectNoError(gate.registerActiveStart(uid(11), 1, token(10)));
  expectNoError(gate.registerActiveStart(uid(22), 2, token(20)));
  expectNoError(
      gate.submitCommitted(transaction(2, 22, 2, 1, 40, 1, false, 2)));
  expectNoError(gate.submitCommitted(transaction(1, 11, 1, 1, 30, 1)));

  expectNoError(gate.updateDatabaseFrontier(token(50)));
  expectNoError(gate.updateProducerDeliveryFrontier(1, token(50), 1));
  CHECK(gate.releaseEligible().empty());
  expectNoError(gate.updateProducerDeliveryFrontier(2, token(50), 1));

  auto released = gate.releaseEligible();
  CHECK(released.size() == 2);
  if (released.size() == 2) {
    CHECK(released[0].meta.commit_token.timestamp == 30);
    CHECK(released[0].meta.commit_seq == 0);
    CHECK(released[1].meta.commit_token.timestamp == 40);
    CHECK(released[1].meta.commit_seq == 1);
  }
}

void test_active_lifecycle_and_commit_token_rules() {
  CommitOrderGate abort_unblocks({1});
  expectNoError(abort_unblocks.registerActiveStart(uid(1), 1, token(10)));
  expectNoError(abort_unblocks.registerActiveStart(uid(2), 1, token(20)));
  expectNoError(
      abort_unblocks.submitCommitted(transaction(2, 2, 2, 1, 20, 1, true)));
  expectNoError(abort_unblocks.updateDatabaseFrontier(token(100)));
  expectNoError(
      abort_unblocks.updateProducerDeliveryFrontier(1, token(100), 1));
  CHECK(abort_unblocks.releaseEligible().empty());
  expectNoError(abort_unblocks.abortActiveStart(uid(1)));
  CHECK(abort_unblocks.releaseEligible().size() == 1);

  CommitOrderGate missing_abort({1});
  expectError(missing_abort.abortActiveStart(uid(9)),
              OrderGateErrorCode::MissingActiveStart);

  CommitOrderGate missing_submit({1});
  expectError(
      missing_submit.submitCommitted(transaction(9, 9, 9, 1, 10, 1, true)),
      OrderGateErrorCode::MissingActiveStart);

  CommitOrderGate bad_read_only({1});
  expectNoError(bad_read_only.registerActiveStart(uid(3), 1, token(30)));
  expectError(
      bad_read_only.submitCommitted(transaction(3, 3, 3, 1, 31, 1, true)),
      OrderGateErrorCode::InvalidCommitToken);

  CommitOrderGate bad_write({1});
  expectNoError(bad_write.registerActiveStart(uid(4), 1, token(40)));
  expectError(bad_write.submitCommitted(transaction(4, 4, 4, 1, 40, 1, false)),
              OrderGateErrorCode::InvalidCommitToken);

  CommitOrderGate no_producers({});
  expectError(no_producers.error(), OrderGateErrorCode::UnknownProducer);
}

void test_duplicate_ambiguous_and_late_records_are_fatal() {
  CommitOrderGate duplicate({1});
  expectNoError(duplicate.registerActiveStart(uid(1), 1, token(10)));
  expectNoError(duplicate.registerActiveStart(uid(2), 1, token(11)));
  expectNoError(duplicate.submitCommitted(transaction(1, 1, 1, 1, 30, 1)));
  expectError(duplicate.submitCommitted(transaction(2, 2, 2, 1, 30, 2)),
              OrderGateErrorCode::DuplicateToken);

  CommitOrderGate ambiguous({1});
  expectNoError(ambiguous.registerActiveStart(uid(3), 1, token(10)));
  expectNoError(ambiguous.registerActiveStart(uid(4), 1, token(11)));
  expectNoError(
      ambiguous.submitCommitted(transaction(3, 3, 3, 1, token(30, 1), 1)));
  expectError(ambiguous.submitCommitted(transaction(4, 4, 4, 1, 30, 2)),
              OrderGateErrorCode::AmbiguousToken);

  CommitOrderGate late({1});
  expectNoError(late.registerActiveStart(uid(5), 1, token(10)));
  expectNoError(late.submitCommitted(transaction(5, 5, 5, 1, 20, 1)));
  expectNoError(late.updateDatabaseFrontier(token(20)));
  expectNoError(late.updateProducerDeliveryFrontier(1, token(20), 1));
  CHECK(late.releaseEligible().size() == 1);
  expectError(late.registerActiveStart(uid(6), 1, token(19)),
              OrderGateErrorCode::LateActiveStart);
}

void test_attempt_transaction_and_producer_identity_rules() {
  CommitOrderGate reused_attempt({1});
  expectNoError(reused_attempt.registerActiveStart(uid(1), 1, token(10)));
  expectNoError(reused_attempt.abortActiveStart(uid(1)));
  expectError(reused_attempt.registerActiveStart(uid(1), 1, token(11)),
              OrderGateErrorCode::DuplicateActiveStart);

  CommitOrderGate duplicate_txn({1});
  expectNoError(duplicate_txn.registerActiveStart(uid(2), 1, token(10)));
  expectNoError(duplicate_txn.submitCommitted(transaction(9, 2, 1, 1, 20, 1)));
  expectNoError(duplicate_txn.registerActiveStart(uid(3), 1, token(21)));
  expectError(duplicate_txn.submitCommitted(transaction(9, 3, 2, 1, 30, 2)),
              OrderGateErrorCode::DuplicateTransaction);

  CommitOrderGate mismatch({1, 2});
  expectNoError(mismatch.registerActiveStart(uid(4), 1, token(40)));
  expectError(
      mismatch.submitCommitted(transaction(10, 4, 3, 1, 40, 1, true, 2)),
      OrderGateErrorCode::ActiveProducerMismatch);

  CommitOrderGate unknown({1});
  expectError(unknown.registerActiveStart(uid(5), 2, token(50)),
              OrderGateErrorCode::UnknownProducer);
}

void test_session_sequence_and_token_must_increase() {
  CommitOrderGate duplicate_sequence({1});
  const auto same_session = session(1);
  expectNoError(duplicate_sequence.registerActiveStart(uid(1), 1, token(10)));
  auto first            = transaction(1, 1, 1, 1, 20, 1);
  first.meta.session_id = same_session;
  expectNoError(duplicate_sequence.submitCommitted(std::move(first)));
  expectNoError(duplicate_sequence.registerActiveStart(uid(2), 1, token(21)));
  auto second            = transaction(2, 2, 1, 1, 30, 2);
  second.meta.session_id = same_session;
  expectError(duplicate_sequence.submitCommitted(std::move(second)),
              OrderGateErrorCode::NonIncreasingSessionSequence);

  CommitOrderGate decreasing_token({1});
  expectNoError(decreasing_token.registerActiveStart(uid(3), 1, token(10)));
  auto high            = transaction(3, 3, 1, 1, 30, 1);
  high.meta.session_id = same_session;
  expectNoError(decreasing_token.submitCommitted(std::move(high)));
  expectNoError(decreasing_token.registerActiveStart(uid(4), 1, token(15)));
  auto low            = transaction(4, 4, 1, 2, 20, 2);
  low.meta.session_id = same_session;
  expectError(decreasing_token.submitCommitted(std::move(low)),
              OrderGateErrorCode::NonIncreasingSessionToken);
}

void test_producer_sequences_delivery_cutoffs_and_frontier_regressions() {
  CommitOrderGate missing({1});
  expectError(missing.updateProducerDeliveryFrontier(1, token(50), 1),
              OrderGateErrorCode::DeliveryCutoffMissing);

  CommitOrderGate gap({1});
  expectNoError(gap.registerActiveStart(uid(1), 1, token(10)));
  expectError(gap.submitCommitted(transaction(1, 1, 1, 1, 20, 2)),
              OrderGateErrorCode::ProducerSequenceGap);

  CommitOrderGate regression({1});
  expectNoError(regression.registerActiveStart(uid(2), 1, token(10)));
  expectNoError(regression.submitCommitted(transaction(2, 2, 2, 1, 20, 1)));
  expectNoError(regression.updateProducerDeliveryFrontier(1, token(50), 1));
  expectError(regression.updateProducerDeliveryFrontier(1, token(40), 1),
              OrderGateErrorCode::FrontierRegression);

  CommitOrderGate cutoff_regression({1});
  expectNoError(cutoff_regression.registerActiveStart(uid(3), 1, token(10)));
  expectNoError(cutoff_regression.registerActiveStart(uid(4), 1, token(11)));
  expectNoError(
      cutoff_regression.submitCommitted(transaction(3, 3, 3, 1, 20, 1)));
  expectNoError(
      cutoff_regression.submitCommitted(transaction(4, 4, 4, 1, 30, 2)));
  expectNoError(
      cutoff_regression.updateProducerDeliveryFrontier(1, token(40), 2));
  expectError(cutoff_regression.updateProducerDeliveryFrontier(1, token(40), 1),
              OrderGateErrorCode::FrontierRegression);

  CommitOrderGate database_regression({1});
  expectNoError(database_regression.updateDatabaseFrontier(token(50)));
  expectError(database_regression.updateDatabaseFrontier(token(49)),
              OrderGateErrorCode::FrontierRegression);
}

void test_epoch_mismatch_and_sticky_error() {
  CommitOrderGate gate({1});
  expectNoError(gate.registerActiveStart(uid(1), 1, token(10)));
  const auto first_error =
      gate.updateDatabaseFrontier(token(20, std::nullopt, 2));
  expectError(first_error, OrderGateErrorCode::ClusterEpochMismatch);
  const auto second_error = gate.abortActiveStart(uid(1));
  CHECK(second_error == first_error);
  CHECK(gate.error() == first_error);
  CHECK(gate.releaseEligible().empty());
  CHECK(gate.metrics().active_starts == 1);
}

void test_byte_accounting_peaks_and_overflow() {
  CommitOrderGate gate({1});
  expectNoError(gate.registerActiveStart(uid(1), 1, token(10)));
  expectNoError(gate.registerActiveStart(uid(2), 1, token(11)));
  expectNoError(
      gate.submitCommitted(transaction(1, 1, 1, 1, 20, 1, false, 1, 100)));
  expectNoError(
      gate.submitCommitted(transaction(2, 2, 2, 1, 30, 2, false, 1, 200)));
  auto pending_metrics = gate.metrics();
  CHECK(pending_metrics.pending_transactions == 2);
  CHECK(pending_metrics.pending_reserved_bytes == 300);
  CHECK(pending_metrics.peak_pending_transactions == 2);
  CHECK(pending_metrics.peak_pending_reserved_bytes == 300);
  expectNoError(gate.updateDatabaseFrontier(token(40)));
  expectNoError(gate.updateProducerDeliveryFrontier(1, token(40), 2));
  CHECK(gate.releaseEligible().size() == 2);
  const auto drained_metrics = gate.metrics();
  CHECK(drained_metrics.pending_transactions == 0);
  CHECK(drained_metrics.pending_reserved_bytes == 0);
  CHECK(drained_metrics.peak_pending_reserved_bytes == 300);

  CommitOrderGate overflow({1});
  expectNoError(overflow.registerActiveStart(uid(3), 1, token(10)));
  expectNoError(overflow.registerActiveStart(uid(4), 1, token(11)));
  constexpr auto almost_max = std::numeric_limits<std::size_t>::max() - 4;
  expectNoError(overflow.submitCommitted(
      transaction(3, 3, 3, 1, 20, 1, false, 1, almost_max)));
  expectError(
      overflow.submitCommitted(transaction(4, 4, 4, 1, 30, 2, false, 1, 10)),
      OrderGateErrorCode::ByteAccountingOverflow);
  CHECK(overflow.metrics().pending_transactions == 1);
  CHECK(overflow.metrics().pending_reserved_bytes == almost_max);
}

void test_later_ambiguity_does_not_drop_an_earlier_owner() {
  CommitOrderGate gate({1});
  expectNoError(gate.registerActiveStart(uid(1), 1, token(1)));
  expectNoError(gate.registerActiveStart(uid(2), 1, token(2)));
  auto owner = std::make_shared<std::uint64_t>(99);
  expectNoError(gate.submitCommitted(
      transaction(1, 1, 1, 1, 10, 1, false, 1, 10, owner)));
  expectNoError(gate.submitCommitted(
      transaction(2, 2, 2, 1, token(20, 1), 2, false, 1, 20)));
  expectNoError(gate.updateDatabaseFrontier(token(30)));
  expectNoError(gate.updateProducerDeliveryFrontier(1, token(20), 2));
  CHECK(owner.use_count() == 2);

  auto released = gate.releaseEligible();
  CHECK(released.empty());
  expectError(gate.error(), OrderGateErrorCode::AmbiguousToken);
  CHECK(owner.use_count() == 2);
  CHECK(gate.metrics().pending_transactions == 2);
  CHECK(gate.metrics().pending_reserved_bytes == 30);
}

void test_exhaustive_small_arrival_and_frontier_permutations() {
  std::array<std::uint64_t, 3> arrival_tokens{10, 20, 30};
  do {
    for (std::size_t database_position = 0; database_position <= 3;
         ++database_position) {
      for (std::size_t producer_position = 0; producer_position <= 3;
           ++producer_position) {
        for (const bool database_first : {false, true}) {
          CommitOrderGate gate({1});
          ClosedPrefixReference reference(
              std::vector<std::uint64_t>(arrival_tokens.begin(),
                                         arrival_tokens.end()),
              {1});
          for (std::size_t index = 0; index < arrival_tokens.size(); ++index) {
            expectNoError(gate.registerActiveStart(
                uid(index + 1), 1, token(arrival_tokens[index])));
          }

          const auto database_update = [&] {
            expectNoError(gate.updateDatabaseFrontier(token(100)));
            reference.setDatabaseFrontier(100);
            compareRelease(gate, reference);
          };
          const auto producer_update = [&](const std::size_t cutoff) {
            expectNoError(
                gate.updateProducerDeliveryFrontier(1, token(100), cutoff));
            reference.setProducerFrontier(1, 100);
            compareRelease(gate, reference);
          };

          for (std::size_t position = 0; position <= arrival_tokens.size();
               ++position) {
            const bool do_database = database_position == position;
            const bool do_producer = producer_position == position;
            if (do_database && do_producer) {
              if (database_first) {
                database_update();
                producer_update(position);
              } else {
                producer_update(position);
                database_update();
              }
            } else if (do_database) {
              database_update();
            } else if (do_producer) {
              producer_update(position);
            }

            if (position < arrival_tokens.size()) {
              expectNoError(gate.submitCommitted(
                  transaction(position + 1, position + 1, position + 1, 1,
                              arrival_tokens[position], position + 1, true)));
              reference.submit(position);
              compareRelease(gate, reference);
            }
          }
          compareRelease(gate, reference);
          CHECK(!gate.error().has_value());
          CHECK(gate.metrics().pending_transactions == 0);
          CHECK(gate.metrics().active_starts == 0);
          CHECK(gate.metrics().next_commit_seq == 3);
        }
      }
    }
  } while (std::next_permutation(arrival_tokens.begin(), arrival_tokens.end()));
}

void test_randomized_schedules_against_closed_prefix_reference() {
  constexpr std::size_t transaction_count = 9;
  constexpr std::size_t trial_count       = 160;
  std::mt19937_64 random(0xc01117U);

  for (std::size_t trial = 0; trial < trial_count; ++trial) {
    std::vector<std::uint64_t> tokens(transaction_count);
    for (std::size_t index = 0; index < transaction_count; ++index) {
      tokens[index] = static_cast<std::uint64_t>((index + 1) * 10);
    }
    std::shuffle(tokens.begin(), tokens.end(), random);

    std::vector<ProducerId> transaction_producers(transaction_count);
    std::array<std::vector<std::size_t>, 2> by_producer;
    for (std::size_t index = 0; index < transaction_count; ++index) {
      const auto producer = static_cast<ProducerId>(1 + (random() % 2));
      transaction_producers[index] = producer;
      by_producer[producer - 1].push_back(index);
    }

    CommitOrderGate gate({1, 2});
    ClosedPrefixReference reference(tokens, {1, 2});
    for (std::size_t index = 0; index < transaction_count; ++index) {
      expectNoError(gate.registerActiveStart(
          uid(index + 1), transaction_producers[index], token(tokens[index])));
    }

    std::array<std::size_t, 2> next_by_producer{};
    std::array<std::optional<std::uint64_t>, 2> producer_frontiers;
    std::optional<std::uint64_t> database_frontier;

    for (std::size_t action = 0; action < 100; ++action) {
      const auto choice = random() % 5;
      if (choice <= 1) {
        const auto producer_index = static_cast<std::size_t>(random() % 2);
        if (next_by_producer[producer_index] <
            by_producer[producer_index].size()) {
          const auto local_index = next_by_producer[producer_index]++;
          const auto txn_index   = by_producer[producer_index][local_index];
          const auto producer    = static_cast<ProducerId>(producer_index + 1);
          expectNoError(gate.submitCommitted(transaction(
              txn_index + 1, txn_index + 1, txn_index + 1, 1, tokens[txn_index],
              local_index + 1, true, producer, 1 + txn_index)));
          reference.submit(txn_index);
        }
      } else if (choice == 2) {
        const auto candidate = static_cast<std::uint64_t>(1 + (random() % 120));
        const auto frontier =
            std::max(database_frontier.value_or(0), candidate);
        expectNoError(gate.updateDatabaseFrontier(token(frontier)));
        reference.setDatabaseFrontier(frontier);
        database_frontier = frontier;
      } else if (choice == 3) {
        const auto producer_index = static_cast<std::size_t>(random() % 2);
        const auto producer       = static_cast<ProducerId>(producer_index + 1);
        const auto candidate = static_cast<std::uint64_t>(1 + (random() % 120));
        const auto frontier =
            std::max(producer_frontiers[producer_index].value_or(0), candidate);
        expectNoError(gate.updateProducerDeliveryFrontier(
            producer, token(frontier), next_by_producer[producer_index]));
        reference.setProducerFrontier(producer, frontier);
        producer_frontiers[producer_index] = frontier;
      }
      compareRelease(gate, reference);
    }

    for (std::size_t producer_index = 0; producer_index < 2; ++producer_index) {
      while (next_by_producer[producer_index] <
             by_producer[producer_index].size()) {
        const auto local_index = next_by_producer[producer_index]++;
        const auto txn_index   = by_producer[producer_index][local_index];
        const auto producer    = static_cast<ProducerId>(producer_index + 1);
        expectNoError(gate.submitCommitted(transaction(
            txn_index + 1, txn_index + 1, txn_index + 1, 1, tokens[txn_index],
            local_index + 1, true, producer, 1 + txn_index)));
        reference.submit(txn_index);
        compareRelease(gate, reference);
      }
    }
    expectNoError(gate.updateDatabaseFrontier(token(200)));
    reference.setDatabaseFrontier(200);
    for (std::size_t producer_index = 0; producer_index < 2; ++producer_index) {
      const auto producer = static_cast<ProducerId>(producer_index + 1);
      expectNoError(gate.updateProducerDeliveryFrontier(
          producer, token(200), next_by_producer[producer_index]));
      reference.setProducerFrontier(producer, 200);
    }
    compareRelease(gate, reference);
    CHECK(!gate.error().has_value());
    CHECK(gate.metrics().pending_transactions == 0);
    CHECK(gate.metrics().active_starts == 0);
    CHECK(gate.metrics().next_commit_seq == transaction_count);
  }
}

void test_concurrent_registration_submission_frontier_and_release_stress() {
  constexpr std::size_t producer_count            = 4;
  constexpr std::size_t transactions_per_producer = 160;
  constexpr std::size_t total_transactions =
      producer_count * transactions_per_producer;
  constexpr std::uint64_t closed_frontier = 1'000'000;

  std::vector<ProducerId> producers(producer_count);
  std::iota(producers.begin(), producers.end(), ProducerId{1});
  CommitOrderGate gate(producers);

  struct Record {
    PendingTransaction transaction;
    CommitOrderToken start;
  };
  std::vector<std::vector<Record>> records(producer_count);
  std::uint64_t next_id = 1;
  for (std::size_t producer_index = 0; producer_index < producer_count;
       ++producer_index) {
    auto &producer_records = records[producer_index];
    producer_records.reserve(transactions_per_producer);
    for (std::size_t local_index = 0; local_index < transactions_per_producer;
         ++local_index) {
      const auto producer = static_cast<ProducerId>(producer_index + 1);
      const auto timestamp =
          static_cast<std::uint64_t>(100 + local_index * producer_count +
                                     (producer_count - producer_index));
      auto owner = std::make_shared<std::uint64_t>(next_id);
      producer_records.push_back(Record{
          .transaction = transaction(next_id, next_id, next_id, 1, timestamp,
                                     local_index + 1, true, producer,
                                     8 + local_index % 17, std::move(owner)),
          .start       = token(timestamp),
      });
      expectNoError(
          gate.registerActiveStart(uid(next_id), producer, token(timestamp)));
      ++next_id;
    }
  }

  // These controls linearize after every active registration. Submissions may
  // complete below them, relying on atomic insert-then-unregister.
  expectNoError(gate.updateDatabaseFrontier(token(closed_frontier)));
  for (const auto producer : producers) {
    expectNoError(gate.updateProducerDeliveryFrontier(
        producer, token(closed_frontier), 0));
  }

  std::array<std::atomic<std::size_t>, producer_count> submitted_counts;
  for (auto &count : submitted_counts) {
    count.store(0, std::memory_order_relaxed);
  }
  std::atomic<std::size_t> submit_errors{0};
  std::atomic<std::size_t> frontier_errors{0};
  std::atomic<bool> producers_done{false};
  std::atomic<bool> stop_releaser{false};
  std::vector<ReleasedTransaction> released;

  std::thread frontier_thread([&] {
    while (!producers_done.load(std::memory_order_acquire)) {
      for (std::size_t producer_index = 0; producer_index < producer_count;
           ++producer_index) {
        const auto producer = static_cast<ProducerId>(producer_index + 1);
        const auto cutoff =
            submitted_counts[producer_index].load(std::memory_order_acquire);
        if (gate.updateProducerDeliveryFrontier(producer,
                                                token(closed_frontier), cutoff)
                .has_value()) {
          frontier_errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
      std::this_thread::yield();
    }
  });

  std::thread releaser([&] {
    while (!stop_releaser.load(std::memory_order_acquire)) {
      auto batch = gate.releaseEligible();
      released.insert(released.end(), std::make_move_iterator(batch.begin()),
                      std::make_move_iterator(batch.end()));
      std::this_thread::yield();
    }
  });

  std::vector<std::thread> submitters;
  submitters.reserve(producer_count);
  for (std::size_t producer_index = 0; producer_index < producer_count;
       ++producer_index) {
    submitters.emplace_back([&, producer_index] {
      for (std::size_t local_index = 0;
           local_index < records[producer_index].size(); ++local_index) {
        if (gate.submitCommitted(
                    std::move(records[producer_index][local_index].transaction))
                .has_value()) {
          submit_errors.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        submitted_counts[producer_index].store(local_index + 1,
                                               std::memory_order_release);
        if ((local_index % 7) == 0) {
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto &submitter : submitters) {
    submitter.join();
  }
  producers_done.store(true, std::memory_order_release);
  frontier_thread.join();
  for (std::size_t producer_index = 0; producer_index < producer_count;
       ++producer_index) {
    const auto producer = static_cast<ProducerId>(producer_index + 1);
    expectNoError(gate.updateProducerDeliveryFrontier(
        producer, token(closed_frontier), transactions_per_producer));
  }
  stop_releaser.store(true, std::memory_order_release);
  releaser.join();
  auto final_batch = gate.releaseEligible();
  released.insert(released.end(), std::make_move_iterator(final_batch.begin()),
                  std::make_move_iterator(final_batch.end()));

  CHECK(submit_errors.load(std::memory_order_relaxed) == 0);
  CHECK(frontier_errors.load(std::memory_order_relaxed) == 0);
  CHECK(!gate.error().has_value());
  CHECK(released.size() == total_transactions);
  std::set<std::uint64_t> transaction_ids;
  for (std::size_t index = 0; index < released.size(); ++index) {
    CHECK(released[index].meta.commit_seq == index);
    if (index != 0) {
      CHECK(compareCommitOrderTokens(released[index - 1].meta.commit_token,
                                     released[index].meta.commit_token) ==
            TokenRelation::Less);
    }
    CHECK(released[index].owner != nullptr);
    transaction_ids.insert(released[index].meta.transaction_id.words[1]);
  }
  CHECK(transaction_ids.size() == total_transactions);
  const auto metrics = gate.metrics();
  CHECK(metrics.pending_transactions == 0);
  CHECK(metrics.pending_reserved_bytes == 0);
  CHECK(metrics.active_starts == 0);
  CHECK(metrics.next_commit_seq == total_transactions);
}

} // namespace

int run_order_gate_tests() {
  test_token_comparison_and_authoritative_ties();
  test_frontier_and_active_registration_linearization();
  test_all_required_frontiers_and_active_minimum_gate_release();
  test_active_lifecycle_and_commit_token_rules();
  test_duplicate_ambiguous_and_late_records_are_fatal();
  test_attempt_transaction_and_producer_identity_rules();
  test_session_sequence_and_token_must_increase();
  test_producer_sequences_delivery_cutoffs_and_frontier_regressions();
  test_epoch_mismatch_and_sticky_error();
  test_byte_accounting_peaks_and_overflow();
  test_later_ambiguity_does_not_drop_an_earlier_owner();
  test_exhaustive_small_arrival_and_frontier_permutations();
  test_randomized_schedules_against_closed_prefix_reference();
  test_concurrent_registration_submission_frontier_and_release_stress();
  return failures;
}

#ifdef CO_CHECKER_ONLINE_ORDER_GATE_TEST_MAIN
int main() { return run_order_gate_tests() == 0 ? EXIT_SUCCESS : EXIT_FAILURE; }
#endif
