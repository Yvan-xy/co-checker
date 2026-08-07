#include "co_checker/online/c_api.h"

#include "co_checker/online/runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace co_checker::online;

static_assert(sizeof(co_checker_online_options_v1) == 112);
static_assert(offsetof(co_checker_online_options_v1, mode) == 80);
static_assert(sizeof(co_checker_online_epoch_v1) == 16);
static_assert(sizeof(co_checker_online_uid_v1) == 16);
static_assert(sizeof(co_checker_online_session_v1) == 32);
static_assert(offsetof(co_checker_online_session_v1, producer_id) == 16);
static_assert(offsetof(co_checker_online_session_v1, local_session_counter) ==
              24);
static_assert(sizeof(co_checker_online_txn_input_header_v1) == 64);
static_assert(offsetof(co_checker_online_txn_input_header_v1, events_offset) ==
              40);
static_assert(sizeof(co_checker_online_event_input_v1) == 32);
static_assert(offsetof(co_checker_online_event_input_v1, key_offset) == 8);
static_assert(offsetof(co_checker_online_event_input_v1, reserved) == 24);
static_assert(sizeof(co_checker_online_verdict_v1) == 104);
static_assert(sizeof(co_checker_online_error_info_v1) == 64);

constexpr std::size_t kPackedDescriptorSize = sizeof(EventDescriptorV1);

enum class NativeMode : std::uint8_t {
  CaptureOnly,
  OrderOnly,
  Full,
};

enum class NativeOrderingProtocol : std::uint8_t {
  SerializedGlobalLock,
  FrontierReorder,
};

struct EntityLess {
  [[nodiscard]] bool operator()(const EntityUid &lhs,
                                const EntityUid &rhs) const noexcept {
    return lhs.words < rhs.words;
  }
};

struct SessionLess {
  [[nodiscard]] bool operator()(const SessionUid &lhs,
                                const SessionUid &rhs) const noexcept {
    return std::tie(lhs.runtime_epoch.words[0], lhs.runtime_epoch.words[1],
                    lhs.producer_id, lhs.local_session_counter) <
           std::tie(rhs.runtime_epoch.words[0], rhs.runtime_epoch.words[1],
                    rhs.producer_id, rhs.local_session_counter);
  }
};

[[nodiscard]] RuntimeError make_runtime_error(const RuntimeErrorCode code,
                                              std::string message) {
  RuntimeError result;
  result.code    = code;
  result.message = std::move(message);
  return result;
}

[[nodiscard]] RuntimeEpoch
to_runtime_epoch(const co_checker_online_epoch_v1 &epoch) noexcept {
  return RuntimeEpoch{{epoch.word0, epoch.word1}};
}

[[nodiscard]] co_checker_online_epoch_v1
to_c_epoch(const RuntimeEpoch &epoch) noexcept {
  return co_checker_online_epoch_v1{epoch.words[0], epoch.words[1]};
}

[[nodiscard]] EntityUid to_uid(const co_checker_online_uid_v1 &uid) noexcept {
  return EntityUid{{uid.word0, uid.word1}};
}

[[nodiscard]] co_checker_online_uid_v1 to_c_uid(const EntityUid &uid) noexcept {
  return co_checker_online_uid_v1{uid.words[0], uid.words[1]};
}

[[nodiscard]] std::optional<OrderTokenKind>
to_order_token_kind(const co_checker_online_order_token_kind_v1 kind) noexcept {
  switch (kind) {
  case CO_CHECKER_ONLINE_ORDER_TOKEN_WRITE_COMMIT_V1:
    return OrderTokenKind::WriteCommit;
  case CO_CHECKER_ONLINE_ORDER_TOKEN_READ_ONLY_START_V1:
    return OrderTokenKind::ReadOnlyStart;
  case CO_CHECKER_ONLINE_ORDER_TOKEN_READ_ONLY_COMPLETION_V1:
    return OrderTokenKind::ReadOnlyCompletion;
  case CO_CHECKER_ONLINE_ORDER_TOKEN_NO_WRITE_COMPLETION_V1:
    return OrderTokenKind::NoWriteCompletion;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] SessionUid
to_session(const co_checker_online_session_v1 &session) noexcept {
  return SessionUid{to_runtime_epoch(session.runtime_epoch),
                    session.producer_id, session.local_session_counter};
}

[[nodiscard]] co_checker_online_session_v1
to_c_session(const SessionUid &session) noexcept {
  return co_checker_online_session_v1{to_c_epoch(session.runtime_epoch),
                                      session.producer_id, 0,
                                      session.local_session_counter};
}

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

[[nodiscard]] RuntimeEpoch make_runtime_epoch() {
  static std::atomic<std::uint64_t> epoch_counter{1};
  std::random_device random;
  const auto random_word = [&] {
    const auto high = static_cast<std::uint64_t>(random());
    const auto low  = static_cast<std::uint64_t>(random());
    return (high << 32U) ^ low;
  };
  const auto counter = epoch_counter.fetch_add(1, std::memory_order_relaxed);
  const auto clock   = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  RuntimeEpoch result{{mix64(random_word() ^ counter),
                       mix64(random_word() ^ clock ^ (counter << 1U))}};
  if (result.words[0] == 0U && result.words[1] == 0U) {
    result.words[1] = counter == 0U ? 1U : counter;
  }
  return result;
}

[[nodiscard]] bool checked_add(const std::size_t lhs, const std::size_t rhs,
                               std::size_t &result) noexcept {
  if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
    return false;
  }
  result = lhs + rhs;
  return true;
}

[[nodiscard]] bool checked_multiply(const std::size_t lhs,
                                    const std::size_t rhs,
                                    std::size_t &result) noexcept {
  if (lhs != 0U && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    return false;
  }
  result = lhs * rhs;
  return true;
}

template <typename T>
[[nodiscard]] T load_unaligned(const std::byte *source) noexcept {
  T result{};
  std::memcpy(&result, source, sizeof(result));
  return result;
}

struct FlatInputView {
  co_checker_online_txn_input_header_v1 header{};
  const std::byte *bytes               = nullptr;
  std::size_t size                     = 0;
  std::size_t descriptor_end           = 0;
  std::size_t normalized_event_count   = 0;
  std::size_t estimated_reserved_bytes = 0;

  [[nodiscard]] co_checker_online_event_input_v1
  event(const std::size_t index) const noexcept {
    return load_unaligned<co_checker_online_event_input_v1>(
        bytes + static_cast<std::size_t>(header.events_offset) +
        index * sizeof(co_checker_online_event_input_v1));
  }

  [[nodiscard]] ByteView blob(const std::uint32_t offset,
                              const std::uint32_t length) const noexcept {
    return ByteView{bytes + offset, length};
  }
};

enum class FlatInputPurpose : std::uint8_t {
  FrontierCommitted,
  SerializedPrepared,
};

struct FlatParseResult {
  std::optional<FlatInputView> input;
  const char *error = nullptr;
};

[[nodiscard]] bool canonical_blob(const std::uint32_t offset,
                                  const std::uint32_t size,
                                  const std::size_t descriptor_end,
                                  const std::size_t buffer_size) noexcept {
  if (size == 0U) {
    return offset == 0U;
  }
  const auto start  = static_cast<std::size_t>(offset);
  const auto length = static_cast<std::size_t>(size);
  return start >= descriptor_end && start <= buffer_size &&
         length <= buffer_size - start;
}

[[nodiscard]] bool add_segment_capacity(const std::size_t capacity,
                                        std::size_t &total) noexcept {
  return checked_add(total, capacity, total);
}

[[nodiscard]] FlatParseResult
parse_flat_input(const void *flat_input, const std::uint64_t flat_input_size,
                 const std::size_t segment_size,
                 const FlatInputPurpose purpose) {
  if (flat_input == nullptr) {
    return {{}, "flat transaction input is null"};
  }
  if (flat_input_size < sizeof(co_checker_online_txn_input_header_v1) ||
      flat_input_size > std::numeric_limits<std::uint32_t>::max() ||
      flat_input_size > std::numeric_limits<std::size_t>::max()) {
    return {{}, "flat transaction input size is outside V1 limits"};
  }

  FlatInputView result;
  result.bytes = static_cast<const std::byte *>(flat_input);
  result.size  = static_cast<std::size_t>(flat_input_size);
  result.header =
      load_unaligned<co_checker_online_txn_input_header_v1>(result.bytes);
  if (result.header.struct_size !=
          sizeof(co_checker_online_txn_input_header_v1) ||
      result.header.abi_version != CO_CHECKER_ONLINE_ABI_VERSION_V1) {
    return {{}, "flat transaction header layout/version is invalid"};
  }
  if (result.header.total_size != flat_input_size ||
      result.header.events_offset !=
          sizeof(co_checker_online_txn_input_header_v1) ||
      result.header.reserved[0] != 0U || result.header.reserved[1] != 0U) {
    return {{}, "flat transaction header is non-canonical"};
  }
  if (result.header.read_only > 1U) {
    return {{}, "flat transaction read-only flag is invalid"};
  }
  if (result.header.start_ts == 0U) {
    return {{}, "transaction start timestamp must be nonzero"};
  }
  if (purpose == FlatInputPurpose::FrontierCommitted) {
    if ((result.header.read_only != 0U && result.header.commit_ts != 0U) ||
        (result.header.read_only == 0U &&
         (result.header.commit_ts == 0U ||
          result.header.commit_ts <= result.header.start_ts))) {
      return {{}, "transaction start/commit timestamp relation is invalid"};
    }
  } else if (result.header.commit_ts != 0U) {
    return {{}, "serialized prepared input must not carry a final commit timestamp"};
  }

  std::size_t descriptor_bytes = 0;
  if (!checked_multiply(static_cast<std::size_t>(result.header.event_count),
                        sizeof(co_checker_online_event_input_v1),
                        descriptor_bytes) ||
      !checked_add(static_cast<std::size_t>(result.header.events_offset),
                   descriptor_bytes, result.descriptor_end) ||
      result.descriptor_end > result.size) {
    return {{}, "flat transaction descriptor range is invalid"};
  }

  std::optional<EventSeq> previous_sequence;
  bool has_write = false;
  for (std::size_t index = 0; index < result.header.event_count; ++index) {
    const auto event = result.event(index);
    if (event.flags != 0U || event.reserved != 0U) {
      return {{}, "flat event reserved fields must be zero"};
    }
    if (previous_sequence.has_value() && event.sequence <= *previous_sequence) {
      return {{}, "flat event sequence must increase strictly"};
    }
    previous_sequence   = event.sequence;
    const auto is_read  = event.operation == CO_CHECKER_ONLINE_EVENT_READ_V1;
    const auto is_write = event.operation == CO_CHECKER_ONLINE_EVENT_WRITE_V1;
    if (!is_read && !is_write) {
      return {{}, "flat event operation is invalid"};
    }
    if (event.value_state != CO_CHECKER_ONLINE_VALUE_PRESENT_V1 &&
        event.value_state != CO_CHECKER_ONLINE_VALUE_ABSENT_V1) {
      return {{}, "flat event value state is invalid"};
    }
    if ((is_read && event.read_source != CO_CHECKER_ONLINE_READ_SNAPSHOT_V1 &&
         event.read_source != CO_CHECKER_ONLINE_READ_LOCAL_V1) ||
        (is_write && event.read_source != 0U)) {
      return {{}, "flat event read source is invalid"};
    }
    if (!canonical_blob(event.key_offset, event.key_size, result.descriptor_end,
                        result.size) ||
        !canonical_blob(event.value_offset, event.value_size,
                        result.descriptor_end, result.size)) {
      return {{}, "flat event blob range is invalid"};
    }
    if (event.value_state == CO_CHECKER_ONLINE_VALUE_ABSENT_V1 &&
        (event.value_offset != 0U || event.value_size != 0U)) {
      return {{}, "absent flat event value carries a payload"};
    }
    if (is_write) {
      has_write = true;
    }
    if (is_write || event.read_source == CO_CHECKER_ONLINE_READ_SNAPSHOT_V1) {
      ++result.normalized_event_count;
    }
  }
  if (result.header.read_only != 0U && has_write) {
    return {{}, "read-only token-kind input contains a normalized write"};
  }

  if (segment_size < kPackedDescriptorSize) {
    return {{}, "chunk segment size cannot store one descriptor"};
  }
  const auto descriptors_per_segment = segment_size / kPackedDescriptorSize;
  const auto descriptor_segment_count =
      result.normalized_event_count == 0U
          ? 0U
          : 1U + (result.normalized_event_count - 1U) / descriptors_per_segment;
  std::size_t descriptor_reserved = 0;
  if (!checked_multiply(descriptor_segment_count, segment_size,
                        descriptor_reserved)) {
    return {{}, "chunk descriptor reservation overflows"};
  }
  result.estimated_reserved_bytes = sizeof(BatchHeaderV1);
  if (!checked_add(result.estimated_reserved_bytes, descriptor_reserved,
                   result.estimated_reserved_bytes)) {
    return {{}, "chunk reservation overflows"};
  }

  bool tail_is_ordinary = false;
  std::size_t tail_used = 0;
  const auto store_blob = [&](const std::size_t blob_size,
                              std::size_t &reserved, bool &ordinary,
                              std::size_t &used) -> bool {
    if (blob_size == 0U) {
      return true;
    }
    if (blob_size > segment_size) {
      if (!add_segment_capacity(blob_size, reserved)) {
        return false;
      }
      ordinary = false;
      used     = blob_size;
      return true;
    }
    if (!ordinary || blob_size > segment_size - used) {
      if (!add_segment_capacity(segment_size, reserved)) {
        return false;
      }
      ordinary = true;
      used     = 0;
    }
    used += blob_size;
    return true;
  };

  for (std::size_t index = 0; index < result.header.event_count; ++index) {
    const auto event = result.event(index);
    const auto normalized =
        event.operation == CO_CHECKER_ONLINE_EVENT_WRITE_V1 ||
        event.read_source == CO_CHECKER_ONLINE_READ_SNAPSHOT_V1;
    if (!normalized) {
      continue;
    }
    const auto key_size = static_cast<std::size_t>(event.key_size);
    const auto value_size =
        event.value_state == CO_CHECKER_ONLINE_VALUE_PRESENT_V1
            ? static_cast<std::size_t>(event.value_size)
            : 0U;
    std::size_t combined = 0;
    if (!checked_add(key_size, value_size, combined)) {
      return {{}, "flat event payload size overflows"};
    }
    if (combined != 0U && combined <= segment_size) {
      if (!tail_is_ordinary || combined > segment_size - tail_used) {
        if (!add_segment_capacity(segment_size,
                                  result.estimated_reserved_bytes)) {
          return {{}, "chunk payload reservation overflows"};
        }
        tail_is_ordinary = true;
        tail_used        = 0;
      }
      tail_used += combined;
      continue;
    }
    if (!store_blob(key_size, result.estimated_reserved_bytes, tail_is_ordinary,
                    tail_used) ||
        !store_blob(value_size, result.estimated_reserved_bytes,
                    tail_is_ordinary, tail_used)) {
      return {{}, "chunk payload reservation overflows"};
    }
  }
  return {result, nullptr};
}

struct NativeConfiguration {
  RuntimeOptions runtime;
  ByteBudgetOptions capture;
  std::size_t chunk_segment_bytes = 0;
  std::size_t max_cached_segments = 0;
  std::uint64_t cluster_epoch     = 0;
  NativeMode mode                 = NativeMode::Full;
  NativeOrderingProtocol ordering_protocol =
      NativeOrderingProtocol::SerializedGlobalLock;
};

struct ConfigurationResult {
  std::optional<NativeConfiguration> configuration;
  co_checker_online_status_v1 status =
      CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
};

[[nodiscard]] bool reserved_options_are_zero(
    const co_checker_online_options_v1 &options) noexcept {
  return options.reserved[0] == 0U && options.reserved[1] == 0U &&
         options.reserved[2] == 0U;
}

[[nodiscard]] ConfigurationResult
parse_configuration(const co_checker_online_options_v1 *options) {
  if (options == nullptr ||
      options->struct_size != sizeof(co_checker_online_options_v1) ||
      options->abi_version != CO_CHECKER_ONLINE_ABI_VERSION_V1 ||
      !reserved_options_are_zero(*options)) {
    return {};
  }
  const auto fits_size = [](const std::uint64_t value) {
    return value <= std::numeric_limits<std::size_t>::max();
  };
  const auto fits_duration = [](const std::uint64_t value) {
    return value <=
           static_cast<std::uint64_t>(std::chrono::nanoseconds::max().count());
  };
  if (options->queue_capacity_bytes == 0U ||
      options->capture_capacity_bytes == 0U ||
      options->retained_state_capacity_bytes == 0U ||
      options->chunk_segment_bytes < kPackedDescriptorSize ||
      options->cluster_epoch == 0U ||
      !fits_size(options->queue_capacity_bytes) ||
      !fits_size(options->reorder_capacity_bytes) ||
      !fits_size(options->capture_capacity_bytes) ||
      !fits_size(options->retained_state_capacity_bytes) ||
      !fits_size(options->chunk_segment_bytes) ||
      !fits_size(options->max_cached_chunk_segments) ||
      !fits_duration(options->queue_timeout_ns) ||
      !fits_duration(options->capture_timeout_ns) ||
      (options->mode != CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1 &&
       options->mode != CO_CHECKER_ONLINE_MODE_ORDER_ONLY_V1 &&
       options->mode != CO_CHECKER_ONLINE_MODE_FULL_V1) ||
      (options->ordering_protocol !=
           CO_CHECKER_ONLINE_ORDERING_SERIALIZED_GLOBAL_LOCK_V1 &&
       options->ordering_protocol !=
           CO_CHECKER_ONLINE_ORDERING_FRONTIER_REORDER_V1) ||
      (options->ordering_protocol ==
           CO_CHECKER_ONLINE_ORDERING_SERIALIZED_GLOBAL_LOCK_V1 &&
       options->reorder_capacity_bytes != 0U) ||
      (options->ordering_protocol ==
           CO_CHECKER_ONLINE_ORDERING_FRONTIER_REORDER_V1 &&
       options->reorder_capacity_bytes == 0U)) {
    return {};
  }

  NativeConfiguration result;
  result.runtime.queue.capacity_bytes =
      static_cast<std::size_t>(options->queue_capacity_bytes);
  result.runtime.queue.push_timeout = std::chrono::nanoseconds{
      static_cast<std::int64_t>(options->queue_timeout_ns)};
  result.runtime.reorder_capacity_bytes =
      static_cast<std::size_t>(options->reorder_capacity_bytes);
  result.runtime.retained_state_capacity_bytes =
      static_cast<std::size_t>(options->retained_state_capacity_bytes);
  if (options->mode == CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1) {
    result.runtime.pipeline_mode = PipelineMode::CaptureOnly;
  } else if (options->mode == CO_CHECKER_ONLINE_MODE_ORDER_ONLY_V1) {
    result.runtime.pipeline_mode = PipelineMode::OrderOnly;
  } else {
    result.runtime.pipeline_mode = PipelineMode::Full;
  }
  if (options->ordering_protocol ==
      CO_CHECKER_ONLINE_ORDERING_FRONTIER_REORDER_V1) {
    result.runtime.ordering_protocol = OrderingProtocol::FrontierReorder;
    result.ordering_protocol = NativeOrderingProtocol::FrontierReorder;
  } else {
    result.runtime.ordering_protocol =
        OrderingProtocol::SerializedGlobalLock;
  }
  result.capture.capacity_bytes =
      static_cast<std::size_t>(options->capture_capacity_bytes);
  result.capture.reservation_timeout = std::chrono::nanoseconds{
      static_cast<std::int64_t>(options->capture_timeout_ns)};
  result.chunk_segment_bytes =
      static_cast<std::size_t>(options->chunk_segment_bytes);
  result.max_cached_segments =
      static_cast<std::size_t>(options->max_cached_chunk_segments);
  result.cluster_epoch = options->cluster_epoch;
  if (options->mode == CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1) {
    result.mode = NativeMode::CaptureOnly;
  } else if (options->mode == CO_CHECKER_ONLINE_MODE_ORDER_ONLY_V1) {
    result.mode = NativeMode::OrderOnly;
  }
  return {std::move(result), CO_CHECKER_ONLINE_STATUS_OK_V1};
}

struct AdapterFailure {
  co_checker_online_adapter_error_v1 code =
      CO_CHECKER_ONLINE_ADAPTER_ERROR_NONE_V1;
  std::uint32_t external_code = 0;
  std::string message;
};

struct SessionState {
  std::uint64_t last_session_seq = 0;
};

struct LogicalTransactionState {
  SessionUid session_id;
  std::uint64_t session_seq = 0;
};

struct PreparedEvidence {
  RuntimePreparedBatch runtime_batch;
  PackedBatchEnvelope capture_only_batch;
  std::uint32_t normalized_write_count = 0;
  std::uint32_t event_count             = 0;
  bool declared_read_only               = false;
};

struct AttemptIdentity {
  EntityUid attempt_id;
  EntityUid transaction_id;
  SessionUid session_id;
  std::uint64_t session_seq = 0;
  std::uint64_t start_ts    = 0;
};

struct AttemptState {
  enum class Phase : std::uint8_t { Registered, Preparing, Prepared, Publishing };

  EntityUid attempt_id;
  EntityUid transaction_id;
  SessionUid session_id;
  std::uint64_t session_seq = 0;
  std::uint64_t start_ts    = 0;
  Phase phase               = Phase::Registered;
  std::unique_ptr<PreparedEvidence> prepared;
};

struct ProducerState {
  std::mutex mutex;
  ProducerSeq last_sequence = 0;
};

class NativeRuntime final {
public:
  NativeRuntime(NativeConfiguration configuration,
                std::vector<ProducerId> producers)
      : epoch_(make_runtime_epoch()),
        cluster_epoch_(configuration.cluster_epoch), mode_(configuration.mode),
        ordering_protocol_(configuration.ordering_protocol),
        producers_(producers.begin(), producers.end()),
        chunk_pool_(
            std::make_shared<ChunkPool>(configuration.chunk_segment_bytes,
                                        configuration.max_cached_segments)),
        capture_budget_(configuration.capture),
        runtime_(runtime_options(std::move(configuration.runtime), epoch_,
                                 cluster_epoch_),
                 std::move(producers)) {
    for (const auto producer : producers_) {
      producer_state_.try_emplace(producer, std::make_unique<ProducerState>());
    }
  }

  ~NativeRuntime() { (void)close(); }

  NativeRuntime(const NativeRuntime &)            = delete;
  NativeRuntime &operator=(const NativeRuntime &) = delete;

  [[nodiscard]] RuntimeEpoch epoch() const noexcept { return epoch_; }

  [[nodiscard]] co_checker_online_status_v1
  create_session(const ProducerId producer, SessionUid &session_out) {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (const auto status = available_status();
        status != CO_CHECKER_ONLINE_STATUS_OK_V1) {
      return status;
    }
    if (!producers_.contains(producer)) {
      return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
    }
    std::lock_guard state_lock(state_mutex_);
    if (next_session_counter_ == std::numeric_limits<std::uint64_t>::max()) {
      return fail_resource("native session identity space is exhausted");
    }
    ++next_session_counter_;
    SessionUid session{epoch_, producer, next_session_counter_};
    sessions_.try_emplace(session);
    session_out = session;
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  }

  [[nodiscard]] co_checker_online_status_v1
  create_logical(const SessionUid &session, const std::uint64_t session_seq,
                 EntityUid &transaction_out) {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (const auto status = available_status();
        status != CO_CHECKER_ONLINE_STATUS_OK_V1) {
      return status;
    }
    if (session.runtime_epoch != epoch_ || session_seq == 0U) {
      return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
    }
    std::lock_guard state_lock(state_mutex_);
    const auto session_it = sessions_.find(session);
    if (session_it == sessions_.end() ||
        session_seq <= session_it->second.last_session_seq) {
      return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
    }
    if (next_logical_counter_ == std::numeric_limits<std::uint64_t>::max()) {
      return fail_resource(
          "native logical transaction identity space is exhausted");
    }
    ++next_logical_counter_;
    session_it->second.last_session_seq = session_seq;
    transaction_out                     = EntityUid{
                            {mix64(epoch_.words[0] ^ 0x10c1ca1ULL ^ next_logical_counter_),
                             next_logical_counter_}};
    logical_transactions_.try_emplace(
        transaction_out, LogicalTransactionState{.session_id  = session,
                                                 .session_seq = session_seq});
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  }

  [[nodiscard]] co_checker_online_status_v1
  register_attempt(const SessionUid &session, const EntityUid &transaction,
                   const std::uint64_t session_seq,
                   const std::uint64_t start_ts, EntityUid &attempt_out) {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (const auto status = available_status();
        status != CO_CHECKER_ONLINE_STATUS_OK_V1) {
      return status;
    }
    if (session.runtime_epoch != epoch_ || session_seq == 0U ||
        start_ts == 0U) {
      return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
    }

    std::lock_guard state_lock(state_mutex_);
    const auto session_it = sessions_.find(session);
    const auto logical_it = logical_transactions_.find(transaction);
    if (session_it == sessions_.end() ||
        session_seq > session_it->second.last_session_seq ||
        logical_it == logical_transactions_.end() ||
        logical_it->second.session_id != session ||
        logical_it->second.session_seq != session_seq ||
        active_by_transaction_.contains(transaction)) {
      return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
    }
    if (next_attempt_counter_ == std::numeric_limits<std::uint64_t>::max()) {
      return fail_resource("native attempt identity space is exhausted");
    }
    ++next_attempt_counter_;
    const EntityUid attempt{
        {mix64(epoch_.words[0] ^ 0xa77e6d5bULL ^ next_attempt_counter_),
         mix64(epoch_.words[1] ^ next_attempt_counter_)}};
    if (ordering_protocol_ ==
            NativeOrderingProtocol::FrontierReorder &&
        mode_ != NativeMode::CaptureOnly) {
      const auto registered = runtime_.registerActiveStart(
          attempt, session.producer_id,
          CommitOrderToken{cluster_epoch_, start_ts, std::nullopt});
      if (!registered.ok()) {
        return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
      }
    }
    attempts_.try_emplace(
        attempt,
        AttemptState{.attempt_id     = attempt,
                     .transaction_id = transaction,
                     .session_id     = session,
                     .session_seq    = session_seq,
                     .start_ts       = start_ts,
                     .phase          = AttemptState::Phase::Registered,
                     .prepared       = nullptr});
    active_by_transaction_.try_emplace(transaction, attempt);
    if (ordering_protocol_ ==
        NativeOrderingProtocol::FrontierReorder) {
      update_highest(start_ts);
    }
    attempt_out = attempt;
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  }

  [[nodiscard]] co_checker_online_status_v1
  abort_attempt(const EntityUid &attempt) {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (const auto status = available_status();
        status != CO_CHECKER_ONLINE_STATUS_OK_V1) {
      return status;
    }

    std::unique_ptr<PreparedEvidence> prepared;
    EntityUid transaction_id;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto attempt_it = attempts_.find(attempt);
      if (attempt_it == attempts_.end()) {
        return fail_lifecycle(
            "abort did not match a registered native attempt");
      }
      const bool serialized_abort =
          ordering_protocol_ ==
              NativeOrderingProtocol::SerializedGlobalLock &&
          (attempt_it->second.phase == AttemptState::Phase::Registered ||
           attempt_it->second.phase == AttemptState::Phase::Prepared);
      const bool frontier_abort =
          ordering_protocol_ == NativeOrderingProtocol::FrontierReorder &&
          attempt_it->second.phase == AttemptState::Phase::Registered;
      if (!serialized_abort && !frontier_abort) {
        return fail_lifecycle(
            "abort did not match an abortable native attempt");
      }
      transaction_id = attempt_it->second.transaction_id;
      prepared       = std::move(attempt_it->second.prepared);
      active_by_transaction_.erase(transaction_id);
      attempts_.erase(attempt_it);
    }

    if (ordering_protocol_ == NativeOrderingProtocol::FrontierReorder &&
        mode_ != NativeMode::CaptureOnly) {
      const auto aborted = runtime_.abortActiveStart(attempt);
      if (!aborted.ok()) {
        return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
      }
    }
    definitive_aborts_.fetch_add(1U, std::memory_order_relaxed);
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  }

  [[nodiscard]] co_checker_online_status_v1
  prepare(const EntityUid &attempt, const void *flat_input,
          const std::uint64_t flat_input_size) {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (ordering_protocol_ !=
        NativeOrderingProtocol::SerializedGlobalLock) {
      return fail_lifecycle(
          "two-stage prepare is only valid for serialized-global-lock");
    }
    if (const auto status = available_status();
        status != CO_CHECKER_ONLINE_STATUS_OK_V1) {
      return status;
    }

    const auto parse_started = std::chrono::steady_clock::now();
    const auto parsed = parse_flat_input(
        flat_input, flat_input_size, chunk_pool_->ordinary_segment_size(),
        FlatInputPurpose::SerializedPrepared);
    native_parse_ns_.fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - parse_started)
                .count()),
        std::memory_order_relaxed);
    if (!parsed.input.has_value()) {
      return fail_malformed(parsed.error);
    }

    AttemptIdentity attempt_state;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto attempt_it = attempts_.find(attempt);
      if (attempt_it == attempts_.end() ||
          attempt_it->second.phase != AttemptState::Phase::Registered) {
        return fail_lifecycle(
            "prepare did not match a registered native attempt");
      }
      if (parsed.input->header.start_ts != attempt_it->second.start_ts) {
        return fail_malformed(
            "prepared start timestamp disagrees with registration");
      }
      attempt_it->second.phase = AttemptState::Phase::Preparing;
      attempt_state = AttemptIdentity{
          .attempt_id     = attempt_it->second.attempt_id,
          .transaction_id = attempt_it->second.transaction_id,
          .session_id     = attempt_it->second.session_id,
          .session_seq    = attempt_it->second.session_seq,
          .start_ts       = attempt_it->second.start_ts,
      };
    }

    OrderedTxnMeta metadata;
    metadata.commit_seq     = kUnassignedCommitSeq;
    metadata.transaction_id = attempt_state.transaction_id;
    metadata.attempt_id     = attempt_state.attempt_id;
    metadata.session_id     = attempt_state.session_id;
    metadata.session_seq    = attempt_state.session_seq;
    metadata.commit_token =
        CommitOrderToken{cluster_epoch_, 0, std::nullopt};
    metadata.order_token_kind =
        parsed.input->header.read_only != 0U
            ? OrderTokenKind::ReadOnlyCompletion
            : OrderTokenKind::WriteCommit;
    metadata.publication_seq = kUnassignedPublicationSeq;
    metadata.producer_id     = attempt_state.session_id.producer_id;
    metadata.producer_seq    = 0;

    PackedBatchEnvelope envelope;
    if (const auto built = build_batch(*parsed.input, metadata, envelope);
        built != CO_CHECKER_ONLINE_STATUS_OK_V1) {
      return built;
    }

    auto evidence = std::make_unique<PreparedEvidence>();
    evidence->normalized_write_count = envelope.view().write_count();
    evidence->event_count             = envelope.view().event_count();
    evidence->declared_read_only =
        parsed.input->header.read_only != 0U;
    if (mode_ == NativeMode::CaptureOnly) {
      evidence->capture_only_batch = std::move(envelope);
    } else {
      const auto handoff_started = std::chrono::steady_clock::now();
      auto prepared = runtime_.prepareCommitted(std::move(envelope));
      native_handoff_ns_.fetch_add(
          static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - handoff_started)
                  .count()),
          std::memory_order_relaxed);
      if (!prepared.ok()) {
        return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
      }
      evidence->runtime_batch = std::move(prepared.prepared);
    }

    {
      std::lock_guard state_lock(state_mutex_);
      const auto attempt_it = attempts_.find(attempt);
      if (attempt_it == attempts_.end() ||
          attempt_it->second.phase != AttemptState::Phase::Preparing ||
          attempt_it->second.prepared != nullptr) {
        return fail_lifecycle(
            "prepared ownership no longer matches its native attempt");
      }
      attempt_it->second.prepared = std::move(evidence);
      attempt_it->second.phase    = AttemptState::Phase::Prepared;
    }
    prepared_transactions_.fetch_add(1U, std::memory_order_relaxed);
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  }

  [[nodiscard]] co_checker_online_status_v1
  finalize(const EntityUid &attempt,
           const co_checker_online_order_token_kind_v1 c_token_kind,
           const std::uint64_t order_token,
           const PublicationSeq publication_seq) {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (ordering_protocol_ !=
        NativeOrderingProtocol::SerializedGlobalLock) {
      return fail_lifecycle(
          "two-stage finalize is only valid for serialized-global-lock");
    }
    if (const auto status = available_status();
        status != CO_CHECKER_ONLINE_STATUS_OK_V1) {
      return status;
    }
    const auto token_kind = to_order_token_kind(c_token_kind);
    if (!token_kind.has_value() ||
        *token_kind == OrderTokenKind::ReadOnlyStart ||
        order_token == 0 || publication_seq == kUnassignedPublicationSeq) {
      return fail_lifecycle("serialized finalization metadata is invalid");
    }

    std::lock_guard publication_lock(serialized_publication_mutex_);
    if (publication_sequence_exhausted_ ||
        publication_seq != next_publication_seq_) {
      return fail_lifecycle(
          "serialized PublicationSeq is not the next accepted value");
    }
    const CommitOrderToken token{
        cluster_epoch_, order_token, std::nullopt};
    if (last_serialized_token_.has_value() &&
        compareCommitOrderTokens(*last_serialized_token_, token) !=
            TokenRelation::Less) {
      return fail_lifecycle(
          "serialized finalization token did not increase strictly");
    }

    AttemptIdentity attempt_state;
    std::unique_ptr<PreparedEvidence> evidence;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto attempt_it = attempts_.find(attempt);
      if (attempt_it == attempts_.end() ||
          attempt_it->second.phase != AttemptState::Phase::Prepared ||
          attempt_it->second.prepared == nullptr) {
        return fail_lifecycle(
            "finalize did not match a prepared native attempt");
      }
      attempt_state = AttemptIdentity{
          .attempt_id     = attempt_it->second.attempt_id,
          .transaction_id = attempt_it->second.transaction_id,
          .session_id     = attempt_it->second.session_id,
          .session_seq    = attempt_it->second.session_seq,
          .start_ts       = attempt_it->second.start_ts,
      };
      evidence = std::move(attempt_it->second.prepared);
      attempt_it->second.phase = AttemptState::Phase::Publishing;
    }

    const bool is_write = *token_kind == OrderTokenKind::WriteCommit;
    if ((!is_write && evidence->normalized_write_count != 0U) ||
        (*token_kind == OrderTokenKind::ReadOnlyCompletion &&
         !evidence->declared_read_only) ||
        (is_write && evidence->declared_read_only)) {
      return fail_lifecycle(
          "serialized token kind disagrees with prepared final writes");
    }

    OrderedTxnMeta metadata;
    metadata.commit_seq       = kUnassignedCommitSeq;
    metadata.transaction_id   = attempt_state.transaction_id;
    metadata.attempt_id       = attempt_state.attempt_id;
    metadata.session_id       = attempt_state.session_id;
    metadata.session_seq      = attempt_state.session_seq;
    metadata.commit_token     = token;
    metadata.order_token_kind = *token_kind;
    metadata.publication_seq  = publication_seq;
    metadata.producer_id      = attempt_state.session_id.producer_id;
    metadata.producer_seq     = publication_seq;

    if (mode_ == NativeMode::CaptureOnly) {
      if (!evidence->capture_only_batch.valid()) {
        return fail_lifecycle(
            "capture-only finalization lost prepared batch ownership");
      }
      recycled_transactions_.fetch_add(1U, std::memory_order_relaxed);
    } else {
      const auto handoff_started = std::chrono::steady_clock::now();
      auto submitted = runtime_.submitPrepared(
          std::move(evidence->runtime_batch), metadata);
      native_handoff_ns_.fetch_add(
          static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - handoff_started)
                  .count()),
          std::memory_order_relaxed);
      if (!submitted.ok()) {
        committed_but_unpublished_.fetch_add(
            is_write ? 1U : 0U, std::memory_order_relaxed);
        return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
      }
    }

    last_serialized_token_ = token;
    if (next_publication_seq_ ==
        std::numeric_limits<PublicationSeq>::max()) {
      publication_sequence_exhausted_ = true;
    } else {
      ++next_publication_seq_;
    }
    accepted_publications_.fetch_add(1U, std::memory_order_relaxed);
    accepted_events_.fetch_add(evidence->event_count,
                               std::memory_order_relaxed);
    if (is_write) {
      successful_write_commits_.fetch_add(1U, std::memory_order_relaxed);
    } else {
      completion_transactions_.fetch_add(1U, std::memory_order_relaxed);
    }
    update_highest(order_token);
    {
      std::lock_guard state_lock(state_mutex_);
      active_by_transaction_.erase(attempt_state.transaction_id);
      attempts_.erase(attempt_state.attempt_id);
      logical_transactions_.erase(attempt_state.transaction_id);
    }
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  }

  [[nodiscard]] co_checker_online_status_v1
  publish(const EntityUid &attempt, const void *flat_input,
          const std::uint64_t flat_input_size) {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (ordering_protocol_ != NativeOrderingProtocol::FrontierReorder) {
      return fail_lifecycle(
          "one-stage publish is only valid for frontier-reorder");
    }
    if (const auto status = available_status();
        status != CO_CHECKER_ONLINE_STATUS_OK_V1) {
      return status;
    }
    const auto parse_started = std::chrono::steady_clock::now();
    const auto parsed = parse_flat_input(
        flat_input, flat_input_size, chunk_pool_->ordinary_segment_size(),
        FlatInputPurpose::FrontierCommitted);
    native_parse_ns_.fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - parse_started)
                .count()),
        std::memory_order_relaxed);
    if (!parsed.input.has_value()) {
      return fail_malformed(parsed.error);
    }

    AttemptIdentity attempt_state;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto attempt_it = attempts_.find(attempt);
      if (attempt_it == attempts_.end() ||
          attempt_it->second.phase != AttemptState::Phase::Registered) {
        return fail_lifecycle(
            "publish did not match a registered native attempt");
      }
      if (parsed.input->header.start_ts != attempt_it->second.start_ts) {
        return fail_malformed(
            "published start timestamp disagrees with registration");
      }
      attempt_it->second.phase = AttemptState::Phase::Publishing;
      attempt_state = AttemptIdentity{
          .attempt_id     = attempt_it->second.attempt_id,
          .transaction_id = attempt_it->second.transaction_id,
          .session_id     = attempt_it->second.session_id,
          .session_seq    = attempt_it->second.session_seq,
          .start_ts       = attempt_it->second.start_ts,
      };
    }

    auto *producer =
        producer_state_.at(attempt_state.session_id.producer_id).get();
    const auto producer_wait_started = std::chrono::steady_clock::now();
    std::lock_guard producer_lock(producer->mutex);
    producer_mutex_wait_ns_.fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - producer_wait_started)
                .count()),
        std::memory_order_relaxed);
    if (producer->last_sequence == std::numeric_limits<ProducerSeq>::max()) {
      return fail_resource("native producer sequence is exhausted");
    }
    const auto producer_sequence = producer->last_sequence + ProducerSeq{1};
    const auto commit_timestamp  = parsed.input->header.read_only != 0U
                                       ? parsed.input->header.start_ts
                                       : parsed.input->header.commit_ts;

    OrderedTxnMeta metadata;
    metadata.commit_seq     = kUnassignedCommitSeq;
    metadata.transaction_id = attempt_state.transaction_id;
    metadata.attempt_id     = attempt_state.attempt_id;
    metadata.session_id     = attempt_state.session_id;
    metadata.session_seq    = attempt_state.session_seq;
    metadata.commit_token =
        CommitOrderToken{cluster_epoch_, commit_timestamp, std::nullopt};
    metadata.producer_id  = attempt_state.session_id.producer_id;
    metadata.producer_seq = producer_sequence;

    PackedBatchEnvelope envelope;
    if (const auto built = build_batch(*parsed.input, metadata, envelope);
        built != CO_CHECKER_ONLINE_STATUS_OK_V1) {
      return built;
    }

    if (mode_ != NativeMode::CaptureOnly) {
      const auto handoff_started = std::chrono::steady_clock::now();
      auto submitted             = runtime_.submitCommitted(
          std::move(envelope), parsed.input->header.read_only != 0U);
      native_handoff_ns_.fetch_add(
          static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - handoff_started)
                  .count()),
          std::memory_order_relaxed);
      if (!submitted.ok()) {
        return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
      }
    }
    producer->last_sequence = producer_sequence;
    update_highest(commit_timestamp);
    {
      std::lock_guard state_lock(state_mutex_);
      active_by_transaction_.erase(attempt_state.transaction_id);
      attempts_.erase(attempt_state.attempt_id);
      logical_transactions_.erase(attempt_state.transaction_id);
    }
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  }

  [[nodiscard]] co_checker_online_status_v1
  database_frontier(const std::uint64_t timestamp) {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (ordering_protocol_ != NativeOrderingProtocol::FrontierReorder) {
      return fail_lifecycle(
          "database frontier is only valid for frontier-reorder");
    }
    if (const auto status = available_status();
        status != CO_CHECKER_ONLINE_STATUS_OK_V1) {
      return status;
    }
    if (timestamp == 0U) {
      return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
    }
    if (mode_ != NativeMode::CaptureOnly) {
      const auto result = runtime_.updateDatabaseFrontier(
          CommitOrderToken{cluster_epoch_, timestamp, std::nullopt});
      if (!result.ok()) {
        return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
      }
    }
    update_highest(timestamp);
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  }

  [[nodiscard]] co_checker_online_status_v1
  producer_frontier(const ProducerId producer_id,
                    const std::uint64_t timestamp) {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (ordering_protocol_ != NativeOrderingProtocol::FrontierReorder) {
      return fail_lifecycle(
          "producer frontier is only valid for frontier-reorder");
    }
    if (const auto status = available_status();
        status != CO_CHECKER_ONLINE_STATUS_OK_V1) {
      return status;
    }
    if (timestamp == 0U) {
      return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
    }
    const auto producer_it = producer_state_.find(producer_id);
    if (producer_it == producer_state_.end()) {
      return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
    }
    std::lock_guard producer_lock(producer_it->second->mutex);
    if (mode_ != NativeMode::CaptureOnly) {
      const auto result = runtime_.updateProducerDeliveryFrontier(
          producer_id,
          CommitOrderToken{cluster_epoch_, timestamp, std::nullopt},
          producer_it->second->last_sequence);
      if (!result.ok()) {
        return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
      }
    }
    update_highest(timestamp);
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  }

  [[nodiscard]] co_checker_online_status_v1 close() {
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    if (closed_) {
      return current_status();
    }
    closing_ = true;

    if (mode_ != NativeMode::CaptureOnly) {
      const auto flushed = runtime_.flush();
      if (!flushed.ok()) {
        (void)runtime_.shutdown();
        closed_ = true;
        (void)capture_budget_.close();
        return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
      }
    }

    {
      std::lock_guard state_lock(state_mutex_);
      if (!attempts_.empty()) {
        (void)set_failure(CO_CHECKER_ONLINE_ADAPTER_ERROR_LIFECYCLE_V1, 0,
                          "quiescent close found active transaction evidence",
                          RuntimeErrorCode::IncompletePrefix);
        (void)runtime_.shutdown();
        closed_ = true;
        (void)capture_budget_.close();
        return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
      }
    }

    if (mode_ != NativeMode::CaptureOnly &&
        !runtime_.quiescentBarrier().ok()) {
      (void)runtime_.shutdown();
      closed_ = true;
      (void)capture_budget_.close();
      return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
    }

    if (ordering_protocol_ ==
        NativeOrderingProtocol::SerializedGlobalLock) {
      const auto writes =
          successful_write_commits_.load(std::memory_order_relaxed);
      const auto completions =
          completion_transactions_.load(std::memory_order_relaxed);
      const auto accepted =
          accepted_publications_.load(std::memory_order_relaxed);
      const auto accepted_events =
          accepted_events_.load(std::memory_order_relaxed);
      const auto recycled =
          recycled_transactions_.load(std::memory_order_relaxed);
      bool lifecycle_mismatch = writes + completions != accepted;
      if (mode_ == NativeMode::CaptureOnly) {
        lifecycle_mismatch =
            lifecycle_mismatch || accepted != recycled;
      } else {
        const auto runtime_metrics = runtime_.metrics();
        lifecycle_mismatch =
            lifecycle_mismatch ||
            accepted != runtime_metrics.published_transactions ||
            accepted != runtime_metrics.ordered_transactions ||
            (mode_ == NativeMode::Full &&
             (accepted != runtime_metrics.checked_transactions ||
              accepted_events != runtime_metrics.checked_events));
      }
      const auto capture_metrics = capture_budget_.metrics();
      lifecycle_mismatch =
          lifecycle_mismatch || capture_metrics.reserved_bytes != 0;
      if (lifecycle_mismatch) {
        (void)set_failure(
            CO_CHECKER_ONLINE_ADAPTER_ERROR_LIFECYCLE_V1, 0,
            "serialized lifecycle counters do not agree at close",
            RuntimeErrorCode::IncompletePrefix);
        (void)runtime_.shutdown();
        closed_ = true;
        (void)capture_budget_.close();
        return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
      }
    }
    const auto shutdown = runtime_.shutdown();
    closed_             = true;
    (void)capture_budget_.close();
    return shutdown.ok() ? CO_CHECKER_ONLINE_STATUS_OK_V1
                         : CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
  }

  [[nodiscard]] co_checker_online_status_v1
  external_fatal(const std::uint32_t external_code, std::string message) {
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    if (closed_ || closing_) {
      return CO_CHECKER_ONLINE_STATUS_STOPPED_V1;
    }
    return set_failure(CO_CHECKER_ONLINE_ADAPTER_ERROR_EXTERNAL_V1,
                       external_code, std::move(message),
                       RuntimeErrorCode::ProtocolFailure);
  }

  [[nodiscard]] co_checker_online_status_v1
  resource_fatal(std::string message) {
    return set_failure(CO_CHECKER_ONLINE_ADAPTER_ERROR_RESOURCE_V1, 0,
                       std::move(message), RuntimeErrorCode::ResourceExhausted);
  }

  [[nodiscard]] co_checker_online_status_v1
  unexpected_fatal(std::string message) {
    return set_failure(CO_CHECKER_ONLINE_ADAPTER_ERROR_LIFECYCLE_V1, 0,
                       std::move(message), RuntimeErrorCode::ProtocolFailure);
  }

  [[nodiscard]] std::optional<AdapterFailure> failure() const {
    std::lock_guard lock(failure_mutex_);
    return failure_;
  }

  [[nodiscard]] std::optional<RuntimeError> runtime_error() const {
    return runtime_.error();
  }

  [[nodiscard]] std::optional<ConsistencyVerdict> verdict() const {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (failure().has_value() || mode_ != NativeMode::Full) {
      return std::nullopt;
    }
    return runtime_.verdict();
  }

  [[nodiscard]] std::string metrics_json() const {
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    const auto runtime_metrics  = runtime_.metrics();
    const auto capture_metrics  = capture_budget_.metrics();
    const auto pool_metrics     = chunk_pool_->stats();
    const auto slowest_check =
        runtime_metrics.slowest_check.value_or(SlowestCheckMetrics{});
    std::size_t active_attempts = 0;
    {
      std::lock_guard state_lock(state_mutex_);
      active_attempts = attempts_.size();
    }
    std::ostringstream out;
    out << '{' << "\"mode\":" << static_cast<unsigned>(mode_) + 1U << ','
        << "\"ordering_protocol\":"
        << static_cast<unsigned>(ordering_protocol_) + 1U << ','
        << "\"closed\":" << (closed_ ? "true" : "false") << ',' << "\"fatal\":"
        << (failure().has_value() || runtime_.error().has_value() ? "true"
                                                                  : "false")
        << ",\"active_attempts\":" << active_attempts
        << ",\"prepared_transactions\":"
        << prepared_transactions_.load(std::memory_order_relaxed)
        << ",\"definitive_aborts\":"
        << definitive_aborts_.load(std::memory_order_relaxed)
        << ",\"successful_write_commits\":"
        << successful_write_commits_.load(std::memory_order_relaxed)
        << ",\"completion_transactions\":"
        << completion_transactions_.load(std::memory_order_relaxed)
        << ",\"accepted_publications\":"
        << accepted_publications_.load(std::memory_order_relaxed)
        << ",\"accepted_events\":"
        << accepted_events_.load(std::memory_order_relaxed)
        << ",\"recycled_transactions\":"
        << recycled_transactions_.load(std::memory_order_relaxed)
        << ",\"committed_but_unpublished\":"
        << committed_but_unpublished_.load(std::memory_order_relaxed)
        << ",\"captured_transactions\":"
        << captured_transactions_.load(std::memory_order_relaxed)
        << ",\"captured_events\":"
        << captured_events_.load(std::memory_order_relaxed)
        << ",\"captured_used_bytes\":"
        << captured_used_bytes_.load(std::memory_order_relaxed)
        << ",\"captured_reserved_bytes\":"
        << captured_reserved_bytes_.load(std::memory_order_relaxed)
        << ",\"native_parse_ns\":"
        << native_parse_ns_.load(std::memory_order_relaxed)
        << ",\"native_reserve_ns\":"
        << native_reserve_ns_.load(std::memory_order_relaxed)
        << ",\"producer_mutex_wait_ns\":"
        << producer_mutex_wait_ns_.load(std::memory_order_relaxed)
        << ",\"native_build_ns\":"
        << native_build_ns_.load(std::memory_order_relaxed)
        << ",\"native_handoff_ns\":"
        << native_handoff_ns_.load(std::memory_order_relaxed)
        << ",\"capture_capacity_bytes\":" << capture_metrics.capacity_bytes
        << ",\"capture_active_bytes\":" << capture_metrics.reserved_bytes
        << ",\"capture_peak_bytes\":" << capture_metrics.peak_reserved_bytes
        << ",\"capture_backpressure_wait_count\":"
        << capture_metrics.backpressure_wait_count
        << ",\"capture_backpressure_ns\":"
        << capture_metrics.total_backpressure_time.count()
        << ",\"chunk_acquisitions\":" << pool_metrics.acquisitions
        << ",\"chunk_allocations\":" << pool_metrics.allocations
        << ",\"chunk_reuses\":" << pool_metrics.reuses
        << ",\"chunk_recycles\":" << pool_metrics.recycles
        << ",\"chunk_cached_segments\":"
        << pool_metrics.cached_ordinary_segments
        << ",\"queue_capacity_bytes\":" << runtime_metrics.queue.capacity_bytes
        << ",\"queue_reserved_bytes\":"
        << runtime_metrics.queue.reserved_bytes
        << ",\"queue_reserved_depth\":"
        << runtime_metrics.queue.reserved_depth
        << ",\"queue_bytes\":" << runtime_metrics.queue.queued_bytes
        << ",\"queue_peak_bytes\":"
        << runtime_metrics.queue.high_water_queued_bytes
        << ",\"queue_peak_depth\":"
        << runtime_metrics.queue.high_water_queued_depth
        << ",\"queue_depth\":" << runtime_metrics.queue.queued_depth
        << ",\"queue_backpressure_wait_count\":"
        << runtime_metrics.queue.backpressure_wait_count
        << ",\"queue_backpressure_ns\":"
        << runtime_metrics.queue.total_backpressure_time.count()
        << ",\"reorder_capacity_bytes\":"
        << runtime_metrics.reorder_capacity_bytes
        << ",\"reorder_bytes\":" << runtime_metrics.gate.pending_reserved_bytes
        << ",\"reorder_peak_bytes\":"
        << runtime_metrics.gate.peak_pending_reserved_bytes
        << ",\"ordered_transactions\":" << runtime_metrics.ordered_transactions
        << ",\"checked_transactions\":" << runtime_metrics.checked_transactions
        << ",\"checked_events\":" << runtime_metrics.checked_events
        << ",\"violating_transactions\":"
        << runtime_metrics.violating_transactions
        << ",\"max_queue_lag_ns\":" << runtime_metrics.max_queue_lag.count()
        << ",\"ordering_ns\":" << runtime_metrics.total_ordering_time.count()
        << ",\"checking_ns\":" << runtime_metrics.total_checking_time.count()
        << ",\"max_checking_ns\":" << runtime_metrics.max_checking_time.count()
        << ",\"detailed_profiling_enabled\":"
        << (runtime_metrics.engine.repeated_reads.detailed_profiling_enabled
                ? "true"
                : "false")
        << ",\"checking_duration_samples\":"
        << runtime_metrics.checking_duration_samples
        << ",\"checking_p50_bucket_upper_bound_ns\":"
        << runtime_metrics.checking_p50_bucket_upper_bound.count()
        << ",\"checking_p95_bucket_upper_bound_ns\":"
        << runtime_metrics.checking_p95_bucket_upper_bound.count()
        << ",\"checking_p99_bucket_upper_bound_ns\":"
        << runtime_metrics.checking_p99_bucket_upper_bound.count()
        << ",\"slowest_check_commit_seq\":" << slowest_check.commit_seq
        << ",\"slowest_check_transaction_uid_word0\":"
        << slowest_check.transaction_id.words[0]
        << ",\"slowest_check_transaction_uid_word1\":"
        << slowest_check.transaction_id.words[1]
        << ",\"slowest_check_event_count\":" << slowest_check.event_count
        << ",\"total_queue_lag_ns\":"
        << runtime_metrics.total_queue_lag.count()
        << ",\"current_commit_token_lag\":"
        << runtime_metrics.current_commit_token_lag
        << ",\"max_commit_token_lag\":" << runtime_metrics.max_commit_token_lag
        << ",\"retained_state_capacity_bytes\":"
        << runtime_metrics.engine.retained_state_capacity_bytes
        << ",\"retained_state_bytes\":"
        << runtime_metrics.engine.retained_state_bytes
        << ",\"peak_retained_state_bytes\":"
        << runtime_metrics.engine.peak_retained_state_bytes
        << ",\"interned_key_bytes\":"
        << runtime_metrics.engine.interned_key_bytes
        << ",\"interned_value_bytes\":"
        << runtime_metrics.engine.interned_value_bytes
        << ",\"history_entries\":" << runtime_metrics.engine.history_entries
        << ",\"history_deltas\":" << runtime_metrics.engine.history_deltas
        << ",\"snapshot_read_events\":"
        << runtime_metrics.engine.snapshot_read_events
        << ",\"unique_snapshot_reads\":"
        << runtime_metrics.engine.unique_snapshot_reads
        << ",\"identical_snapshot_repeats\":"
        << runtime_metrics.engine.identical_snapshot_repeats
        << ",\"conflicting_snapshot_repeats\":"
        << runtime_metrics.engine.conflicting_snapshot_repeats
        << ",\"snapshot_read_packed_bytes\":"
        << runtime_metrics.engine.snapshot_read_packed_bytes
        << ",\"duplicate_snapshot_read_packed_bytes\":"
        << runtime_metrics.engine.duplicate_snapshot_read_packed_bytes
        << ",\"identical_snapshot_repeat_packed_bytes\":"
        << runtime_metrics.engine.identical_snapshot_repeat_packed_bytes
        << ",\"conflicting_snapshot_repeat_packed_bytes\":"
        << runtime_metrics.engine.conflicting_snapshot_repeat_packed_bytes
        << ",\"compatible_snapshot_checks\":"
        << runtime_metrics.engine.compatible_snapshot_checks
        << ",\"history_entries_visited\":"
        << runtime_metrics.engine.history_entries_visited
        << ",\"history_deltas_visited\":"
        << runtime_metrics.engine.history_deltas_visited
        << ",\"history_relevant_deltas_visited\":"
        << runtime_metrics.engine.history_relevant_deltas_visited
        << ",\"max_history_entries_visited\":"
        << runtime_metrics.engine.max_history_entries_visited
        << ",\"max_history_deltas_visited\":"
        << runtime_metrics.engine.max_history_deltas_visited
        << ",\"repeated_read_table_size\":"
        << runtime_metrics.engine.repeated_reads.size
        << ",\"repeated_read_table_capacity\":"
        << runtime_metrics.engine.repeated_reads.capacity
        << ",\"repeated_read_touched_size\":"
        << runtime_metrics.engine.repeated_reads.touched_size
        << ",\"repeated_read_touched_capacity\":"
        << runtime_metrics.engine.repeated_reads.touched_capacity
        << ",\"repeated_read_probe_groups\":"
        << runtime_metrics.engine.repeated_reads.probe_groups
        << ",\"repeated_read_key_comparisons\":"
        << runtime_metrics.engine.repeated_reads.key_comparisons
        << ",\"repeated_read_resizes\":"
        << runtime_metrics.engine.repeated_reads.resizes
        << ",\"repeated_read_shrinks\":"
        << runtime_metrics.engine.repeated_reads.shrinks
        << ",\"repeated_read_shrink_attempts\":"
        << runtime_metrics.engine.repeated_reads.shrink_attempts
        << ",\"repeated_read_shrink_failures\":"
        << runtime_metrics.engine.repeated_reads.shrink_failures
        << ",\"repeated_read_max_shrink_rebuild_ns\":"
        << runtime_metrics.engine.repeated_reads.max_shrink_rebuild_ns
        << ",\"repeated_read_reset_calls\":"
        << runtime_metrics.engine.repeated_reads.reset_calls
        << ",\"repeated_read_reset_expected_events\":"
        << runtime_metrics.engine.repeated_reads.reset_expected_events
        << ",\"repeated_read_max_reset_expected_events\":"
        << runtime_metrics.engine.repeated_reads.max_reset_expected_events
        << ",\"repeated_read_clear_calls\":"
        << runtime_metrics.engine.repeated_reads.clear_calls
        << ",\"repeated_read_clear_slot_visits\":"
        << runtime_metrics.engine.repeated_reads.clear_slot_visits
        << ",\"repeated_read_clear_control_bytes_filled\":"
        << runtime_metrics.engine.repeated_reads.clear_control_bytes_filled
        << ",\"repeated_read_clear_occupied_slots_reset\":"
        << runtime_metrics.engine.repeated_reads.clear_occupied_slots_reset
        << ",\"repeated_read_probe_mode\":"
        << static_cast<unsigned>(runtime_metrics.engine.repeated_reads.mode)
        << '}';
    return out.str();
  }

private:
  [[nodiscard]] co_checker_online_status_v1
  build_batch(const FlatInputView &input, const OrderedTxnMeta &metadata,
              PackedBatchEnvelope &output) {
    const auto reserve_started = std::chrono::steady_clock::now();
    auto reservation =
        capture_budget_.reserve(input.estimated_reserved_bytes);
    native_reserve_ns_.fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - reserve_started)
                .count()),
        std::memory_order_relaxed);
    if (!reservation.ok()) {
      return fail_resource(reservation.error.has_value()
                               ? reservation.error->message
                               : "native capture reservation failed");
    }

    const auto build_started = std::chrono::steady_clock::now();
    ChunkBatchBuilder builder(chunk_pool_, metadata);
    for (std::size_t index = 0; index < input.header.event_count; ++index) {
      const auto event = input.event(index);
      const auto key   = input.blob(event.key_offset, event.key_size);
      const TaggedBytes tagged =
          event.value_state == CO_CHECKER_ONLINE_VALUE_PRESENT_V1
              ? TaggedBytes{
                    PresentBytes{input.blob(event.value_offset,
                                            event.value_size)}}
              : TaggedBytes{Absent{}};
      BatchBuildStatus built;
      if (event.operation == CO_CHECKER_ONLINE_EVENT_READ_V1) {
        built = builder.append_read(
            event.sequence, key, tagged,
            event.read_source == CO_CHECKER_ONLINE_READ_SNAPSHOT_V1
                ? ReadSource::Snapshot
                : ReadSource::Local);
      } else {
        built = builder.append_write(event.sequence, key, tagged);
      }
      if (!built.ok()) {
        return fail_malformed("native chunk builder rejected a flat event");
      }
    }

    auto sealed = builder.seal();
    if (!sealed.ok()) {
      return fail_malformed("native chunk builder could not seal its batch");
    }
    if (sealed.batch->reserved_bytes() != input.estimated_reserved_bytes) {
      return fail_resource(
          "native capture reservation disagrees with sealed batch bytes");
    }
    native_build_ns_.fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - build_started)
                .count()),
        std::memory_order_relaxed);

    output = PackedBatchEnvelope(std::move(sealed.batch),
                                 std::move(reservation.lease));
    captured_transactions_.fetch_add(1U, std::memory_order_relaxed);
    captured_events_.fetch_add(input.header.event_count,
                               std::memory_order_relaxed);
    captured_used_bytes_.fetch_add(output.used_bytes(),
                                   std::memory_order_relaxed);
    captured_reserved_bytes_.fetch_add(output.reserved_bytes(),
                                       std::memory_order_relaxed);
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  }

  [[nodiscard]] static RuntimeOptions
  runtime_options(RuntimeOptions options, const RuntimeEpoch &epoch,
                  const std::uint64_t cluster_epoch) noexcept {
    options.expected_runtime_epoch = epoch;
    options.expected_cluster_epoch = cluster_epoch;
    return options;
  }

  void update_highest(const std::uint64_t timestamp) noexcept {
    auto current = highest_observed_timestamp_.load(std::memory_order_relaxed);
    while (current < timestamp &&
           !highest_observed_timestamp_.compare_exchange_weak(
               current, timestamp, std::memory_order_release,
               std::memory_order_relaxed)) {
    }
  }

  [[nodiscard]] co_checker_online_status_v1 available_status() const {
    if (closed_ || closing_) {
      return CO_CHECKER_ONLINE_STATUS_STOPPED_V1;
    }
    return current_status();
  }

  [[nodiscard]] co_checker_online_status_v1 current_status() const {
    if (failure().has_value() || runtime_.error().has_value()) {
      return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
    }
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  }

  [[nodiscard]] co_checker_online_status_v1
  fail_malformed(const std::string_view message) {
    return set_failure(CO_CHECKER_ONLINE_ADAPTER_ERROR_MALFORMED_INPUT_V1, 0,
                       std::string(message), RuntimeErrorCode::ProtocolFailure);
  }

  [[nodiscard]] co_checker_online_status_v1
  fail_lifecycle(const std::string_view message) {
    return set_failure(CO_CHECKER_ONLINE_ADAPTER_ERROR_LIFECYCLE_V1, 0,
                       std::string(message), RuntimeErrorCode::ProtocolFailure);
  }

  [[nodiscard]] co_checker_online_status_v1
  fail_resource(const std::string_view message) {
    return set_failure(CO_CHECKER_ONLINE_ADAPTER_ERROR_RESOURCE_V1, 0,
                       std::string(message),
                       RuntimeErrorCode::ResourceExhausted);
  }

  [[nodiscard]] co_checker_online_status_v1
  set_failure(const co_checker_online_adapter_error_v1 code,
              const std::uint32_t external_code, std::string message,
              const RuntimeErrorCode runtime_code) {
    {
      std::lock_guard failure_lock(failure_mutex_);
      if (!failure_.has_value()) {
        failure_ = AdapterFailure{code, external_code, message};
      }
    }
    (void)runtime_.fail(make_runtime_error(runtime_code, std::move(message)));
    (void)capture_budget_.close();
    return code == CO_CHECKER_ONLINE_ADAPTER_ERROR_RESOURCE_V1
               ? CO_CHECKER_ONLINE_STATUS_RESOURCE_ERROR_V1
               : CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
  }

  RuntimeEpoch epoch_;
  std::uint64_t cluster_epoch_ = 0;
  NativeMode mode_             = NativeMode::Full;
  NativeOrderingProtocol ordering_protocol_ =
      NativeOrderingProtocol::SerializedGlobalLock;
  std::set<ProducerId> producers_;
  std::map<ProducerId, std::unique_ptr<ProducerState>> producer_state_;
  std::shared_ptr<ChunkPool> chunk_pool_;
  ByteBudget capture_budget_;
  OnlineRuntime runtime_;

  mutable std::shared_mutex lifecycle_mutex_;
  bool closing_ = false;
  bool closed_  = false;

  mutable std::mutex state_mutex_;
  std::uint64_t next_session_counter_ = 0;
  std::uint64_t next_logical_counter_ = 0;
  std::uint64_t next_attempt_counter_ = 0;
  std::map<SessionUid, SessionState, SessionLess> sessions_;
  std::map<EntityUid, LogicalTransactionState, EntityLess>
      logical_transactions_;
  std::map<EntityUid, AttemptState, EntityLess> attempts_;
  std::map<EntityUid, EntityUid, EntityLess> active_by_transaction_;

  mutable std::mutex failure_mutex_;
  std::optional<AdapterFailure> failure_;

  std::mutex serialized_publication_mutex_;
  PublicationSeq next_publication_seq_ = 1;
  bool publication_sequence_exhausted_ = false;
  std::optional<CommitOrderToken> last_serialized_token_;

  std::atomic<std::uint64_t> highest_observed_timestamp_{0};
  std::atomic<std::uint64_t> prepared_transactions_{0};
  std::atomic<std::uint64_t> definitive_aborts_{0};
  std::atomic<std::uint64_t> successful_write_commits_{0};
  std::atomic<std::uint64_t> completion_transactions_{0};
  std::atomic<std::uint64_t> accepted_events_{0};
  std::atomic<std::uint64_t> accepted_publications_{0};
  std::atomic<std::uint64_t> recycled_transactions_{0};
  std::atomic<std::uint64_t> committed_but_unpublished_{0};
  std::atomic<std::uint64_t> captured_transactions_{0};
  std::atomic<std::uint64_t> captured_events_{0};
  std::atomic<std::uint64_t> captured_used_bytes_{0};
  std::atomic<std::uint64_t> captured_reserved_bytes_{0};
  std::atomic<std::uint64_t> native_parse_ns_{0};
  std::atomic<std::uint64_t> native_reserve_ns_{0};
  std::atomic<std::uint64_t> producer_mutex_wait_ns_{0};
  std::atomic<std::uint64_t> native_build_ns_{0};
  std::atomic<std::uint64_t> native_handoff_ns_{0};
};

[[nodiscard]] co_checker_online_status_v1
copy_output_string(const std::string &source, char *destination,
                   const std::uint64_t capacity,
                   std::uint64_t *required_out) noexcept {
  if (required_out == nullptr ||
      source.size() == std::numeric_limits<std::size_t>::max()) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  const auto required = source.size() + 1U;
  *required_out       = static_cast<std::uint64_t>(required);
  if (destination == nullptr || capacity < required) {
    return CO_CHECKER_ONLINE_STATUS_BUFFER_TOO_SMALL_V1;
  }
  std::memcpy(destination, source.data(), source.size());
  destination[source.size()] = '\0';
  return CO_CHECKER_ONLINE_STATUS_OK_V1;
}

} // namespace

struct co_checker_online_runtime_v1 {
  std::unique_ptr<NativeRuntime> native;
};

extern "C" {

void co_checker_online_options_init_v1(co_checker_online_options_v1 *options) {
  if (options == nullptr) {
    return;
  }
  *options                               = co_checker_online_options_v1{};
  options->struct_size                   = sizeof(*options);
  options->abi_version                   = CO_CHECKER_ONLINE_ABI_VERSION_V1;
  options->queue_capacity_bytes          = 64U * 1024U * 1024U;
  options->reorder_capacity_bytes        = 0U;
  options->capture_capacity_bytes        = 64U * 1024U * 1024U;
  options->retained_state_capacity_bytes = 256U * 1024U * 1024U;
  options->queue_timeout_ns              = 30U * 1000U * 1000U * 1000U;
  options->capture_timeout_ns            = 30U * 1000U * 1000U * 1000U;
  options->chunk_segment_bytes           = 4U * 1024U;
  options->max_cached_chunk_segments     = 64U;
  options->cluster_epoch                 = 1U;
  options->mode                          = CO_CHECKER_ONLINE_MODE_FULL_V1;
  options->ordering_protocol =
      CO_CHECKER_ONLINE_ORDERING_SERIALIZED_GLOBAL_LOCK_V1;
}

void co_checker_online_txn_input_header_init_v1(
    co_checker_online_txn_input_header_v1 *header) {
  if (header == nullptr) {
    return;
  }
  *header               = co_checker_online_txn_input_header_v1{};
  header->struct_size   = sizeof(*header);
  header->abi_version   = CO_CHECKER_ONLINE_ABI_VERSION_V1;
  header->events_offset = sizeof(*header);
}

co_checker_online_status_v1 co_checker_online_runtime_create_v1(
    const co_checker_online_options_v1 *options,
    const std::uint32_t *required_producers, const std::uint32_t producer_count,
    co_checker_online_runtime_v1 **runtime_out) {
  if (runtime_out == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  *runtime_out      = nullptr;
  const auto parsed = parse_configuration(options);
  if (!parsed.configuration.has_value() || required_producers == nullptr ||
      producer_count == 0U ||
      (parsed.configuration.has_value() &&
       parsed.configuration->ordering_protocol ==
           NativeOrderingProtocol::SerializedGlobalLock &&
       producer_count != 1U)) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    std::vector<ProducerId> producers(required_producers,
                                      required_producers + producer_count);
    std::set<ProducerId> unique(producers.begin(), producers.end());
    if (unique.size() != producers.size()) {
      return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
    }
    auto runtime    = std::make_unique<co_checker_online_runtime_v1>();
    runtime->native = std::make_unique<NativeRuntime>(*parsed.configuration,
                                                      std::move(producers));
    *runtime_out    = runtime.release();
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  } catch (const std::bad_alloc &) {
    return CO_CHECKER_ONLINE_STATUS_RESOURCE_ERROR_V1;
  } catch (...) {
    return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
  }
}

void co_checker_online_runtime_destroy_v1(
    co_checker_online_runtime_v1 *runtime) {
  if (runtime == nullptr) {
    return;
  }
  try {
    (void)runtime->native->close();
  } catch (...) {
  }
  delete runtime;
}

co_checker_online_status_v1
co_checker_online_runtime_epoch_v1(const co_checker_online_runtime_v1 *runtime,
                                   co_checker_online_epoch_v1 *epoch_out) {
  if (runtime == nullptr || epoch_out == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  *epoch_out = to_c_epoch(runtime->native->epoch());
  return CO_CHECKER_ONLINE_STATUS_OK_V1;
}

co_checker_online_status_v1
co_checker_online_session_create_v1(co_checker_online_runtime_v1 *runtime,
                                    const std::uint32_t producer_id,
                                    co_checker_online_session_v1 *session_out) {
  if (runtime == nullptr || session_out == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    SessionUid session;
    const auto status = runtime->native->create_session(producer_id, session);
    if (status == CO_CHECKER_ONLINE_STATUS_OK_V1) {
      *session_out = to_c_session(session);
    }
    return status;
  } catch (const std::bad_alloc &) {
    return runtime->native->resource_fatal(
        "could not allocate native session state");
  } catch (...) {
    return runtime->native->unexpected_fatal(
        "unexpected native session creation failure");
  }
}

co_checker_online_status_v1 co_checker_online_logical_transaction_create_v1(
    co_checker_online_runtime_v1 *runtime,
    const co_checker_online_session_v1 *session,
    const std::uint64_t session_seq,
    co_checker_online_uid_v1 *transaction_out) {
  if (runtime == nullptr || session == nullptr || transaction_out == nullptr ||
      session->reserved0 != 0U) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    EntityUid transaction;
    const auto status = runtime->native->create_logical(
        to_session(*session), session_seq, transaction);
    if (status == CO_CHECKER_ONLINE_STATUS_OK_V1) {
      *transaction_out = to_c_uid(transaction);
    }
    return status;
  } catch (const std::bad_alloc &) {
    return runtime->native->resource_fatal(
        "could not allocate native logical transaction state");
  } catch (...) {
    return runtime->native->unexpected_fatal(
        "unexpected native logical transaction failure");
  }
}

co_checker_online_status_v1 co_checker_online_attempt_register_v1(
    co_checker_online_runtime_v1 *runtime,
    const co_checker_online_session_v1 *session,
    const co_checker_online_uid_v1 *transaction_id,
    const std::uint64_t session_seq, const std::uint64_t start_ts,
    co_checker_online_uid_v1 *attempt_out) {
  if (runtime == nullptr || session == nullptr || transaction_id == nullptr ||
      attempt_out == nullptr || session->reserved0 != 0U) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    EntityUid attempt;
    const auto status = runtime->native->register_attempt(
        to_session(*session), to_uid(*transaction_id), session_seq, start_ts,
        attempt);
    if (status == CO_CHECKER_ONLINE_STATUS_OK_V1) {
      *attempt_out = to_c_uid(attempt);
    }
    return status;
  } catch (const std::bad_alloc &) {
    return runtime->native->resource_fatal(
        "could not allocate native attempt state");
  } catch (...) {
    return runtime->native->unexpected_fatal(
        "unexpected native attempt registration failure");
  }
}

co_checker_online_status_v1
co_checker_online_attempt_abort_v1(co_checker_online_runtime_v1 *runtime,
                                   const co_checker_online_uid_v1 *attempt_id) {
  if (runtime == nullptr || attempt_id == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    return runtime->native->abort_attempt(to_uid(*attempt_id));
  } catch (const std::bad_alloc &) {
    return runtime->native->resource_fatal(
        "could not complete native attempt abort");
  } catch (...) {
    return runtime->native->unexpected_fatal(
        "unexpected native attempt abort failure");
  }
}

co_checker_online_status_v1
co_checker_online_attempt_prepare_v1(co_checker_online_runtime_v1 *runtime,
                                     const co_checker_online_uid_v1 *attempt_id,
                                     const void *flat_input,
                                     const std::uint64_t flat_input_size) {
  if (runtime == nullptr || attempt_id == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    return runtime->native->prepare(to_uid(*attempt_id), flat_input,
                                    flat_input_size);
  } catch (const std::bad_alloc &) {
    return runtime->native->resource_fatal(
        "could not allocate prepared native capture state");
  } catch (...) {
    return runtime->native->unexpected_fatal(
        "unexpected native prepare failure");
  }
}

co_checker_online_status_v1 co_checker_online_attempt_finalize_v1(
    co_checker_online_runtime_v1 *runtime,
    const co_checker_online_uid_v1 *attempt_id,
    const co_checker_online_order_token_kind_v1 token_kind,
    const std::uint64_t order_token,
    const std::uint64_t publication_seq) {
  if (runtime == nullptr || attempt_id == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    return runtime->native->finalize(to_uid(*attempt_id), token_kind,
                                     order_token, publication_seq);
  } catch (const std::bad_alloc &) {
    return runtime->native->resource_fatal(
        "serialized finalization unexpectedly allocated");
  } catch (...) {
    return runtime->native->unexpected_fatal(
        "unexpected native finalize failure");
  }
}

co_checker_online_status_v1
co_checker_online_attempt_publish_v1(co_checker_online_runtime_v1 *runtime,
                                     const co_checker_online_uid_v1 *attempt_id,
                                     const void *flat_input,
                                     const std::uint64_t flat_input_size) {
  if (runtime == nullptr || attempt_id == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    return runtime->native->publish(to_uid(*attempt_id), flat_input,
                                    flat_input_size);
  } catch (const std::bad_alloc &) {
    return runtime->native->resource_fatal(
        "could not allocate native capture state");
  } catch (...) {
    return runtime->native->unexpected_fatal(
        "unexpected native publish failure");
  }
}

co_checker_online_status_v1
co_checker_online_database_frontier_v1(co_checker_online_runtime_v1 *runtime,
                                       const std::uint64_t resolved_ts) {
  if (runtime == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    return runtime->native->database_frontier(resolved_ts);
  } catch (...) {
    return runtime->native->unexpected_fatal(
        "unexpected native database frontier failure");
  }
}

co_checker_online_status_v1
co_checker_online_producer_frontier_v1(co_checker_online_runtime_v1 *runtime,
                                       const std::uint32_t producer_id,
                                       const std::uint64_t resolved_ts) {
  if (runtime == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    return runtime->native->producer_frontier(producer_id, resolved_ts);
  } catch (...) {
    return runtime->native->unexpected_fatal(
        "unexpected native producer frontier failure");
  }
}

co_checker_online_status_v1
co_checker_online_quiescent_close_v1(co_checker_online_runtime_v1 *runtime) {
  if (runtime == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    return runtime->native->close();
  } catch (...) {
    return runtime->native->unexpected_fatal(
        "unexpected native quiescent close failure");
  }
}

co_checker_online_status_v1 co_checker_online_fatal_v1(
    co_checker_online_runtime_v1 *runtime, const std::uint32_t external_error,
    const char *message, const std::uint64_t message_size) {
  if (runtime == nullptr || (message == nullptr && message_size != 0U) ||
      message_size > std::numeric_limits<std::size_t>::max()) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    const std::string text =
        message_size == 0U
            ? std::string{"external producer failure"}
            : std::string{message, static_cast<std::size_t>(message_size)};
    return runtime->native->external_fatal(external_error, text);
  } catch (...) {
    return CO_CHECKER_ONLINE_STATUS_RESOURCE_ERROR_V1;
  }
}

co_checker_online_status_v1
co_checker_online_verdict_get_v1(const co_checker_online_runtime_v1 *runtime,
                                 co_checker_online_verdict_v1 *verdict_out) {
  if (runtime == nullptr || verdict_out == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    *verdict_out             = co_checker_online_verdict_v1{};
    verdict_out->struct_size = sizeof(*verdict_out);
    verdict_out->abi_version = CO_CHECKER_ONLINE_ABI_VERSION_V1;
    const auto verdict       = runtime->native->verdict();
    if (!verdict.has_value()) {
      verdict_out->status = CO_CHECKER_ONLINE_VERDICT_UNAVAILABLE_V1;
      return CO_CHECKER_ONLINE_STATUS_OK_V1;
    }
    if (verdict->status == ConsistencyStatus::Pass) {
      verdict_out->status = CO_CHECKER_ONLINE_VERDICT_PASS_V1;
      return CO_CHECKER_ONLINE_STATUS_OK_V1;
    }
    verdict_out->status = CO_CHECKER_ONLINE_VERDICT_VIOLATION_V1;
    if (verdict->first_violation.has_value()) {
      const auto &violation = *verdict->first_violation;
      verdict_out->violation_kind =
          violation.kind == ViolationKind::NoCompatibleSnapshot
              ? CO_CHECKER_ONLINE_VIOLATION_NO_COMPATIBLE_SNAPSHOT_V1
              : CO_CHECKER_ONLINE_VIOLATION_INCONSISTENT_REPEATED_READ_V1;
      verdict_out->commit_seq     = violation.commit_seq;
      verdict_out->transaction_id = to_c_uid(violation.transaction_id);
      verdict_out->session_id     = to_c_session(violation.session_id);
      verdict_out->session_seq    = violation.session_seq;
    }
    return CO_CHECKER_ONLINE_STATUS_OK_V1;
  } catch (...) {
    return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
  }
}

co_checker_online_status_v1 co_checker_online_error_get_v1(
    const co_checker_online_runtime_v1 *runtime,
    co_checker_online_error_info_v1 *error_out, char *message,
    const std::uint64_t message_capacity, std::uint64_t *message_size_out) {
  if (runtime == nullptr || error_out == nullptr ||
      message_size_out == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    *error_out             = co_checker_online_error_info_v1{};
    error_out->struct_size = sizeof(*error_out);
    error_out->abi_version = CO_CHECKER_ONLINE_ABI_VERSION_V1;
    std::string text;
    if (const auto failure = runtime->native->failure(); failure.has_value()) {
      error_out->present        = 1U;
      error_out->adapter_error  = failure->code;
      error_out->external_error = failure->external_code;
      text                      = failure->message;
    }
    if (const auto runtime_error = runtime->native->runtime_error();
        runtime_error.has_value()) {
      error_out->present = 1U;
      error_out->runtime_error =
          static_cast<std::uint32_t>(runtime_error->code) + 1U;
      if (runtime_error->queue_error.has_value()) {
        error_out->source_error =
            static_cast<std::uint32_t>(*runtime_error->queue_error) + 1U;
      } else if (runtime_error->order_error.has_value()) {
        error_out->source_error =
            static_cast<std::uint32_t>(*runtime_error->order_error) + 1U;
      } else if (runtime_error->engine_error.has_value()) {
        error_out->source_error =
            static_cast<std::uint32_t>(*runtime_error->engine_error) + 1U;
      }
      if (text.empty()) {
        text = runtime_error->message;
      }
    }
    if (error_out->present == 0U) {
      *message_size_out       = 0U;
      error_out->message_size = 0U;
      if (message != nullptr && message_capacity != 0U) {
        message[0] = '\0';
      }
      return CO_CHECKER_ONLINE_STATUS_OK_V1;
    }
    const auto status =
        copy_output_string(text, message, message_capacity, message_size_out);
    error_out->message_size = *message_size_out;
    return status;
  } catch (...) {
    return CO_CHECKER_ONLINE_STATUS_RESOURCE_ERROR_V1;
  }
}

co_checker_online_status_v1
co_checker_online_metrics_json_v1(const co_checker_online_runtime_v1 *runtime,
                                  char *json, const std::uint64_t json_capacity,
                                  std::uint64_t *json_size_out) {
  if (runtime == nullptr || json_size_out == nullptr) {
    return CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1;
  }
  try {
    return copy_output_string(runtime->native->metrics_json(), json,
                              json_capacity, json_size_out);
  } catch (const std::bad_alloc &) {
    return CO_CHECKER_ONLINE_STATUS_RESOURCE_ERROR_V1;
  } catch (...) {
    return CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1;
  }
}

} // extern "C"
