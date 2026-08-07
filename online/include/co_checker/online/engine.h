#pragma once

#include <limits>
#include <memory>
#include <span>

#include "co_checker/online/repeated_read_table.h"
#include "co_checker/online/result.h"
#include "co_checker/online/types.h"

namespace co_checker::online {

class ValidatedTxnView;

struct SnapshotSOWWOptions {
  // Exact logical bytes retained by key/value interning and rollback history.
  // Container bookkeeping is reported separately by process memory tools.
  std::size_t retained_state_capacity_bytes =
      std::numeric_limits<std::size_t>::max();
};

struct SnapshotSOWWMetrics {
  std::size_t retained_state_capacity_bytes = 0;
  std::size_t retained_state_bytes          = 0;
  std::size_t peak_retained_state_bytes     = 0;
  std::size_t interned_key_count            = 0;
  std::size_t interned_key_bytes            = 0;
  std::size_t interned_value_count          = 0;
  std::size_t interned_value_bytes          = 0;
  std::size_t current_snapshot_keys         = 0;
  std::size_t history_entries               = 0;
  std::size_t history_deltas                = 0;
  std::size_t history_logical_bytes         = 0;
  std::uint64_t snapshot_read_events                     = 0;
  std::uint64_t unique_snapshot_reads                    = 0;
  std::uint64_t identical_snapshot_repeats               = 0;
  std::uint64_t conflicting_snapshot_repeats             = 0;
  std::uint64_t snapshot_read_packed_bytes               = 0;
  std::uint64_t duplicate_snapshot_read_packed_bytes     = 0;
  std::uint64_t identical_snapshot_repeat_packed_bytes   = 0;
  std::uint64_t conflicting_snapshot_repeat_packed_bytes = 0;
  std::uint64_t compatible_snapshot_checks               = 0;
  std::uint64_t history_entries_visited                  = 0;
  std::uint64_t history_deltas_visited                   = 0;
  std::uint64_t history_relevant_deltas_visited          = 0;
  std::uint64_t max_history_entries_visited              = 0;
  std::uint64_t max_history_deltas_visited               = 0;
  RepeatedReadTableMetrics repeated_reads;
};

class SnapshotSOWWEngine {
public:
  explicit SnapshotSOWWEngine(SnapshotSOWWOptions options = {});
  ~SnapshotSOWWEngine();

  SnapshotSOWWEngine(SnapshotSOWWEngine &&) noexcept;
  SnapshotSOWWEngine &operator=(SnapshotSOWWEngine &&) noexcept;

  SnapshotSOWWEngine(const SnapshotSOWWEngine &)            = delete;
  SnapshotSOWWEngine &operator=(const SnapshotSOWWEngine &) = delete;

  [[nodiscard]] ConsumeResult consume(const OrderedTxnMeta &meta,
                                      std::span<const EventView> events);
  // Traverses packed descriptors directly. EventView values are decoded one at
  // a time by the validated range; no intermediate EventView container is
  // allocated or retained.
  [[nodiscard]] ConsumeResult consume(const ValidatedTxnView &transaction);

  void reset();

  [[nodiscard]] CommitSeq next_expected_commit_seq() const noexcept;
  [[nodiscard]] std::optional<ConsistencyVerdict> verdict() const;
  [[nodiscard]] std::optional<EngineError> error() const;
  [[nodiscard]] SnapshotSOWWMetrics metrics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace co_checker::online
