#pragma once

#include <map>
#include <optional>
#include <span>
#include <vector>

#include "co_checker/online/result.h"
#include "co_checker/online/types.h"

namespace co_checker::online::test {

struct ReferenceResult {
  std::optional<ViolationKind> transaction_violation;
  bool aggregate_violation = false;
};

// A deliberately simple oracle for small tests. It materializes every commit
// prefix and searches all permitted snapshots directly; it does not share the
// production engine's delta rollback algorithm.
class ReferenceSOWWModel {
public:
  ReferenceSOWWModel();

  [[nodiscard]] ReferenceResult consume(const OrderedTxnMeta &meta,
                                        std::span<const EventView> events);

private:
  struct BytesLess {
    [[nodiscard]] bool operator()(const OwnedBytes &lhs,
                                  const OwnedBytes &rhs) const;
  };

  struct SessionLess {
    [[nodiscard]] bool operator()(const SessionUid &lhs,
                                  const SessionUid &rhs) const;
  };

  struct TaggedValue {
    ValueState state = ValueState::Absent;
    OwnedBytes bytes;

    friend bool operator==(const TaggedValue &, const TaggedValue &) = default;
  };

  struct ReadObservation {
    TaggedValue result;
  };

  struct SessionProgress {
    std::uint64_t session_seq = 0;
    CommitSeq commit_seq      = 0;
  };

  using Snapshot = std::map<OwnedBytes, OwnedBytes, BytesLess>;
  using ReadMap  = std::map<OwnedBytes, ReadObservation, BytesLess>;
  using WriteMap = std::map<OwnedBytes, TaggedValue, BytesLess>;

  std::vector<Snapshot> snapshots_;
  std::map<SessionUid, SessionProgress, SessionLess> sessions_;
  std::map<OwnedBytes, CommitSeq, BytesLess> latest_writers_;
  bool aggregate_violation_ = false;
};

} // namespace co_checker::online::test
