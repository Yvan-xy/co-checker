#include "reference_model.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <tuple>
#include <utility>

namespace co_checker::online::test {

namespace {

[[nodiscard]] OwnedBytes copy_bytes(const ByteView bytes) {
  return OwnedBytes(bytes.begin(), bytes.end());
}

template <typename Tagged>
[[nodiscard]] Tagged copy_result(const EventView &event) {
  Tagged result;
  result.state = event.value_state;
  if (event.value_state == ValueState::Present) {
    result.bytes = copy_bytes(event.value);
  }
  return result;
}

[[nodiscard]] std::optional<CommitSeq>
max_bound(const std::optional<CommitSeq> lhs,
          const std::optional<CommitSeq> rhs) {
  if (!lhs.has_value()) {
    return rhs;
  }
  if (!rhs.has_value()) {
    return lhs;
  }
  return std::max(*lhs, *rhs);
}

} // namespace

ReferenceSOWWModel::ReferenceSOWWModel() { snapshots_.emplace_back(); }

bool ReferenceSOWWModel::BytesLess::operator()(const OwnedBytes &lhs,
                                               const OwnedBytes &rhs) const {
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(),
                                      rhs.end());
}

bool ReferenceSOWWModel::SessionLess::operator()(const SessionUid &lhs,
                                                 const SessionUid &rhs) const {
  return std::tie(lhs.runtime_epoch.words[0], lhs.runtime_epoch.words[1],
                  lhs.producer_id, lhs.local_session_counter) <
         std::tie(rhs.runtime_epoch.words[0], rhs.runtime_epoch.words[1],
                  rhs.producer_id, rhs.local_session_counter);
}

ReferenceResult
ReferenceSOWWModel::consume(const OrderedTxnMeta &meta,
                            const std::span<const EventView> events) {
  assert(meta.commit_seq + 1U == snapshots_.size());

  ReadMap reads;
  WriteMap writes;
  bool repeated_read_disagreement = false;

  for (const auto &event : events) {
    const auto key = copy_bytes(event.key);
    if (event.op == EventOp::Read &&
        event.read_source == ReadSource::Snapshot) {
      const auto observed = copy_result<TaggedValue>(event);
      const auto [read_it, inserted] =
          reads.try_emplace(key, ReadObservation{.result = observed});
      if (!inserted && read_it->second.result != observed) {
        repeated_read_disagreement = true;
      }
    } else if (event.op == EventOp::Write) {
      writes.insert_or_assign(key, copy_result<TaggedValue>(event));
    }
  }

  std::optional<CommitSeq> session_bound;
  const auto session = sessions_.find(meta.session_id);
  if (session != sessions_.end()) {
    assert(meta.session_seq > session->second.session_seq);
    session_bound = session->second.commit_seq;
  }

  std::optional<CommitSeq> write_bound;
  for (const auto &[key, value] : writes) {
    (void)value;
    const auto writer = latest_writers_.find(key);
    if (writer != latest_writers_.end()) {
      write_bound = max_bound(write_bound, writer->second);
    }
  }
  const auto lower_bound = max_bound(session_bound, write_bound);

  bool compatible           = false;
  const auto first_snapshot = lower_bound.has_value()
                                  ? static_cast<std::size_t>(*lower_bound + 1U)
                                  : 0U;
  for (std::size_t snapshot_index = first_snapshot;
       snapshot_index < snapshots_.size(); ++snapshot_index) {
    const auto &snapshot = snapshots_[snapshot_index];
    bool matches         = true;
    for (const auto &[key, observation] : reads) {
      const auto value = snapshot.find(key);
      if (observation.result.state == ValueState::Absent) {
        matches = matches && value == snapshot.end();
      } else {
        matches = matches && value != snapshot.end() &&
                  value->second == observation.result.bytes;
      }
      if (!matches) {
        break;
      }
    }
    if (matches) {
      compatible = true;
      break;
    }
  }

  ReferenceResult result;
  if (repeated_read_disagreement) {
    result.transaction_violation = ViolationKind::InconsistentRepeatedRead;
  } else if (!compatible) {
    result.transaction_violation = ViolationKind::NoCompatibleSnapshot;
  }

  auto next_snapshot = snapshots_.back();
  for (const auto &[key, value] : writes) {
    if (value.state == ValueState::Present) {
      next_snapshot.insert_or_assign(key, value.bytes);
    } else {
      next_snapshot.erase(key);
    }
    latest_writers_.insert_or_assign(key, meta.commit_seq);
  }
  snapshots_.push_back(std::move(next_snapshot));
  sessions_.insert_or_assign(meta.session_id,
                             SessionProgress{.session_seq = meta.session_seq,
                                             .commit_seq  = meta.commit_seq});

  aggregate_violation_ =
      aggregate_violation_ || result.transaction_violation.has_value();
  result.aggregate_violation = aggregate_violation_;
  return result;
}

} // namespace co_checker::online::test
