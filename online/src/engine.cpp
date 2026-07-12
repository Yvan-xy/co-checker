#include "co_checker/online/engine.h"

#include "co_checker/online/packed_batch.h"
#include "co_checker/online/repeated_read_table.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace co_checker::online {

namespace {

class RetainedStateLimitError final : public std::length_error {
public:
  using std::length_error::length_error;
};

[[nodiscard]] bool equal_bytes(const ByteView lhs,
                               const ByteView rhs) noexcept {
  return lhs.size() == rhs.size() &&
         (lhs.empty() || std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0);
}

using KeyId   = std::uint32_t;
using ValueId = std::uint32_t;

template <typename Id> class ByteInterner {
public:
  static_assert(std::is_unsigned_v<Id>);

  struct Lookup {
    std::optional<Id> id;
    std::uint64_t hash = 0;
  };

  [[nodiscard]] Lookup lookup(const ByteView bytes) const noexcept {
    const auto hash = repeated_read_hash(bytes);
    if (index_.empty()) {
      return Lookup{std::nullopt, hash};
    }
    auto slot = static_cast<std::size_t>(hash) & (index_.size() - 1U);
    for (;;) {
      const auto &entry = index_[slot];
      if (!entry.occupied) {
        return Lookup{std::nullopt, hash};
      }
      if (entry.hash == hash &&
          equal_bytes(ByteView(values_[entry.id]), bytes)) {
        return Lookup{entry.id, hash};
      }
      slot = (slot + 1U) & (index_.size() - 1U);
    }
  }

  [[nodiscard]] std::optional<Id> find(const ByteView bytes) const noexcept {
    return lookup(bytes).id;
  }

  [[nodiscard]] Id insert_new(const ByteView bytes, const std::uint64_t hash) {
    if (values_.size() >=
        static_cast<std::size_t>(std::numeric_limits<Id>::max())) {
      throw std::length_error("byte interner identity space exhausted");
    }
    ensure_index_capacity();
    const auto id = static_cast<Id>(values_.size());
    OwnedBytes owned(bytes.begin(), bytes.end());
    values_.push_back(std::move(owned));
    insert_index(IndexEntry{hash, id, true});
    byte_count_ += bytes.size();
    return id;
  }

  [[nodiscard]] const OwnedBytes &at(const Id id) const {
    return values_.at(static_cast<std::size_t>(id));
  }

  void clear() {
    values_.clear();
    std::fill(index_.begin(), index_.end(), IndexEntry{});
    index_size_ = 0;
    byte_count_ = 0;
  }

  [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
  [[nodiscard]] std::size_t byte_count() const noexcept { return byte_count_; }

private:
  struct IndexEntry {
    std::uint64_t hash = 0;
    Id id              = 0;
    bool occupied      = false;
  };

  void ensure_index_capacity() {
    if (!index_.empty() && (index_size_ + 1U) * 8U <= index_.size() * 7U) {
      return;
    }
    const auto new_capacity = index_.empty() ? 16U : index_.size() * 2U;
    auto old_index          = std::move(index_);
    index_.assign(new_capacity, IndexEntry{});
    index_size_ = 0;
    for (const auto &entry : old_index) {
      if (entry.occupied) {
        insert_index(entry);
      }
    }
  }

  void insert_index(const IndexEntry entry) noexcept {
    auto slot = static_cast<std::size_t>(entry.hash) & (index_.size() - 1U);
    while (index_[slot].occupied) {
      slot = (slot + 1U) & (index_.size() - 1U);
    }
    index_[slot] = entry;
    ++index_size_;
  }

  std::vector<IndexEntry> index_;
  std::size_t index_size_ = 0;
  std::vector<OwnedBytes> values_;
  std::size_t byte_count_ = 0;
};

void hash_combine(std::size_t &seed, const std::uint64_t value) noexcept {
  seed ^= std::hash<std::uint64_t>{}(value) +
          static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (seed << 6U) +
          (seed >> 2U);
}

struct SessionUidHash {
  [[nodiscard]] std::size_t
  operator()(const SessionUid &session) const noexcept {
    std::size_t seed = 0;
    hash_combine(seed, session.runtime_epoch.words[0]);
    hash_combine(seed, session.runtime_epoch.words[1]);
    hash_combine(seed, session.producer_id);
    hash_combine(seed, session.local_session_counter);
    return seed;
  }
};

struct TaggedValue {
  ValueState state = ValueState::Absent;
  ByteView bytes;
};

struct ReadObservation {
  TaggedValue result;
  EventSeq first_sequence = 0;
};

struct DeltaEntry {
  KeyId key = 0;
  std::optional<ValueId> old_value;
};

struct HistoryEntry {
  CommitSeq commit_seq = 0;
  std::vector<DeltaEntry> delta;
};

struct SessionProgress {
  std::uint64_t session_seq = 0;
  CommitSeq commit_seq      = 0;
};

using Snapshot      = std::vector<std::optional<ValueId>>;
using ReadMap       = std::unordered_map<KeyId, ReadObservation>;
using FinalWriteMap = std::unordered_map<KeyId, TaggedValue>;
using WrongKeySet   = std::unordered_set<KeyId>;
using LastWriterMap = std::vector<std::optional<CommitSeq>>;
using SessionMap =
    std::unordered_map<SessionUid, SessionProgress, SessionUidHash>;

struct PendingRead {
  ByteView key;
  ReadObservation observation;
};

[[nodiscard]] OwnedBytes copy_bytes(const ByteView bytes) {
  return OwnedBytes(bytes.begin(), bytes.end());
}

[[nodiscard]] TaggedValue borrowed_tagged_value(const EventView &event) {
  return TaggedValue{event.value_state, event.value_state == ValueState::Present
                                            ? event.value
                                            : ByteView{}};
}

[[nodiscard]] bool valid_event_op(const EventOp op) noexcept {
  switch (op) {
  case EventOp::Read:
  case EventOp::Write:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_value_state(const ValueState state) noexcept {
  switch (state) {
  case ValueState::Present:
  case ValueState::Absent:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_read_source(const ReadSource source) noexcept {
  switch (source) {
  case ReadSource::Snapshot:
  case ReadSource::Local:
    return true;
  }
  return false;
}

[[nodiscard]] bool byte_less(const OwnedBytes &lhs,
                             const OwnedBytes &rhs) noexcept {
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(),
                                      rhs.end());
}

[[nodiscard]] std::optional<CommitSeq>
max_bound(const std::optional<CommitSeq> lhs,
          const std::optional<CommitSeq> rhs) noexcept {
  if (!lhs.has_value()) {
    return rhs;
  }
  if (!rhs.has_value()) {
    return lhs;
  }
  return std::max(*lhs, *rhs);
}

[[nodiscard]] bool matches_snapshot(const TaggedValue &observed,
                                    const Snapshot &snapshot,
                                    const ByteInterner<ValueId> &values,
                                    const KeyId key) {
  const auto index = static_cast<std::size_t>(key);
  const auto current =
      index < snapshot.size() ? snapshot[index] : std::optional<ValueId>{};
  if (observed.state == ValueState::Absent) {
    return !current.has_value();
  }
  return current.has_value() &&
         equal_bytes(ByteView(values.at(*current)), observed.bytes);
}

[[nodiscard]] bool matches_old_value(const TaggedValue &observed,
                                     const std::optional<ValueId> &old_value,
                                     const ByteInterner<ValueId> &values) {
  if (observed.state == ValueState::Absent) {
    return !old_value.has_value();
  }
  return old_value.has_value() &&
         equal_bytes(ByteView(values.at(*old_value)), observed.bytes);
}

} // namespace

class SnapshotSOWWEngine::Impl {
public:
  explicit Impl(SnapshotSOWWOptions options) : options_(options) {}

  [[nodiscard]] ConsumeResult consume(const OrderedTxnMeta &meta,
                                      const std::span<const EventView> events) {
    return guarded_consume(meta.commit_seq, [&] {
      return consume_range(meta, events, events.size());
    });
  }

  [[nodiscard]] ConsumeResult consume(const ValidatedTxnView &transaction) {
    const auto events = transaction.events();
    return guarded_consume(transaction.meta().commit_seq, [&] {
      return consume_range(transaction.meta(), events,
                           transaction.event_count());
    });
  }

private:
  template <typename Consumer>
  [[nodiscard]] ConsumeResult guarded_consume(const CommitSeq commit_seq,
                                              Consumer &&consumer) {
    try {
      return std::forward<Consumer>(consumer)();
    } catch (const RetainedStateLimitError &error) {
      return fail(EngineErrorCode::RetainedStateLimitExceeded, error.what(),
                  commit_seq, std::nullopt);
    } catch (const std::length_error &error) {
      return fail(EngineErrorCode::ResourceExhausted, error.what(), commit_seq,
                  std::nullopt);
    } catch (const std::bad_alloc &) {
      return fail(EngineErrorCode::ResourceExhausted,
                  "checker state allocation failed", commit_seq, std::nullopt);
    }
  }

  template <typename EventRange>
  [[nodiscard]] ConsumeResult consume_range(const OrderedTxnMeta &meta,
                                            const EventRange &events,
                                            const std::size_t event_count) {
    if (sticky_error_.has_value()) {
      return error_result();
    }

    if (meta.commit_seq != next_expected_commit_seq_) {
      return fail(EngineErrorCode::UnexpectedCommitSequence,
                  "expected commit sequence " +
                      std::to_string(next_expected_commit_seq_) +
                      ", received " + std::to_string(meta.commit_seq),
                  meta.commit_seq, std::nullopt);
    }

    const auto session_it = latest_by_session_.find(meta.session_id);
    if (session_it != latest_by_session_.end() &&
        meta.session_seq <= session_it->second.session_seq) {
      return fail(EngineErrorCode::NonIncreasingSessionSequence,
                  "session sequence must increase strictly", meta.commit_seq,
                  std::nullopt);
    }

    if (const auto validation_error = validate_events(meta, events);
        validation_error.has_value()) {
      sticky_error_ = *validation_error;
      return error_result();
    }

    ReadMap reads;
    std::vector<PendingRead> pending_reads;
    std::vector<OwnedBytes> permanently_wrong_keys;
    FinalWriteMap final_writes;
    reads.reserve(event_count);
    pending_reads.reserve(event_count);
    final_writes.reserve(event_count);
    repeated_reads_.reset(event_count);

    struct BorrowedReadGuard {
      RepeatedReadTable &table;
      ~BorrowedReadGuard() { table.clear(); }
    } borrowed_read_guard{repeated_reads_};

    std::optional<Violation> repeated_read_violation;
    for (const auto event : events) {
      if (event.op == EventOp::Read &&
          event.read_source == ReadSource::Snapshot) {
        const auto repeated = repeated_reads_.insert(event);
        if (repeated.kind == RepeatedReadInsertKind::Inserted) {
          pending_reads.push_back(PendingRead{
              event.key, ReadObservation{.result = borrowed_tagged_value(event),
                                         .first_sequence = event.sequence}});
          continue;
        }
        if (repeated.kind == RepeatedReadInsertKind::ConflictingResult) {
          const auto key = copy_bytes(event.key);
          if (!repeated_read_violation.has_value()) {
            repeated_read_violation =
                make_violation(ViolationKind::InconsistentRepeatedRead, meta);
            repeated_read_violation->first_event_sequence =
                repeated.first_sequence;
            repeated_read_violation->conflicting_event_sequence =
                event.sequence;
          }
          const auto already_recorded =
              std::find(repeated_read_violation->keys.begin(),
                        repeated_read_violation->keys.end(), key);
          if (already_recorded == repeated_read_violation->keys.end()) {
            repeated_read_violation->keys.push_back(key);
          }
        }
      } else if (event.op == EventOp::Write) {
        const auto key = intern_key(event.key);
        final_writes.insert_or_assign(key, borrowed_tagged_value(event));
      }
    }

    // A key that has never been written cannot have any earlier present
    // version. Keep such reads borrowed and out of the persistent key interner;
    // an Absent observation is always compatible, while Present is impossible
    // for every candidate prefix. A same-transaction final write has already
    // interned the key above and is therefore handled by the ordinary map.
    for (const auto &pending : pending_reads) {
      if (const auto key = keys_.find(pending.key); key.has_value()) {
        reads.emplace(*key, pending.observation);
      } else if (pending.observation.result.state == ValueState::Present) {
        permanently_wrong_keys.push_back(copy_bytes(pending.key));
      }
    }

    std::optional<CommitSeq> session_lower_bound;
    if (session_it != latest_by_session_.end()) {
      session_lower_bound = session_it->second.commit_seq;
    }

    std::optional<CommitSeq> write_write_lower_bound;
    for (const auto &[key, value] : final_writes) {
      (void)value;
      const auto index  = static_cast<std::size_t>(key);
      const auto writer = index < latest_writer_by_key_.size()
                              ? latest_writer_by_key_[index]
                              : std::optional<CommitSeq>{};
      if (writer.has_value()) {
        write_write_lower_bound = max_bound(write_write_lower_bound, writer);
      }
    }
    const auto effective_lower_bound =
        max_bound(session_lower_bound, write_write_lower_bound);

    std::optional<Violation> transaction_violation;
    if (repeated_read_violation.has_value()) {
      repeated_read_violation->session_lower_bound = session_lower_bound;
      repeated_read_violation->write_write_lower_bound =
          write_write_lower_bound;
      repeated_read_violation->effective_lower_bound = effective_lower_bound;
      transaction_violation = std::move(repeated_read_violation);
    } else {
      WrongKeySet wrong_keys;
      const auto persistent_compatible =
          has_compatible_snapshot(reads, effective_lower_bound, wrong_keys);
      const bool compatible =
          persistent_compatible && permanently_wrong_keys.empty();
      if (!compatible) {
        auto violation =
            make_violation(ViolationKind::NoCompatibleSnapshot, meta);
        violation.session_lower_bound     = session_lower_bound;
        violation.write_write_lower_bound = write_write_lower_bound;
        violation.effective_lower_bound   = effective_lower_bound;
        violation.keys.reserve(wrong_keys.size() +
                               permanently_wrong_keys.size());
        for (const auto key : wrong_keys) {
          violation.keys.push_back(keys_.at(key));
        }
        for (auto &key : permanently_wrong_keys) {
          violation.keys.push_back(std::move(key));
        }
        std::sort(violation.keys.begin(), violation.keys.end(), byte_less);
        transaction_violation = std::move(violation);
      }
    }

    // The database transaction is already committed. Its final writes and
    // session position must become visible even when this transaction violates
    // SOWW, so later diagnostic continuation observes the real committed state.
    apply_writes(meta.commit_seq, final_writes);
    latest_by_session_.insert_or_assign(
        meta.session_id, SessionProgress{.session_seq = meta.session_seq,
                                         .commit_seq  = meta.commit_seq});
    ++next_expected_commit_seq_;

    if (transaction_violation.has_value() && !first_violation_.has_value()) {
      first_violation_ = transaction_violation;
    }

    ConsumeResult result;
    result.verdict               = current_verdict();
    result.transaction_violation = std::move(transaction_violation);
    return result;
  }

public:
  void reset() {
    next_expected_commit_seq_ = 0;
    current_snapshot_.clear();
    latest_by_session_.clear();
    latest_writer_by_key_.clear();
    history_.clear();
    keys_.clear();
    values_.clear();
    repeated_reads_.clear();
    retained_state_bytes_      = 0;
    peak_retained_state_bytes_ = 0;
    current_snapshot_keys_     = 0;
    history_deltas_            = 0;
    history_logical_bytes_     = 0;
    first_violation_.reset();
    sticky_error_.reset();
  }

  [[nodiscard]] CommitSeq next_expected_commit_seq() const noexcept {
    return next_expected_commit_seq_;
  }

  [[nodiscard]] std::optional<ConsistencyVerdict> verdict() const {
    if (sticky_error_.has_value()) {
      return std::nullopt;
    }
    return current_verdict();
  }

  [[nodiscard]] std::optional<EngineError> error() const {
    return sticky_error_;
  }

  [[nodiscard]] SnapshotSOWWMetrics metrics() const {
    return SnapshotSOWWMetrics{
        .retained_state_capacity_bytes = options_.retained_state_capacity_bytes,
        .retained_state_bytes          = retained_state_bytes_,
        .peak_retained_state_bytes     = peak_retained_state_bytes_,
        .interned_key_count            = keys_.size(),
        .interned_key_bytes            = keys_.byte_count(),
        .interned_value_count          = values_.size(),
        .interned_value_bytes          = values_.byte_count(),
        .current_snapshot_keys         = current_snapshot_keys_,
        .history_entries               = history_.size(),
        .history_deltas                = history_deltas_,
        .history_logical_bytes         = history_logical_bytes_,
        .repeated_reads                = repeated_reads_.metrics(),
    };
  }

private:
  [[nodiscard]] static Violation make_violation(const ViolationKind kind,
                                                const OrderedTxnMeta &meta) {
    Violation violation;
    violation.kind           = kind;
    violation.commit_seq     = meta.commit_seq;
    violation.transaction_id = meta.transaction_id;
    violation.session_id     = meta.session_id;
    violation.session_seq    = meta.session_seq;
    return violation;
  }

  template <typename EventRange>
  [[nodiscard]] std::optional<EngineError>
  validate_events(const OrderedTxnMeta &meta, const EventRange &events) const {
    std::optional<EventSeq> previous_sequence;
    for (const auto event : events) {
      if (previous_sequence.has_value() &&
          event.sequence <= *previous_sequence) {
        return EngineError{
            .code           = EngineErrorCode::NonIncreasingEventSequence,
            .message        = "event sequence must increase strictly",
            .commit_seq     = meta.commit_seq,
            .event_sequence = event.sequence,
        };
      }
      previous_sequence = event.sequence;

      if (!valid_event_op(event.op)) {
        return EngineError{
            .code           = EngineErrorCode::InvalidEventOperation,
            .message        = "event operation is invalid",
            .commit_seq     = meta.commit_seq,
            .event_sequence = event.sequence,
        };
      }
      if (!valid_value_state(event.value_state)) {
        return EngineError{
            .code           = EngineErrorCode::InvalidValueState,
            .message        = "event value state is invalid",
            .commit_seq     = meta.commit_seq,
            .event_sequence = event.sequence,
        };
      }
      if (event.value_state == ValueState::Absent && !event.value.empty()) {
        return EngineError{
            .code           = EngineErrorCode::AbsentValueHasPayload,
            .message        = "an absent value cannot carry a payload",
            .commit_seq     = meta.commit_seq,
            .event_sequence = event.sequence,
        };
      }
      if (event.op == EventOp::Read && !valid_read_source(event.read_source)) {
        return EngineError{
            .code           = EngineErrorCode::InvalidReadSource,
            .message        = "read source is invalid",
            .commit_seq     = meta.commit_seq,
            .event_sequence = event.sequence,
        };
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool
  has_compatible_snapshot(const ReadMap &reads,
                          const std::optional<CommitSeq> lower_bound,
                          WrongKeySet &wrong_keys) const {
    wrong_keys.clear();
    wrong_keys.reserve(reads.size());
    for (const auto &[key, observation] : reads) {
      if (!matches_snapshot(observation.result, current_snapshot_, values_,
                            key)) {
        wrong_keys.insert(key);
      }
    }
    if (wrong_keys.empty()) {
      return true;
    }

    // Starting with the latest prefix, roll committed deltas backward. If a
    // lower bound L exists, the snapshot after L is the oldest permitted one,
    // so L's own delta is never rolled back.
    for (auto history_it = history_.rbegin(); history_it != history_.rend();
         ++history_it) {
      if (lower_bound.has_value() && history_it->commit_seq <= *lower_bound) {
        break;
      }

      for (const auto &delta : history_it->delta) {
        const auto read_it = reads.find(delta.key);
        if (read_it == reads.end()) {
          continue;
        }
        if (matches_old_value(read_it->second.result, delta.old_value,
                              values_)) {
          wrong_keys.erase(delta.key);
        } else {
          wrong_keys.insert(delta.key);
        }
      }
      if (wrong_keys.empty()) {
        return true;
      }
    }
    return false;
  }

  void apply_writes(const CommitSeq commit_seq,
                    const FinalWriteMap &final_writes) {
    if (final_writes.empty()) {
      return;
    }

    struct PreparedWrite {
      KeyId key        = 0;
      ValueState state = ValueState::Absent;
      std::optional<ValueId> value;
    };
    std::vector<PreparedWrite> prepared;
    prepared.reserve(final_writes.size());
    for (const auto &[key, value] : final_writes) {
      std::optional<ValueId> value_id;
      if (value.state == ValueState::Present) {
        value_id = intern_value(value.bytes);
      }
      prepared.push_back(PreparedWrite{key, value.state, value_id});
    }

    if (final_writes.size() >
        (std::numeric_limits<std::size_t>::max() - sizeof(HistoryEntry)) /
            sizeof(DeltaEntry)) {
      throw std::length_error("history byte accounting overflow");
    }
    const auto history_bytes =
        sizeof(HistoryEntry) + final_writes.size() * sizeof(DeltaEntry);
    ensure_retained_capacity(history_bytes);

    HistoryEntry history_entry;
    history_entry.commit_seq = commit_seq;
    history_entry.delta.reserve(final_writes.size());

    for (const auto &write : prepared) {
      const auto index = static_cast<std::size_t>(write.key);
      DeltaEntry delta;
      delta.key       = write.key;
      delta.old_value = current_snapshot_[index];
      history_entry.delta.push_back(std::move(delta));

      const auto was_present = current_snapshot_[index].has_value();
      if (write.state == ValueState::Present) {
        current_snapshot_[index] = write.value;
        if (!was_present) {
          ++current_snapshot_keys_;
        }
      } else {
        current_snapshot_[index].reset();
        if (was_present) {
          --current_snapshot_keys_;
        }
      }
      latest_writer_by_key_[index] = commit_seq;
    }
    history_.push_back(std::move(history_entry));
    charge_retained(history_bytes);
    history_deltas_ += final_writes.size();
    history_logical_bytes_ += history_bytes;
  }

  [[nodiscard]] KeyId intern_key(const ByteView bytes) {
    const auto lookup = keys_.lookup(bytes);
    if (lookup.id.has_value()) {
      return *lookup.id;
    }
    ensure_retained_capacity(bytes.size());
    const auto id = keys_.insert_new(bytes, lookup.hash);
    charge_retained(bytes.size());
    const auto required = static_cast<std::size_t>(id) + 1U;
    current_snapshot_.resize(required);
    latest_writer_by_key_.resize(required);
    return id;
  }

  [[nodiscard]] ValueId intern_value(const ByteView bytes) {
    const auto lookup = values_.lookup(bytes);
    if (lookup.id.has_value()) {
      return *lookup.id;
    }
    ensure_retained_capacity(bytes.size());
    const auto id = values_.insert_new(bytes, lookup.hash);
    charge_retained(bytes.size());
    return id;
  }

  void ensure_retained_capacity(const std::size_t additional) const {
    if (additional > options_.retained_state_capacity_bytes ||
        retained_state_bytes_ >
            options_.retained_state_capacity_bytes - additional) {
      throw RetainedStateLimitError(
          "configured retained checker-state byte limit exceeded");
    }
  }

  void charge_retained(const std::size_t additional) noexcept {
    retained_state_bytes_ += additional;
    peak_retained_state_bytes_ =
        std::max(peak_retained_state_bytes_, retained_state_bytes_);
  }

  [[nodiscard]] ConsistencyVerdict current_verdict() const {
    ConsistencyVerdict verdict;
    if (first_violation_.has_value()) {
      verdict.status          = ConsistencyStatus::Violation;
      verdict.first_violation = first_violation_;
    }
    return verdict;
  }

  [[nodiscard]] ConsumeResult
  fail(const EngineErrorCode code, std::string message,
       const std::optional<CommitSeq> commit_seq,
       const std::optional<EventSeq> event_sequence) {
    if (!sticky_error_.has_value()) {
      sticky_error_ = EngineError{
          .code           = code,
          .message        = std::move(message),
          .commit_seq     = commit_seq,
          .event_sequence = event_sequence,
      };
    }
    return error_result();
  }

  [[nodiscard]] ConsumeResult error_result() const {
    ConsumeResult result;
    result.error = sticky_error_;
    return result;
  }

  SnapshotSOWWOptions options_;
  CommitSeq next_expected_commit_seq_ = 0;
  Snapshot current_snapshot_;
  SessionMap latest_by_session_;
  LastWriterMap latest_writer_by_key_;
  std::vector<HistoryEntry> history_;
  ByteInterner<KeyId> keys_;
  ByteInterner<ValueId> values_;
  RepeatedReadTable repeated_reads_;
  std::size_t retained_state_bytes_      = 0;
  std::size_t peak_retained_state_bytes_ = 0;
  std::size_t current_snapshot_keys_     = 0;
  std::size_t history_deltas_            = 0;
  std::size_t history_logical_bytes_     = 0;
  std::optional<Violation> first_violation_;
  std::optional<EngineError> sticky_error_;
};

SnapshotSOWWEngine::SnapshotSOWWEngine(SnapshotSOWWOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

SnapshotSOWWEngine::~SnapshotSOWWEngine() = default;

SnapshotSOWWEngine::SnapshotSOWWEngine(SnapshotSOWWEngine &&) noexcept =
    default;

SnapshotSOWWEngine &
SnapshotSOWWEngine::operator=(SnapshotSOWWEngine &&) noexcept = default;

ConsumeResult
SnapshotSOWWEngine::consume(const OrderedTxnMeta &meta,
                            const std::span<const EventView> events) {
  return impl_->consume(meta, events);
}

ConsumeResult SnapshotSOWWEngine::consume(const ValidatedTxnView &transaction) {
  return impl_->consume(transaction);
}

void SnapshotSOWWEngine::reset() { impl_->reset(); }

CommitSeq SnapshotSOWWEngine::next_expected_commit_seq() const noexcept {
  return impl_->next_expected_commit_seq();
}

std::optional<ConsistencyVerdict> SnapshotSOWWEngine::verdict() const {
  return impl_->verdict();
}

std::optional<EngineError> SnapshotSOWWEngine::error() const {
  return impl_->error();
}

SnapshotSOWWMetrics SnapshotSOWWEngine::metrics() const {
  return impl_->metrics();
}

} // namespace co_checker::online
