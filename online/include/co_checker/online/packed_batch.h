#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "co_checker/online/types.h"

namespace co_checker::online {

inline constexpr std::array<std::uint8_t, 8> kPackedBatchMagicV1{
    'C', 'O', 'B', 'A', 'T', 'C', 'H', '1'};
inline constexpr std::uint16_t kPackedBatchVersionV1 = 1;
inline constexpr std::uint16_t kNullSegment          = 0xffffU;
// CommitSeq is assigned only after the order gate closes a prefix. Captured
// producer batches must carry this sentinel and receive an ordered metadata
// overlay before checker consumption.
inline constexpr CommitSeq kUnassignedCommitSeq =
    std::numeric_limits<CommitSeq>::max();
inline constexpr std::uint64_t kSupportedBatchFeaturesV1      = 0;
inline constexpr std::uint32_t kMetadataHasCommitTieBreakerV1 = 1U << 0U;
inline constexpr std::uint32_t kSupportedMetadataFlagsV1 =
    kMetadataHasCommitTieBreakerV1;

enum class PackedEventOp : std::uint8_t {
  Invalid = 0,
  Read    = 1,
  Write   = 2,
};

enum class PackedValueState : std::uint8_t {
  Invalid = 0,
  Present = 1,
  Absent  = 2,
};

struct BlobRefV1 {
  std::uint32_t offset_le;
  std::uint32_t size_le;
  std::uint16_t segment_le;
  std::uint16_t reserved_le;
};

struct alignas(8) EventDescriptorV1 {
  std::uint32_t sequence_le;
  std::uint8_t op;
  std::uint8_t value_state;
  std::uint8_t flags;
  std::uint8_t reserved;
  BlobRefV1 key;
  BlobRefV1 value;
};

// Every multi-byte integer is stored little-endian. Metadata flags describe
// optional values in otherwise fixed fields; feature bits describe format
// extensions. Unknown flags or features are rejected by the V1 decoder.
struct alignas(8) BatchHeaderV1 {
  std::array<std::uint8_t, 8> magic;
  std::uint16_t version_le;
  std::uint16_t header_size_le;
  std::uint16_t descriptor_size_le;
  std::uint16_t blob_ref_size_le;
  std::uint64_t feature_bits_le;
  std::uint32_t metadata_flags_le;
  std::uint32_t reserved0_le;

  std::uint32_t event_count_le;
  std::uint32_t read_count_le;
  std::uint32_t write_count_le;
  std::uint32_t descriptor_span_count_le;
  std::uint32_t payload_segment_count_le;
  std::uint32_t reserved1_le;

  std::uint64_t commit_seq_le;
  std::uint64_t transaction_uid_word0_le;
  std::uint64_t transaction_uid_word1_le;
  std::uint64_t attempt_uid_word0_le;
  std::uint64_t attempt_uid_word1_le;
  std::uint64_t runtime_epoch_word0_le;
  std::uint64_t runtime_epoch_word1_le;
  std::uint32_t session_producer_id_le;
  std::uint32_t producer_id_le;
  std::uint64_t local_session_counter_le;
  std::uint64_t session_seq_le;
  std::uint64_t commit_token_cluster_epoch_le;
  std::uint64_t commit_token_timestamp_le;
  std::uint64_t commit_token_tie_breaker_le;
  std::uint64_t producer_seq_le;
};

static_assert(sizeof(BlobRefV1) == 12);
static_assert(offsetof(BlobRefV1, offset_le) == 0);
static_assert(offsetof(BlobRefV1, size_le) == 4);
static_assert(offsetof(BlobRefV1, segment_le) == 8);
static_assert(offsetof(BlobRefV1, reserved_le) == 10);

static_assert(sizeof(EventDescriptorV1) == 32);
static_assert(alignof(EventDescriptorV1) == 8);
static_assert(offsetof(EventDescriptorV1, sequence_le) == 0);
static_assert(offsetof(EventDescriptorV1, op) == 4);
static_assert(offsetof(EventDescriptorV1, value_state) == 5);
static_assert(offsetof(EventDescriptorV1, flags) == 6);
static_assert(offsetof(EventDescriptorV1, reserved) == 7);
static_assert(offsetof(EventDescriptorV1, key) == 8);
static_assert(offsetof(EventDescriptorV1, value) == 20);

static_assert(sizeof(BatchHeaderV1) == 168);
static_assert(alignof(BatchHeaderV1) == 8);
static_assert(offsetof(BatchHeaderV1, magic) == 0);
static_assert(offsetof(BatchHeaderV1, version_le) == 8);
static_assert(offsetof(BatchHeaderV1, header_size_le) == 10);
static_assert(offsetof(BatchHeaderV1, descriptor_size_le) == 12);
static_assert(offsetof(BatchHeaderV1, blob_ref_size_le) == 14);
static_assert(offsetof(BatchHeaderV1, feature_bits_le) == 16);
static_assert(offsetof(BatchHeaderV1, metadata_flags_le) == 24);
static_assert(offsetof(BatchHeaderV1, reserved0_le) == 28);
static_assert(offsetof(BatchHeaderV1, event_count_le) == 32);
static_assert(offsetof(BatchHeaderV1, read_count_le) == 36);
static_assert(offsetof(BatchHeaderV1, write_count_le) == 40);
static_assert(offsetof(BatchHeaderV1, descriptor_span_count_le) == 44);
static_assert(offsetof(BatchHeaderV1, payload_segment_count_le) == 48);
static_assert(offsetof(BatchHeaderV1, reserved1_le) == 52);
static_assert(offsetof(BatchHeaderV1, commit_seq_le) == 56);
static_assert(offsetof(BatchHeaderV1, transaction_uid_word0_le) == 64);
static_assert(offsetof(BatchHeaderV1, transaction_uid_word1_le) == 72);
static_assert(offsetof(BatchHeaderV1, attempt_uid_word0_le) == 80);
static_assert(offsetof(BatchHeaderV1, attempt_uid_word1_le) == 88);
static_assert(offsetof(BatchHeaderV1, runtime_epoch_word0_le) == 96);
static_assert(offsetof(BatchHeaderV1, runtime_epoch_word1_le) == 104);
static_assert(offsetof(BatchHeaderV1, session_producer_id_le) == 112);
static_assert(offsetof(BatchHeaderV1, producer_id_le) == 116);
static_assert(offsetof(BatchHeaderV1, local_session_counter_le) == 120);
static_assert(offsetof(BatchHeaderV1, session_seq_le) == 128);
static_assert(offsetof(BatchHeaderV1, commit_token_cluster_epoch_le) == 136);
static_assert(offsetof(BatchHeaderV1, commit_token_timestamp_le) == 144);
static_assert(offsetof(BatchHeaderV1, commit_token_tie_breaker_le) == 152);
static_assert(offsetof(BatchHeaderV1, producer_seq_le) == 160);
static_assert(std::is_standard_layout_v<BlobRefV1>);
static_assert(std::is_standard_layout_v<EventDescriptorV1>);
static_assert(std::is_standard_layout_v<BatchHeaderV1>);
static_assert(std::is_trivially_copyable_v<BlobRefV1>);
static_assert(std::is_trivially_copyable_v<EventDescriptorV1>);
static_assert(std::is_trivially_copyable_v<BatchHeaderV1>);

namespace detail {

template <typename T, std::size_t Alignment> class AlignedAllocator {
public:
  using value_type      = T;
  using is_always_equal = std::true_type;

  static_assert(Alignment >= alignof(T));
  static_assert((Alignment & (Alignment - 1U)) == 0);

  AlignedAllocator() noexcept = default;

  template <typename U>
  AlignedAllocator(const AlignedAllocator<U, Alignment> &) noexcept {}

  [[nodiscard]] T *allocate(std::size_t count) {
    if (count == 0) {
      return nullptr;
    }
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::bad_array_new_length();
    }
    return static_cast<T *>(
        ::operator new(count * sizeof(T), std::align_val_t{Alignment}));
  }

  void deallocate(T *pointer, std::size_t) noexcept {
    ::operator delete(pointer, std::align_val_t{Alignment});
  }

  template <typename U> struct rebind {
    using other = AlignedAllocator<U, Alignment>;
  };
};

template <typename T, typename U, std::size_t Alignment>
bool operator==(const AlignedAllocator<T, Alignment> &,
                const AlignedAllocator<U, Alignment> &) noexcept {
  return true;
}

template <typename T, typename U, std::size_t Alignment>
bool operator!=(const AlignedAllocator<T, Alignment> &,
                const AlignedAllocator<U, Alignment> &) noexcept {
  return false;
}

using DescriptorVectorV1 =
    std::vector<EventDescriptorV1, AlignedAllocator<EventDescriptorV1, 64>>;
using AlignedByteVector =
    std::vector<std::byte, AlignedAllocator<std::byte, 64>>;

} // namespace detail

struct PresentBytes {
  ByteView bytes;
};

struct Absent {};

using TaggedBytes = std::variant<PresentBytes, Absent>;

// Runtime-only scatter/gather description. The referenced header and
// descriptor bytes use the pointer-free V1 wire format; the spans themselves
// are borrowed and must outlive any ValidatedTxnView produced from them.
struct PackedBatchInput {
  ByteView header;
  std::span<const ByteView> descriptor_spans;
  std::span<const ByteView> payload_segments;
};

enum class BatchValidationErrorCode : std::uint8_t {
  HeaderSizeMismatch,
  InvalidMagic,
  UnsupportedVersion,
  HeaderLayoutMismatch,
  UnsupportedFeatures,
  UnsupportedMetadataFlags,
  NonZeroHeaderReserved,
  NonCanonicalMetadata,
  NonCanonicalCommitSequence,
  DescriptorSpanCountMismatch,
  PayloadSegmentCountMismatch,
  TooManyPayloadSegments,
  EmptyDescriptorSpan,
  MisalignedDescriptorSpan,
  EmptyPayloadSegment,
  EventCountMismatch,
  OperationCountMismatch,
  NonIncreasingEventSequence,
  InvalidOperation,
  InvalidValueState,
  NonZeroDescriptorFlags,
  NonZeroDescriptorReserved,
  NonZeroBlobReserved,
  NonCanonicalNullReference,
  NonCanonicalEmptyReference,
  PayloadSegmentOutOfRange,
  PayloadBounds,
  AbsentValueHasPayload,
};

struct BatchValidationError {
  BatchValidationErrorCode code = BatchValidationErrorCode::HeaderSizeMismatch;
  std::optional<std::uint64_t> event_index;

  friend bool operator==(const BatchValidationError &,
                         const BatchValidationError &) = default;
};

enum class PackedValidationMode : std::uint8_t;

class ValidatedTxnView {
public:
  class EventIterator {
  public:
    using iterator_concept  = std::input_iterator_tag;
    using iterator_category = std::input_iterator_tag;
    using value_type        = EventView;
    using difference_type   = std::ptrdiff_t;
    using reference         = EventView;

    EventIterator() = default;

    [[nodiscard]] EventView operator*() const;
    EventIterator &operator++();
    EventIterator operator++(int);

    friend bool operator==(const EventIterator &lhs,
                           const EventIterator &rhs) noexcept {
      return lhs.view_ == rhs.view_ && lhs.event_index_ == rhs.event_index_;
    }

  private:
    friend class ValidatedTxnView;
    EventIterator(const ValidatedTxnView *view, std::uint64_t event_index,
                  std::size_t span_index,
                  std::size_t descriptor_offset) noexcept;

    const ValidatedTxnView *view_  = nullptr;
    std::uint64_t event_index_     = 0;
    std::size_t span_index_        = 0;
    std::size_t descriptor_offset_ = 0;
  };

  class EventRange {
  public:
    [[nodiscard]] EventIterator begin() const noexcept;
    [[nodiscard]] EventIterator end() const noexcept;

  private:
    friend class ValidatedTxnView;
    explicit EventRange(const ValidatedTxnView *view) noexcept : view_(view) {}

    const ValidatedTxnView *view_;
  };

  [[nodiscard]] const OrderedTxnMeta &meta() const noexcept { return meta_; }
  [[nodiscard]] EventRange events() const noexcept { return EventRange(this); }
  [[nodiscard]] std::uint32_t event_count() const noexcept {
    return event_count_;
  }
  [[nodiscard]] std::uint32_t read_count() const noexcept {
    return read_count_;
  }
  [[nodiscard]] std::uint32_t write_count() const noexcept {
    return write_count_;
  }
  // The returned view borrows the same descriptor and payload storage. This is
  // the zero-copy boundary used by the order gate after it assigns CommitSeq.
  [[nodiscard]] ValidatedTxnView with_meta(OrderedTxnMeta meta) const noexcept;
  [[nodiscard]] ValidatedTxnView
  with_commit_seq(CommitSeq commit_seq) const noexcept;

private:
  friend std::variant<ValidatedTxnView, BatchValidationError>
  validate_packed_batch(const PackedBatchInput &input);
  friend std::variant<ValidatedTxnView, BatchValidationError>
  validate_packed_batch_with_mode(const PackedBatchInput &input,
                                  PackedValidationMode mode);

  ValidatedTxnView(OrderedTxnMeta meta,
                   std::span<const ByteView> descriptor_spans,
                   std::span<const ByteView> payload_segments,
                   std::uint32_t event_count, std::uint32_t read_count,
                   std::uint32_t write_count) noexcept;

  [[nodiscard]] EventView decode_event(std::size_t span_index,
                                       std::size_t descriptor_offset) const;

  OrderedTxnMeta meta_;
  std::span<const ByteView> descriptor_spans_;
  std::span<const ByteView> payload_segments_;
  std::uint32_t event_count_ = 0;
  std::uint32_t read_count_  = 0;
  std::uint32_t write_count_ = 0;
};

using PackedValidationResult =
    std::variant<ValidatedTxnView, BatchValidationError>;

enum class PackedValidationMode : std::uint8_t {
  Scalar,
  SSE2,
  AVX2,
  NEON,
};

[[nodiscard]] std::vector<PackedValidationMode>
supported_packed_validation_modes();
[[nodiscard]] PackedValidationMode default_packed_validation_mode() noexcept;

[[nodiscard]] PackedValidationResult
validate_packed_batch(const PackedBatchInput &input);
[[nodiscard]] PackedValidationResult
validate_packed_batch_with_mode(const PackedBatchInput &input,
                                PackedValidationMode mode);

enum class BatchBuildErrorCode : std::uint8_t {
  AlreadySealed,
  NonIncreasingEventSequence,
  InvalidReadSource,
  EventCountOverflow,
  BlobTooLarge,
  PayloadSegmentLimit,
  DescriptorSpanLimit,
  InvalidCheckpoint,
};

struct BatchBuildError {
  BatchBuildErrorCode code = BatchBuildErrorCode::AlreadySealed;

  friend bool operator==(const BatchBuildError &,
                         const BatchBuildError &) = default;
};

struct BatchBuildStatus {
  std::optional<BatchBuildError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }

  static BatchBuildStatus success() noexcept { return {}; }
  static BatchBuildStatus failure(BatchBuildErrorCode code) noexcept {
    return BatchBuildStatus{BatchBuildError{code}};
  }
};

class VectorBatchBuilder;

class VectorBatch final {
public:
  VectorBatch(const VectorBatch &)            = delete;
  VectorBatch &operator=(const VectorBatch &) = delete;
  VectorBatch(VectorBatch &&)                 = delete;
  VectorBatch &operator=(VectorBatch &&)      = delete;
  ~VectorBatch()                              = default;

  [[nodiscard]] const ValidatedTxnView &view() const noexcept;
  [[nodiscard]] PackedBatchInput input() const noexcept;
  [[nodiscard]] std::size_t used_bytes() const noexcept;
  [[nodiscard]] std::size_t reserved_bytes() const noexcept;

private:
  friend class VectorBatchBuilder;
  VectorBatch() = default;

  BatchHeaderV1 header_{};
  detail::DescriptorVectorV1 descriptors_;
  std::vector<std::byte> payload_;
  std::array<ByteView, 1> descriptor_views_{};
  std::array<ByteView, 1> payload_views_{};
  std::size_t descriptor_view_count_ = 0;
  std::size_t payload_view_count_    = 0;
  std::optional<ValidatedTxnView> view_;
};

struct VectorBatchSealResult {
  std::unique_ptr<VectorBatch> batch;
  std::optional<BatchBuildError> build_error;
  std::optional<BatchValidationError> validation_error;

  [[nodiscard]] bool ok() const noexcept { return batch != nullptr; }
};

class VectorBatchBuilder final {
public:
  explicit VectorBatchBuilder(OrderedTxnMeta meta);

  VectorBatchBuilder(const VectorBatchBuilder &)            = delete;
  VectorBatchBuilder &operator=(const VectorBatchBuilder &) = delete;
  VectorBatchBuilder(VectorBatchBuilder &&)                 = delete;
  VectorBatchBuilder &operator=(VectorBatchBuilder &&)      = delete;

  [[nodiscard]] BatchBuildStatus reserve(std::size_t event_capacity,
                                         std::size_t payload_capacity);
  [[nodiscard]] BatchBuildStatus append_read(EventSeq sequence, ByteView key,
                                             TaggedBytes result,
                                             ReadSource source);
  [[nodiscard]] BatchBuildStatus append_write(EventSeq sequence, ByteView key,
                                              TaggedBytes result);
  [[nodiscard]] VectorBatchSealResult seal();

private:
  [[nodiscard]] BatchBuildStatus check_sequence(EventSeq sequence) const;
  void accept_sequence(EventSeq sequence) noexcept;

  OrderedTxnMeta meta_;
  std::unique_ptr<VectorBatch> batch_;
  EventSeq last_sequence_    = 0;
  bool has_sequence_         = false;
  std::uint32_t read_count_  = 0;
  std::uint32_t write_count_ = 0;
};

struct ChunkPoolStats {
  std::uint64_t acquisitions           = 0;
  std::uint64_t allocations            = 0;
  std::uint64_t reuses                 = 0;
  std::uint64_t recycles               = 0;
  std::size_t cached_ordinary_segments = 0;
};

class ChunkBatch;
class ChunkBatchBuilder;

class ChunkPool final {
public:
  explicit ChunkPool(std::size_t ordinary_segment_size,
                     std::size_t max_cached_ordinary_segments = 64);
  ~ChunkPool() = default;

  ChunkPool(const ChunkPool &)            = delete;
  ChunkPool &operator=(const ChunkPool &) = delete;

  [[nodiscard]] std::size_t ordinary_segment_size() const noexcept {
    return ordinary_segment_size_;
  }
  [[nodiscard]] ChunkPoolStats stats() const;

private:
  struct Segment {
    explicit Segment(std::size_t capacity, bool is_dedicated)
        : bytes(capacity), dedicated(is_dedicated) {}

    detail::AlignedByteVector bytes;
    bool dedicated = false;
  };

  using SegmentPtr = std::unique_ptr<Segment>;

  [[nodiscard]] SegmentPtr acquire(std::size_t minimum_capacity,
                                   bool dedicated);
  void recycle(SegmentPtr segment) noexcept;

  friend class ChunkBatch;
  friend class ChunkBatchBuilder;

  const std::size_t ordinary_segment_size_;
  const std::size_t max_cached_ordinary_segments_;
  mutable std::mutex mutex_;
  std::vector<SegmentPtr> free_ordinary_segments_;
  std::uint64_t acquisitions_ = 0;
  std::uint64_t allocations_  = 0;
  std::uint64_t reuses_       = 0;
  std::uint64_t recycles_     = 0;
};

class ChunkBatch final {
public:
  ChunkBatch(const ChunkBatch &)            = delete;
  ChunkBatch &operator=(const ChunkBatch &) = delete;
  ChunkBatch(ChunkBatch &&)                 = delete;
  ChunkBatch &operator=(ChunkBatch &&)      = delete;
  ~ChunkBatch();

  [[nodiscard]] const ValidatedTxnView &view() const noexcept;
  [[nodiscard]] PackedBatchInput input() const noexcept;
  [[nodiscard]] std::size_t used_bytes() const noexcept;
  [[nodiscard]] std::size_t reserved_bytes() const noexcept;

private:
  struct OwnedSegment {
    ChunkPool::SegmentPtr segment;
    std::size_t used = 0;
  };

  friend class ChunkBatchBuilder;
  explicit ChunkBatch(std::shared_ptr<ChunkPool> pool)
      : pool_(std::move(pool)) {}

  void recycle_segments(std::vector<OwnedSegment> &segments) noexcept;

  std::shared_ptr<ChunkPool> pool_;
  BatchHeaderV1 header_{};
  std::vector<OwnedSegment> descriptor_segments_;
  std::vector<OwnedSegment> payload_segments_;
  std::vector<ByteView> descriptor_views_;
  std::vector<ByteView> payload_views_;
  std::optional<ValidatedTxnView> view_;
};

struct ChunkBatchSealResult {
  std::unique_ptr<ChunkBatch> batch;
  std::optional<BatchBuildError> build_error;
  std::optional<BatchValidationError> validation_error;

  [[nodiscard]] bool ok() const noexcept { return batch != nullptr; }
};

class ChunkCheckpoint {
public:
  ChunkCheckpoint() = default;

private:
  friend class ChunkBatchBuilder;

  std::uint64_t builder_cookie_         = 0;
  std::size_t descriptor_segment_count_ = 0;
  std::size_t descriptor_tail_used_     = 0;
  std::size_t payload_segment_count_    = 0;
  std::size_t payload_tail_used_        = 0;
  std::uint32_t read_count_             = 0;
  std::uint32_t write_count_            = 0;
  EventSeq last_sequence_               = 0;
  bool has_sequence_                    = false;
  std::size_t lineage_size_             = 0;
  std::uint64_t lineage_token_          = 0;
};

class ChunkBatchBuilder final {
public:
  ChunkBatchBuilder(std::shared_ptr<ChunkPool> pool, OrderedTxnMeta meta);
  ~ChunkBatchBuilder() = default;

  ChunkBatchBuilder(const ChunkBatchBuilder &)            = delete;
  ChunkBatchBuilder &operator=(const ChunkBatchBuilder &) = delete;
  ChunkBatchBuilder(ChunkBatchBuilder &&)                 = delete;
  ChunkBatchBuilder &operator=(ChunkBatchBuilder &&)      = delete;

  [[nodiscard]] BatchBuildStatus append_read(EventSeq sequence, ByteView key,
                                             TaggedBytes result,
                                             ReadSource source);
  [[nodiscard]] BatchBuildStatus append_write(EventSeq sequence, ByteView key,
                                              TaggedBytes result);

  [[nodiscard]] ChunkCheckpoint checkpoint() const noexcept;
  [[nodiscard]] BatchBuildStatus rollback(const ChunkCheckpoint &checkpoint);
  void abort() noexcept;

  [[nodiscard]] ChunkBatchSealResult seal();

private:
  struct StoredBlobRef {
    std::uint32_t offset  = 0;
    std::uint32_t size    = 0;
    std::uint16_t segment = kNullSegment;
  };

  [[nodiscard]] BatchBuildStatus check_sequence(EventSeq sequence) const;
  void accept_sequence(EventSeq sequence) noexcept;
  void record_lineage();
  [[nodiscard]] BatchBuildStatus append_normalized_event(EventSeq sequence,
                                                         PackedEventOp op,
                                                         ByteView key,
                                                         TaggedBytes result);
  [[nodiscard]] BatchBuildStatus
  store_event_payloads(ByteView key,
                       const std::optional<ByteView> &present_value,
                       StoredBlobRef &key_ref, StoredBlobRef &value_ref);
  [[nodiscard]] BatchBuildStatus store_blob(ByteView bytes, StoredBlobRef &ref);
  [[nodiscard]] BatchBuildStatus ensure_payload_space(std::size_t bytes);
  [[nodiscard]] BatchBuildStatus
  append_descriptor(EventSeq sequence, PackedEventOp op, PackedValueState state,
                    const StoredBlobRef &key_ref,
                    const StoredBlobRef &value_ref);
  [[nodiscard]] BatchBuildStatus ensure_descriptor_space();
  void restore(const ChunkCheckpoint &checkpoint) noexcept;

  std::shared_ptr<ChunkPool> pool_;
  OrderedTxnMeta meta_;
  std::unique_ptr<ChunkBatch> batch_;
  std::uint64_t builder_cookie_ = 0;
  EventSeq last_sequence_       = 0;
  bool has_sequence_            = false;
  std::uint32_t read_count_     = 0;
  std::uint32_t write_count_    = 0;
  std::vector<std::uint64_t> lineage_;
  std::uint64_t next_lineage_token_ = 1;
};

} // namespace co_checker::online
