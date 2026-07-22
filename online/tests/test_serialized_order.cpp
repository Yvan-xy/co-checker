#include "co_checker/online/serialized_order.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>

namespace {

using namespace co_checker::online;

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

constexpr RuntimeEpoch kRuntimeEpoch{{0x1234U, 0x5678U}};
constexpr std::uint64_t kClusterEpoch = 7;

[[nodiscard]] EntityUid uid(const std::uint64_t domain,
                            const std::uint64_t value) {
  return EntityUid{{domain, value}};
}

[[nodiscard]] OrderedTxnMeta meta(
    const std::uint64_t transaction, const std::uint64_t attempt,
    const std::uint64_t session_counter, const std::uint64_t session_seq,
    const std::uint64_t token, const PublicationSeq publication_seq,
    const OrderTokenKind kind = OrderTokenKind::WriteCommit) {
  OrderedTxnMeta result;
  result.commit_seq       = kUnassignedCommitSeq;
  result.transaction_id   = uid(1, transaction);
  result.attempt_id       = uid(2, attempt);
  result.session_id       = SessionUid{kRuntimeEpoch, 1, session_counter};
  result.session_seq      = session_seq;
  result.commit_token =
      CommitOrderToken{kClusterEpoch, token, std::nullopt};
  result.order_token_kind = kind;
  result.publication_seq  = publication_seq;
  result.producer_id      = 1;
  result.producer_seq     = publication_seq;
  return result;
}

void expect_error(const SerializedOrderResult &result,
                  const SerializedOrderErrorCode code) {
  CHECK(!result.ok());
  CHECK(result.error.has_value());
  if (result.error.has_value()) {
    CHECK(result.error->code == code);
  }
}

void test_contiguous_sequence_assigns_dense_commit_order() {
  SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
  const auto first = validator.accept(meta(1, 11, 9, 1, 100, 1), 1);
  CHECK(first.ok());
  CHECK(first.ordered_meta->commit_seq == 0);

  const auto second = validator.accept(
      meta(2, 12, 9, 2, 120, 2,
           OrderTokenKind::ReadOnlyCompletion),
      0);
  CHECK(second.ok());
  CHECK(second.ordered_meta->commit_seq == 1);
  const auto metrics = validator.metrics();
  CHECK(metrics.next_publication_seq == 3);
  CHECK(metrics.next_commit_seq == 2);
  CHECK(metrics.accepted_transactions == 2);
  CHECK(metrics.last_token == second.ordered_meta->commit_token);
}

void test_publication_sequence_failures() {
  for (const auto publication_seq : {PublicationSeq{0}, PublicationSeq{2},
                                     PublicationSeq{9}}) {
    SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
    expect_error(validator.accept(meta(1, 11, 1, 1, 10,
                                      publication_seq), 1),
                 SerializedOrderErrorCode::InvalidPublicationSequence);
  }

  SerializedOrderValidator duplicate(kRuntimeEpoch, kClusterEpoch);
  CHECK(duplicate.accept(meta(1, 11, 1, 1, 10, 1), 1).ok());
  expect_error(duplicate.accept(meta(2, 12, 2, 1, 20, 1), 1),
               SerializedOrderErrorCode::InvalidPublicationSequence);

  SerializedOrderValidator exhausted(
      kRuntimeEpoch, kClusterEpoch,
      std::numeric_limits<PublicationSeq>::max(),
      std::numeric_limits<CommitSeq>::max() - 1);
  CHECK(exhausted
            .accept(meta(1, 11, 1, 1, 10,
                         std::numeric_limits<PublicationSeq>::max()),
                    1)
            .ok());
  expect_error(
      exhausted.accept(meta(2, 12, 2, 1, 20,
                            std::numeric_limits<PublicationSeq>::max()),
                       1),
      SerializedOrderErrorCode::PublicationSequenceExhausted);
}

void test_token_and_kind_failures() {
  {
    SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
    expect_error(validator.accept(meta(1, 11, 1, 1, 0, 1), 1),
                 SerializedOrderErrorCode::InvalidToken);
  }
  {
    SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
    CHECK(validator.accept(meta(1, 11, 1, 1, 20, 1), 1).ok());
    expect_error(validator.accept(meta(2, 12, 2, 1, 20, 2), 1),
                 SerializedOrderErrorCode::InvalidToken);
  }
  {
    SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
    CHECK(validator.accept(meta(1, 11, 1, 1, 20, 1), 1).ok());
    expect_error(validator.accept(meta(2, 12, 2, 1, 10, 2), 1),
                 SerializedOrderErrorCode::InvalidToken);
  }
  {
    SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
    expect_error(
        validator.accept(
            meta(1, 11, 1, 1, 10, 1, OrderTokenKind::ReadOnlyStart), 0),
        SerializedOrderErrorCode::InvalidTokenKind);
  }
  {
    SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
    expect_error(
        validator.accept(
            meta(1, 11, 1, 1, 10, 1,
                 OrderTokenKind::ReadOnlyCompletion),
            1),
        SerializedOrderErrorCode::InvalidTokenKind);
  }
}

void test_identity_epoch_and_session_failures() {
  {
    auto wrong = meta(1, 11, 1, 1, 10, 1);
    wrong.session_id.runtime_epoch.words[0] ^= 1U;
    SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
    expect_error(validator.accept(wrong, 1),
                 SerializedOrderErrorCode::EpochMismatch);
  }
  {
    auto wrong = meta(1, 11, 1, 1, 10, 1);
    wrong.commit_token.cluster_epoch = kClusterEpoch + 1;
    SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
    expect_error(validator.accept(wrong, 1),
                 SerializedOrderErrorCode::EpochMismatch);
  }
  {
    SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
    CHECK(validator.accept(meta(1, 11, 1, 1, 10, 1), 1).ok());
    expect_error(validator.accept(meta(2, 11, 2, 1, 20, 2), 1),
                 SerializedOrderErrorCode::DuplicateAttempt);
  }
  {
    SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
    CHECK(validator.accept(meta(1, 11, 1, 1, 10, 1), 1).ok());
    expect_error(validator.accept(meta(1, 12, 2, 1, 20, 2), 1),
                 SerializedOrderErrorCode::DuplicateTransaction);
  }
  {
    SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
    CHECK(validator.accept(meta(1, 11, 1, 2, 10, 1), 1).ok());
    expect_error(validator.accept(meta(2, 12, 1, 1, 20, 2), 1),
                 SerializedOrderErrorCode::SessionOrderRegression);
  }
  {
    auto assigned = meta(1, 11, 1, 1, 10, 1);
    assigned.commit_seq = 0;
    SerializedOrderValidator validator(kRuntimeEpoch, kClusterEpoch);
    expect_error(validator.accept(assigned, 1),
                 SerializedOrderErrorCode::InvalidIdentity);
  }
}

} // namespace

int run_serialized_order_tests() {
  test_contiguous_sequence_assigns_dense_commit_order();
  test_publication_sequence_failures();
  test_token_and_kind_failures();
  test_identity_epoch_and_session_failures();
  return failures;
}

#ifdef CO_CHECKER_ONLINE_SERIALIZED_ORDER_STANDALONE_TEST_MAIN
int main() {
  return run_serialized_order_tests() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
