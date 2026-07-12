#include "co_checker/online/packed_batch.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace co_checker::online {
namespace {

constexpr std::size_t kDescriptorSize = sizeof(EventDescriptorV1);
constexpr std::uint32_t kRequiredZeroByteMask =
    (std::uint32_t{1} << 6U) | (std::uint32_t{1} << 7U) |
    (std::uint32_t{1} << 18U) | (std::uint32_t{1} << 19U) |
    (std::uint32_t{1} << 30U) | (std::uint32_t{1} << 31U);

[[nodiscard]] bool
packed_validation_mode_supported(const PackedValidationMode mode) noexcept {
  switch (mode) {
  case PackedValidationMode::Scalar:
    return true;
  case PackedValidationMode::SSE2:
#if defined(__x86_64__)
    return true;
#elif defined(__i386__) && (defined(__GNUC__) || defined(__clang__))
    return __builtin_cpu_supports("sse2");
#else
    return false;
#endif
  case PackedValidationMode::AVX2:
#if (defined(__x86_64__) || defined(__i386__)) &&                              \
    (defined(__GNUC__) || defined(__clang__))
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
  case PackedValidationMode::NEON:
#if defined(__aarch64__) || defined(__ARM_NEON)
    return true;
#else
    return false;
#endif
  }
  return false;
}

[[nodiscard]] bool valid_operation_and_state(const std::byte *descriptor) {
  const auto op =
      static_cast<PackedEventOp>(std::to_integer<std::uint8_t>(descriptor[4]));
  const auto state = static_cast<PackedValueState>(
      std::to_integer<std::uint8_t>(descriptor[5]));
  return (op == PackedEventOp::Read || op == PackedEventOp::Write) &&
         (state == PackedValueState::Present ||
          state == PackedValueState::Absent);
}

#if defined(__x86_64__) || defined(__i386__)
[[nodiscard]] bool
sse2_descriptor_controls_valid(const std::byte *descriptor) noexcept {
  const auto first = _mm_loadu_si128(
      reinterpret_cast<const __m128i *>(static_cast<const void *>(descriptor)));
  const auto second     = _mm_loadu_si128(reinterpret_cast<const __m128i *>(
      static_cast<const void *>(descriptor + 16)));
  const auto zero       = _mm_setzero_si128();
  const auto first_mask = static_cast<std::uint32_t>(
      _mm_movemask_epi8(_mm_cmpeq_epi8(first, zero)));
  const auto second_mask = static_cast<std::uint32_t>(
      _mm_movemask_epi8(_mm_cmpeq_epi8(second, zero)));
  const auto zero_mask = first_mask | (second_mask << 16U);
  return (zero_mask & kRequiredZeroByteMask) == kRequiredZeroByteMask &&
         valid_operation_and_state(descriptor);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
bool avx2_descriptor_controls_valid(const std::byte *descriptor) noexcept {
  const auto bytes = _mm256_loadu_si256(
      reinterpret_cast<const __m256i *>(static_cast<const void *>(descriptor)));
  const auto zero_mask = static_cast<std::uint32_t>(
      _mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, _mm256_setzero_si256())));
  return (zero_mask & kRequiredZeroByteMask) == kRequiredZeroByteMask &&
         valid_operation_and_state(descriptor);
}
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
[[nodiscard]] bool
neon_descriptor_controls_valid(const std::byte *descriptor) noexcept {
  const auto first =
      vceqq_u8(vld1q_u8(reinterpret_cast<const std::uint8_t *>(descriptor)),
               vdupq_n_u8(0));
  const auto second = vceqq_u8(
      vld1q_u8(reinterpret_cast<const std::uint8_t *>(descriptor + 16)),
      vdupq_n_u8(0));
  alignas(16) std::uint8_t zero[32];
  vst1q_u8(zero, first);
  vst1q_u8(zero + 16, second);
  for (const auto offset : {6U, 7U, 18U, 19U, 30U, 31U}) {
    if (zero[offset] == 0) {
      return false;
    }
  }
  return valid_operation_and_state(descriptor);
}
#endif

[[nodiscard]] bool
descriptor_controls_valid(const ByteView descriptor,
                          const PackedValidationMode mode) noexcept {
  switch (mode) {
  case PackedValidationMode::AVX2:
#if defined(__x86_64__) || defined(__i386__)
    return avx2_descriptor_controls_valid(descriptor.data());
#else
    break;
#endif
  case PackedValidationMode::SSE2:
#if defined(__x86_64__) || defined(__i386__)
    return sse2_descriptor_controls_valid(descriptor.data());
#else
    break;
#endif
  case PackedValidationMode::NEON:
#if defined(__aarch64__) || defined(__ARM_NEON)
    return neon_descriptor_controls_valid(descriptor.data());
#else
    break;
#endif
  case PackedValidationMode::Scalar:
    break;
  }
  return false;
}

[[nodiscard]] bool
descriptor_span_controls_valid(const ByteView span,
                               const PackedValidationMode mode) noexcept {
  if (mode == PackedValidationMode::Scalar) {
    return false;
  }
  for (std::size_t offset = 0; offset < span.size();
       offset += kDescriptorSize) {
    if (!descriptor_controls_valid(span.subspan(offset, kDescriptorSize),
                                   mode)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::uint16_t read_u16_le(ByteView bytes,
                                        std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) |
      (static_cast<std::uint16_t>(
           std::to_integer<std::uint8_t>(bytes[offset + 1]))
       << 8U));
}

[[nodiscard]] std::uint32_t read_u32_le(ByteView bytes,
                                        std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(bytes[offset + index]))
             << static_cast<unsigned>(index * 8U);
  }
  return value;
}

[[nodiscard]] std::uint64_t read_u64_le(ByteView bytes,
                                        std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(bytes[offset + index]))
             << static_cast<unsigned>(index * 8U);
  }
  return value;
}

void write_u16_le(std::span<std::byte> bytes, std::size_t offset,
                  std::uint16_t value) noexcept {
  for (std::size_t index = 0; index < 2; ++index) {
    bytes[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned>(index * 8U)));
  }
}

void write_u32_le(std::span<std::byte> bytes, std::size_t offset,
                  std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned>(index * 8U)));
  }
}

void write_u64_le(std::span<std::byte> bytes, std::size_t offset,
                  std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned>(index * 8U)));
  }
}

[[nodiscard]] std::span<std::byte>
writable_header_bytes(BatchHeaderV1 &header) noexcept {
  return std::as_writable_bytes(std::span<BatchHeaderV1>(&header, 1));
}

[[nodiscard]] ByteView header_bytes(const BatchHeaderV1 &header) noexcept {
  return std::as_bytes(std::span<const BatchHeaderV1>(&header, 1));
}

struct DecodedBlobRef {
  std::uint32_t offset   = 0;
  std::uint32_t size     = 0;
  std::uint16_t segment  = kNullSegment;
  std::uint16_t reserved = 0;

  [[nodiscard]] bool is_null() const noexcept {
    return segment == kNullSegment;
  }
};

[[nodiscard]] DecodedBlobRef decode_blob_ref(ByteView descriptor,
                                             std::size_t offset) noexcept {
  return DecodedBlobRef{
      read_u32_le(descriptor, offset + offsetof(BlobRefV1, offset_le)),
      read_u32_le(descriptor, offset + offsetof(BlobRefV1, size_le)),
      read_u16_le(descriptor, offset + offsetof(BlobRefV1, segment_le)),
      read_u16_le(descriptor, offset + offsetof(BlobRefV1, reserved_le))};
}

void encode_blob_ref(std::span<std::byte> descriptor, std::size_t offset,
                     std::uint32_t blob_offset, std::uint32_t blob_size,
                     std::uint16_t segment) noexcept {
  write_u32_le(descriptor, offset + offsetof(BlobRefV1, offset_le),
               blob_offset);
  write_u32_le(descriptor, offset + offsetof(BlobRefV1, size_le), blob_size);
  write_u16_le(descriptor, offset + offsetof(BlobRefV1, segment_le), segment);
  write_u16_le(descriptor, offset + offsetof(BlobRefV1, reserved_le), 0);
}

template <typename Ref>
void encode_blob_ref(std::span<std::byte> descriptor, std::size_t offset,
                     const Ref &ref) noexcept {
  encode_blob_ref(descriptor, offset, ref.offset, ref.size, ref.segment);
}

[[nodiscard]] ByteView
resolve_blob(const DecodedBlobRef &ref,
             std::span<const ByteView> payload_segments) noexcept {
  if (ref.is_null()) {
    return {};
  }
  return payload_segments[ref.segment].subspan(ref.offset, ref.size);
}

[[nodiscard]] std::optional<BatchValidationErrorCode>
validate_blob_ref(const DecodedBlobRef &ref,
                  std::span<const ByteView> payload_segments) noexcept {
  if (ref.reserved != 0) {
    return BatchValidationErrorCode::NonZeroBlobReserved;
  }
  if (ref.is_null()) {
    if (ref.offset != 0 || ref.size != 0) {
      return BatchValidationErrorCode::NonCanonicalNullReference;
    }
    return std::nullopt;
  }
  if (ref.size == 0) {
    return BatchValidationErrorCode::NonCanonicalEmptyReference;
  }
  if (static_cast<std::size_t>(ref.segment) >= payload_segments.size()) {
    return BatchValidationErrorCode::PayloadSegmentOutOfRange;
  }

  // Both operands are 32-bit. Widening before addition makes wraparound
  // impossible even for deliberately malformed references.
  const auto end = static_cast<std::uintmax_t>(ref.offset) +
                   static_cast<std::uintmax_t>(ref.size);
  const auto segment_size =
      static_cast<std::uintmax_t>(payload_segments[ref.segment].size());
  if (end > segment_size) {
    return BatchValidationErrorCode::PayloadBounds;
  }
  return std::nullopt;
}

void encode_header(BatchHeaderV1 &header, const OrderedTxnMeta &meta,
                   std::uint32_t event_count, std::uint32_t read_count,
                   std::uint32_t write_count,
                   std::uint32_t descriptor_span_count,
                   std::uint32_t payload_segment_count) noexcept {
  header       = {};
  header.magic = kPackedBatchMagicV1;
  auto bytes   = writable_header_bytes(header);
  write_u16_le(bytes, offsetof(BatchHeaderV1, version_le),
               kPackedBatchVersionV1);
  write_u16_le(bytes, offsetof(BatchHeaderV1, header_size_le),
               static_cast<std::uint16_t>(sizeof(BatchHeaderV1)));
  write_u16_le(bytes, offsetof(BatchHeaderV1, descriptor_size_le),
               static_cast<std::uint16_t>(sizeof(EventDescriptorV1)));
  write_u16_le(bytes, offsetof(BatchHeaderV1, blob_ref_size_le),
               static_cast<std::uint16_t>(sizeof(BlobRefV1)));
  write_u64_le(bytes, offsetof(BatchHeaderV1, feature_bits_le), 0);

  const std::uint32_t metadata_flags =
      meta.commit_token.authoritative_tie_breaker.has_value()
          ? kMetadataHasCommitTieBreakerV1
          : 0;
  write_u32_le(bytes, offsetof(BatchHeaderV1, metadata_flags_le),
               metadata_flags);
  write_u32_le(bytes, offsetof(BatchHeaderV1, event_count_le), event_count);
  write_u32_le(bytes, offsetof(BatchHeaderV1, read_count_le), read_count);
  write_u32_le(bytes, offsetof(BatchHeaderV1, write_count_le), write_count);
  write_u32_le(bytes, offsetof(BatchHeaderV1, descriptor_span_count_le),
               descriptor_span_count);
  write_u32_le(bytes, offsetof(BatchHeaderV1, payload_segment_count_le),
               payload_segment_count);

  write_u64_le(bytes, offsetof(BatchHeaderV1, commit_seq_le),
               kUnassignedCommitSeq);
  write_u64_le(bytes, offsetof(BatchHeaderV1, transaction_uid_word0_le),
               meta.transaction_id.words[0]);
  write_u64_le(bytes, offsetof(BatchHeaderV1, transaction_uid_word1_le),
               meta.transaction_id.words[1]);
  write_u64_le(bytes, offsetof(BatchHeaderV1, attempt_uid_word0_le),
               meta.attempt_id.words[0]);
  write_u64_le(bytes, offsetof(BatchHeaderV1, attempt_uid_word1_le),
               meta.attempt_id.words[1]);
  write_u64_le(bytes, offsetof(BatchHeaderV1, runtime_epoch_word0_le),
               meta.session_id.runtime_epoch.words[0]);
  write_u64_le(bytes, offsetof(BatchHeaderV1, runtime_epoch_word1_le),
               meta.session_id.runtime_epoch.words[1]);
  write_u32_le(bytes, offsetof(BatchHeaderV1, session_producer_id_le),
               meta.session_id.producer_id);
  write_u32_le(bytes, offsetof(BatchHeaderV1, producer_id_le),
               meta.producer_id);
  write_u64_le(bytes, offsetof(BatchHeaderV1, local_session_counter_le),
               meta.session_id.local_session_counter);
  write_u64_le(bytes, offsetof(BatchHeaderV1, session_seq_le),
               meta.session_seq);
  write_u64_le(bytes, offsetof(BatchHeaderV1, commit_token_cluster_epoch_le),
               meta.commit_token.cluster_epoch);
  write_u64_le(bytes, offsetof(BatchHeaderV1, commit_token_timestamp_le),
               meta.commit_token.timestamp);
  write_u64_le(bytes, offsetof(BatchHeaderV1, commit_token_tie_breaker_le),
               meta.commit_token.authoritative_tie_breaker.value_or(0));
  write_u64_le(bytes, offsetof(BatchHeaderV1, producer_seq_le),
               meta.producer_seq);
}

[[nodiscard]] OrderedTxnMeta decode_meta(ByteView header) {
  OrderedTxnMeta meta;
  meta.commit_seq = read_u64_le(header, offsetof(BatchHeaderV1, commit_seq_le));
  meta.transaction_id.words = {
      read_u64_le(header, offsetof(BatchHeaderV1, transaction_uid_word0_le)),
      read_u64_le(header, offsetof(BatchHeaderV1, transaction_uid_word1_le))};
  meta.attempt_id.words = {
      read_u64_le(header, offsetof(BatchHeaderV1, attempt_uid_word0_le)),
      read_u64_le(header, offsetof(BatchHeaderV1, attempt_uid_word1_le))};
  meta.session_id.runtime_epoch.words = {
      read_u64_le(header, offsetof(BatchHeaderV1, runtime_epoch_word0_le)),
      read_u64_le(header, offsetof(BatchHeaderV1, runtime_epoch_word1_le))};
  meta.session_id.producer_id =
      read_u32_le(header, offsetof(BatchHeaderV1, session_producer_id_le));
  meta.producer_id =
      read_u32_le(header, offsetof(BatchHeaderV1, producer_id_le));
  meta.session_id.local_session_counter =
      read_u64_le(header, offsetof(BatchHeaderV1, local_session_counter_le));
  meta.session_seq =
      read_u64_le(header, offsetof(BatchHeaderV1, session_seq_le));
  meta.commit_token.cluster_epoch = read_u64_le(
      header, offsetof(BatchHeaderV1, commit_token_cluster_epoch_le));
  meta.commit_token.timestamp =
      read_u64_le(header, offsetof(BatchHeaderV1, commit_token_timestamp_le));
  const auto metadata_flags =
      read_u32_le(header, offsetof(BatchHeaderV1, metadata_flags_le));
  if ((metadata_flags & kMetadataHasCommitTieBreakerV1) != 0) {
    meta.commit_token.authoritative_tie_breaker = read_u64_le(
        header, offsetof(BatchHeaderV1, commit_token_tie_breaker_le));
  }
  meta.producer_seq =
      read_u64_le(header, offsetof(BatchHeaderV1, producer_seq_le));
  return meta;
}

[[nodiscard]] BatchValidationError validation_error(
    BatchValidationErrorCode code,
    std::optional<std::uint64_t> event_index = std::nullopt) noexcept {
  return BatchValidationError{code, event_index};
}

struct StoredVectorBlobRef {
  std::uint32_t offset  = 0;
  std::uint32_t size    = 0;
  std::uint16_t segment = kNullSegment;
};

[[nodiscard]] BatchBuildStatus
store_vector_blob(std::vector<std::byte> &payload, ByteView value,
                  StoredVectorBlobRef &ref) {
  ref         = {};
  ref.segment = kNullSegment;
  if (value.empty()) {
    return BatchBuildStatus::success();
  }
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::BlobTooLarge);
  }
  const auto end = static_cast<std::uint64_t>(payload.size()) +
                   static_cast<std::uint64_t>(value.size());
  if (end > std::numeric_limits<std::uint32_t>::max()) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::BlobTooLarge);
  }

  ref.offset  = static_cast<std::uint32_t>(payload.size());
  ref.size    = static_cast<std::uint32_t>(value.size());
  ref.segment = 0;
  payload.insert(payload.end(), value.begin(), value.end());
  return BatchBuildStatus::success();
}

[[nodiscard]] BatchBuildStatus
append_vector_record(detail::DescriptorVectorV1 &descriptors,
                     std::vector<std::byte> &payload, EventSeq sequence,
                     PackedEventOp op, ByteView key,
                     const TaggedBytes &result) {
  const auto old_payload_size = payload.size();
  StoredVectorBlobRef key_ref;
  StoredVectorBlobRef value_ref;
  const auto state = std::holds_alternative<PresentBytes>(result)
                         ? PackedValueState::Present
                         : PackedValueState::Absent;

  BatchBuildStatus status;
  try {
    status = store_vector_blob(payload, key, key_ref);
    if (!status.ok()) {
      return status;
    }
    if (const auto *present = std::get_if<PresentBytes>(&result)) {
      status = store_vector_blob(payload, present->bytes, value_ref);
      if (!status.ok()) {
        payload.resize(old_payload_size);
        return status;
      }
    } else {
      value_ref.segment = kNullSegment;
    }
  } catch (...) {
    payload.resize(old_payload_size);
    throw;
  }

  EventDescriptorV1 descriptor{};
  auto bytes =
      std::as_writable_bytes(std::span<EventDescriptorV1>(&descriptor, 1));
  write_u32_le(bytes, offsetof(EventDescriptorV1, sequence_le), sequence);
  encode_blob_ref(bytes, offsetof(EventDescriptorV1, key), key_ref);
  encode_blob_ref(bytes, offsetof(EventDescriptorV1, value), value_ref);
  // Tags are the final fields written. A partially filled descriptor remains
  // invalid because both valid tag domains reserve zero.
  bytes[offsetof(EventDescriptorV1, value_state)] =
      static_cast<std::byte>(state);
  bytes[offsetof(EventDescriptorV1, op)] = static_cast<std::byte>(op);

  try {
    descriptors.push_back(descriptor);
  } catch (...) {
    payload.resize(old_payload_size);
    throw;
  }
  return BatchBuildStatus::success();
}

[[nodiscard]] std::uint32_t checked_u32_size(std::size_t value) {
  assert(value <= std::numeric_limits<std::uint32_t>::max());
  return static_cast<std::uint32_t>(value);
}

} // namespace

PackedValidationResult
validate_packed_batch_with_mode(const PackedBatchInput &input,
                                PackedValidationMode mode) {
  if (!packed_validation_mode_supported(mode)) {
    mode = PackedValidationMode::Scalar;
  }
  if (input.header.size() != sizeof(BatchHeaderV1)) {
    return validation_error(BatchValidationErrorCode::HeaderSizeMismatch);
  }
  for (std::size_t index = 0; index < kPackedBatchMagicV1.size(); ++index) {
    if (std::to_integer<std::uint8_t>(input.header[index]) !=
        kPackedBatchMagicV1[index]) {
      return validation_error(BatchValidationErrorCode::InvalidMagic);
    }
  }

  if (read_u16_le(input.header, offsetof(BatchHeaderV1, version_le)) !=
      kPackedBatchVersionV1) {
    return validation_error(BatchValidationErrorCode::UnsupportedVersion);
  }
  if (read_u16_le(input.header, offsetof(BatchHeaderV1, header_size_le)) !=
          sizeof(BatchHeaderV1) ||
      read_u16_le(input.header, offsetof(BatchHeaderV1, descriptor_size_le)) !=
          sizeof(EventDescriptorV1) ||
      read_u16_le(input.header, offsetof(BatchHeaderV1, blob_ref_size_le)) !=
          sizeof(BlobRefV1)) {
    return validation_error(BatchValidationErrorCode::HeaderLayoutMismatch);
  }

  const auto feature_bits =
      read_u64_le(input.header, offsetof(BatchHeaderV1, feature_bits_le));
  if ((feature_bits & ~kSupportedBatchFeaturesV1) != 0) {
    return validation_error(BatchValidationErrorCode::UnsupportedFeatures);
  }
  const auto metadata_flags =
      read_u32_le(input.header, offsetof(BatchHeaderV1, metadata_flags_le));
  if ((metadata_flags & ~kSupportedMetadataFlagsV1) != 0) {
    return validation_error(BatchValidationErrorCode::UnsupportedMetadataFlags);
  }
  if (read_u32_le(input.header, offsetof(BatchHeaderV1, reserved0_le)) != 0 ||
      read_u32_le(input.header, offsetof(BatchHeaderV1, reserved1_le)) != 0) {
    return validation_error(BatchValidationErrorCode::NonZeroHeaderReserved);
  }
  if ((metadata_flags & kMetadataHasCommitTieBreakerV1) == 0 &&
      read_u64_le(input.header,
                  offsetof(BatchHeaderV1, commit_token_tie_breaker_le)) != 0) {
    return validation_error(BatchValidationErrorCode::NonCanonicalMetadata);
  }
  if (read_u64_le(input.header, offsetof(BatchHeaderV1, commit_seq_le)) !=
      kUnassignedCommitSeq) {
    return validation_error(
        BatchValidationErrorCode::NonCanonicalCommitSequence);
  }

  const auto event_count =
      read_u32_le(input.header, offsetof(BatchHeaderV1, event_count_le));
  const auto read_count =
      read_u32_le(input.header, offsetof(BatchHeaderV1, read_count_le));
  const auto write_count =
      read_u32_le(input.header, offsetof(BatchHeaderV1, write_count_le));
  const auto descriptor_span_count = read_u32_le(
      input.header, offsetof(BatchHeaderV1, descriptor_span_count_le));
  const auto payload_segment_count = read_u32_le(
      input.header, offsetof(BatchHeaderV1, payload_segment_count_le));

  if (input.descriptor_spans.size() != descriptor_span_count) {
    return validation_error(
        BatchValidationErrorCode::DescriptorSpanCountMismatch);
  }
  if (input.payload_segments.size() != payload_segment_count) {
    return validation_error(
        BatchValidationErrorCode::PayloadSegmentCountMismatch);
  }
  if (payload_segment_count > kNullSegment) {
    return validation_error(BatchValidationErrorCode::TooManyPayloadSegments);
  }
  for (const auto segment : input.payload_segments) {
    if (segment.empty()) {
      return validation_error(BatchValidationErrorCode::EmptyPayloadSegment);
    }
  }

  std::uintmax_t actual_event_count = 0;
  for (const auto span : input.descriptor_spans) {
    if (span.empty()) {
      return validation_error(BatchValidationErrorCode::EmptyDescriptorSpan);
    }
    if ((span.size() % kDescriptorSize) != 0) {
      return validation_error(
          BatchValidationErrorCode::MisalignedDescriptorSpan);
    }
    actual_event_count +=
        static_cast<std::uintmax_t>(span.size() / kDescriptorSize);
  }
  if (actual_event_count != event_count) {
    return validation_error(BatchValidationErrorCode::EventCountMismatch);
  }
  if (static_cast<std::uint64_t>(read_count) +
          static_cast<std::uint64_t>(write_count) !=
      event_count) {
    return validation_error(BatchValidationErrorCode::OperationCountMismatch);
  }

  bool have_previous_sequence = false;
  EventSeq previous_sequence  = 0;
  std::uint32_t actual_reads  = 0;
  std::uint32_t actual_writes = 0;
  std::uint64_t event_index   = 0;
  for (const auto descriptor_span : input.descriptor_spans) {
    const auto controls_prevalidated =
        descriptor_span_controls_valid(descriptor_span, mode);
    for (std::size_t offset = 0; offset < descriptor_span.size();
         offset += kDescriptorSize, ++event_index) {
      const auto descriptor = descriptor_span.subspan(offset, kDescriptorSize);
      const auto sequence =
          read_u32_le(descriptor, offsetof(EventDescriptorV1, sequence_le));
      if (have_previous_sequence && sequence <= previous_sequence) {
        return validation_error(
            BatchValidationErrorCode::NonIncreasingEventSequence, event_index);
      }
      previous_sequence      = sequence;
      have_previous_sequence = true;

      const auto op = static_cast<PackedEventOp>(std::to_integer<std::uint8_t>(
          descriptor[offsetof(EventDescriptorV1, op)]));
      const auto state =
          static_cast<PackedValueState>(std::to_integer<std::uint8_t>(
              descriptor[offsetof(EventDescriptorV1, value_state)]));
      if (!controls_prevalidated) {
        if (op != PackedEventOp::Read && op != PackedEventOp::Write) {
          return validation_error(BatchValidationErrorCode::InvalidOperation,
                                  event_index);
        }
        if (state != PackedValueState::Present &&
            state != PackedValueState::Absent) {
          return validation_error(BatchValidationErrorCode::InvalidValueState,
                                  event_index);
        }
        if (descriptor[offsetof(EventDescriptorV1, flags)] != std::byte{0}) {
          return validation_error(
              BatchValidationErrorCode::NonZeroDescriptorFlags, event_index);
        }
        if (descriptor[offsetof(EventDescriptorV1, reserved)] != std::byte{0}) {
          return validation_error(
              BatchValidationErrorCode::NonZeroDescriptorReserved, event_index);
        }
      }

      const auto key_ref =
          decode_blob_ref(descriptor, offsetof(EventDescriptorV1, key));
      if (const auto error =
              validate_blob_ref(key_ref, input.payload_segments)) {
        return validation_error(*error, event_index);
      }
      const auto value_ref =
          decode_blob_ref(descriptor, offsetof(EventDescriptorV1, value));
      if (const auto error =
              validate_blob_ref(value_ref, input.payload_segments)) {
        return validation_error(*error, event_index);
      }
      if (state == PackedValueState::Absent && !value_ref.is_null()) {
        return validation_error(BatchValidationErrorCode::AbsentValueHasPayload,
                                event_index);
      }

      if (op == PackedEventOp::Read) {
        ++actual_reads;
      } else {
        ++actual_writes;
      }
    }
  }
  if (actual_reads != read_count || actual_writes != write_count) {
    return validation_error(BatchValidationErrorCode::OperationCountMismatch);
  }

  return ValidatedTxnView(decode_meta(input.header), input.descriptor_spans,
                          input.payload_segments, event_count, read_count,
                          write_count);
}

std::vector<PackedValidationMode> supported_packed_validation_modes() {
  std::vector<PackedValidationMode> result{PackedValidationMode::Scalar};
  for (const auto mode :
       {PackedValidationMode::SSE2, PackedValidationMode::AVX2,
        PackedValidationMode::NEON}) {
    if (packed_validation_mode_supported(mode)) {
      result.push_back(mode);
    }
  }
  return result;
}

PackedValidationMode default_packed_validation_mode() noexcept {
  static const auto selected = [] {
#if defined(__x86_64__) || defined(__i386__)
    // Fixed descriptor control fields are sparse. On the measured x86 host,
    // loading whole SIMD registers costs more than the scalar field checks.
    // Keep the differential-tested SIMD paths selectable, but dispatch the
    // production default to the measured winner.
    return PackedValidationMode::Scalar;
#else
    if (packed_validation_mode_supported(PackedValidationMode::NEON)) {
      return PackedValidationMode::NEON;
    }
    return PackedValidationMode::Scalar;
#endif
  }();
  return selected;
}

PackedValidationResult validate_packed_batch(const PackedBatchInput &input) {
  return validate_packed_batch_with_mode(input,
                                         default_packed_validation_mode());
}

ValidatedTxnView::ValidatedTxnView(OrderedTxnMeta meta,
                                   std::span<const ByteView> descriptor_spans,
                                   std::span<const ByteView> payload_segments,
                                   std::uint32_t event_count,
                                   std::uint32_t read_count,
                                   std::uint32_t write_count) noexcept
    : meta_(std::move(meta)), descriptor_spans_(descriptor_spans),
      payload_segments_(payload_segments), event_count_(event_count),
      read_count_(read_count), write_count_(write_count) {}

ValidatedTxnView
ValidatedTxnView::with_meta(OrderedTxnMeta meta) const noexcept {
  return ValidatedTxnView(std::move(meta), descriptor_spans_, payload_segments_,
                          event_count_, read_count_, write_count_);
}

ValidatedTxnView
ValidatedTxnView::with_commit_seq(CommitSeq commit_seq) const noexcept {
  auto ordered_meta       = meta_;
  ordered_meta.commit_seq = commit_seq;
  return with_meta(std::move(ordered_meta));
}

EventView ValidatedTxnView::decode_event(std::size_t span_index,
                                         std::size_t descriptor_offset) const {
  const auto descriptor =
      descriptor_spans_[span_index].subspan(descriptor_offset, kDescriptorSize);
  const auto packed_op =
      static_cast<PackedEventOp>(std::to_integer<std::uint8_t>(
          descriptor[offsetof(EventDescriptorV1, op)]));
  const auto packed_state =
      static_cast<PackedValueState>(std::to_integer<std::uint8_t>(
          descriptor[offsetof(EventDescriptorV1, value_state)]));
  const auto key_ref =
      decode_blob_ref(descriptor, offsetof(EventDescriptorV1, key));
  const auto value_ref =
      decode_blob_ref(descriptor, offsetof(EventDescriptorV1, value));

  return EventView{
      .sequence =
          read_u32_le(descriptor, offsetof(EventDescriptorV1, sequence_le)),
      .op = packed_op == PackedEventOp::Read ? EventOp::Read : EventOp::Write,
      .value_state = packed_state == PackedValueState::Present
                         ? ValueState::Present
                         : ValueState::Absent,
      .read_source = ReadSource::Snapshot,
      .key         = resolve_blob(key_ref, payload_segments_),
      .value       = resolve_blob(value_ref, payload_segments_),
  };
}

ValidatedTxnView::EventIterator::EventIterator(
    const ValidatedTxnView *view, std::uint64_t event_index,
    std::size_t span_index, std::size_t descriptor_offset) noexcept
    : view_(view), event_index_(event_index), span_index_(span_index),
      descriptor_offset_(descriptor_offset) {}

EventView ValidatedTxnView::EventIterator::operator*() const {
  assert(view_ != nullptr);
  assert(event_index_ < view_->event_count_);
  return view_->decode_event(span_index_, descriptor_offset_);
}

ValidatedTxnView::EventIterator &ValidatedTxnView::EventIterator::operator++() {
  assert(view_ != nullptr);
  assert(event_index_ < view_->event_count_);
  ++event_index_;
  if (event_index_ == view_->event_count_) {
    return *this;
  }

  descriptor_offset_ += kDescriptorSize;
  if (descriptor_offset_ == view_->descriptor_spans_[span_index_].size()) {
    ++span_index_;
    descriptor_offset_ = 0;
  }
  return *this;
}

ValidatedTxnView::EventIterator
ValidatedTxnView::EventIterator::operator++(int) {
  auto previous = *this;
  ++(*this);
  return previous;
}

ValidatedTxnView::EventIterator
ValidatedTxnView::EventRange::begin() const noexcept {
  return EventIterator(view_, 0, 0, 0);
}

ValidatedTxnView::EventIterator
ValidatedTxnView::EventRange::end() const noexcept {
  return EventIterator(view_, view_->event_count_,
                       view_->descriptor_spans_.size(), 0);
}

const ValidatedTxnView &VectorBatch::view() const noexcept {
  assert(view_.has_value());
  return *view_;
}

PackedBatchInput VectorBatch::input() const noexcept {
  return PackedBatchInput{
      header_bytes(header_),
      std::span<const ByteView>(descriptor_views_.data(),
                                descriptor_view_count_),
      std::span<const ByteView>(payload_views_.data(), payload_view_count_)};
}

std::size_t VectorBatch::used_bytes() const noexcept {
  return sizeof(header_) + descriptors_.size() * sizeof(EventDescriptorV1) +
         payload_.size();
}

std::size_t VectorBatch::reserved_bytes() const noexcept {
  return sizeof(header_) + descriptors_.capacity() * sizeof(EventDescriptorV1) +
         payload_.capacity();
}

VectorBatchBuilder::VectorBatchBuilder(OrderedTxnMeta meta)
    : meta_(std::move(meta)), batch_(new VectorBatch()) {
  meta_.commit_seq = kUnassignedCommitSeq;
}

BatchBuildStatus VectorBatchBuilder::reserve(std::size_t event_capacity,
                                             std::size_t payload_capacity) {
  if (batch_ == nullptr) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::AlreadySealed);
  }
  if (event_capacity > std::numeric_limits<std::uint32_t>::max()) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::EventCountOverflow);
  }
  if (payload_capacity > std::numeric_limits<std::uint32_t>::max()) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::BlobTooLarge);
  }
  batch_->descriptors_.reserve(event_capacity);
  batch_->payload_.reserve(payload_capacity);
  return BatchBuildStatus::success();
}

BatchBuildStatus VectorBatchBuilder::check_sequence(EventSeq sequence) const {
  if (has_sequence_ && sequence <= last_sequence_) {
    return BatchBuildStatus::failure(
        BatchBuildErrorCode::NonIncreasingEventSequence);
  }
  return BatchBuildStatus::success();
}

void VectorBatchBuilder::accept_sequence(EventSeq sequence) noexcept {
  last_sequence_ = sequence;
  has_sequence_  = true;
}

BatchBuildStatus VectorBatchBuilder::append_read(EventSeq sequence,
                                                 ByteView key,
                                                 TaggedBytes result,
                                                 ReadSource source) {
  if (batch_ == nullptr) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::AlreadySealed);
  }
  if (source != ReadSource::Snapshot && source != ReadSource::Local) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::InvalidReadSource);
  }
  if (auto status = check_sequence(sequence); !status.ok()) {
    return status;
  }
  if (source == ReadSource::Local) {
    // Local/read-own-write probes are deliberately absent from sealed V1.
    accept_sequence(sequence);
    return BatchBuildStatus::success();
  }
  if (batch_->descriptors_.size() ==
          std::numeric_limits<std::uint32_t>::max() ||
      read_count_ == std::numeric_limits<std::uint32_t>::max()) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::EventCountOverflow);
  }
  auto status =
      append_vector_record(batch_->descriptors_, batch_->payload_, sequence,
                           PackedEventOp::Read, key, result);
  if (!status.ok()) {
    return status;
  }
  ++read_count_;
  accept_sequence(sequence);
  return BatchBuildStatus::success();
}

BatchBuildStatus VectorBatchBuilder::append_write(EventSeq sequence,
                                                  ByteView key,
                                                  TaggedBytes result) {
  if (batch_ == nullptr) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::AlreadySealed);
  }
  if (auto status = check_sequence(sequence); !status.ok()) {
    return status;
  }
  if (batch_->descriptors_.size() ==
          std::numeric_limits<std::uint32_t>::max() ||
      write_count_ == std::numeric_limits<std::uint32_t>::max()) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::EventCountOverflow);
  }
  auto status =
      append_vector_record(batch_->descriptors_, batch_->payload_, sequence,
                           PackedEventOp::Write, key, result);
  if (!status.ok()) {
    return status;
  }
  ++write_count_;
  accept_sequence(sequence);
  return BatchBuildStatus::success();
}

VectorBatchSealResult VectorBatchBuilder::seal() {
  if (batch_ == nullptr) {
    return VectorBatchSealResult{
        nullptr, BatchBuildError{BatchBuildErrorCode::AlreadySealed},
        std::nullopt};
  }

  batch_->descriptor_view_count_ = batch_->descriptors_.empty() ? 0 : 1;
  if (!batch_->descriptors_.empty()) {
    batch_->descriptor_views_[0] =
        std::as_bytes(std::span<const EventDescriptorV1>(batch_->descriptors_));
  }
  batch_->payload_view_count_ = batch_->payload_.empty() ? 0 : 1;
  if (!batch_->payload_.empty()) {
    batch_->payload_views_[0] = ByteView(batch_->payload_);
  }

  encode_header(batch_->header_, meta_,
                checked_u32_size(batch_->descriptors_.size()), read_count_,
                write_count_, checked_u32_size(batch_->descriptor_view_count_),
                checked_u32_size(batch_->payload_view_count_));
  auto validation = validate_packed_batch(batch_->input());
  if (auto *error = std::get_if<BatchValidationError>(&validation)) {
    return VectorBatchSealResult{nullptr, std::nullopt, *error};
  }
  batch_->view_ = std::get<ValidatedTxnView>(std::move(validation));
  return VectorBatchSealResult{std::move(batch_), std::nullopt, std::nullopt};
}

ChunkPool::ChunkPool(std::size_t ordinary_segment_size,
                     std::size_t max_cached_ordinary_segments)
    : ordinary_segment_size_(ordinary_segment_size),
      max_cached_ordinary_segments_(max_cached_ordinary_segments) {
  if (ordinary_segment_size < sizeof(EventDescriptorV1) ||
      ordinary_segment_size > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(
        "chunk size must fit BlobRefV1 and one EventDescriptorV1");
  }
}

ChunkPool::SegmentPtr ChunkPool::acquire(std::size_t minimum_capacity,
                                         bool dedicated) {
  std::lock_guard lock(mutex_);
  ++acquisitions_;
  if (!dedicated && !free_ordinary_segments_.empty()) {
    auto segment = std::move(free_ordinary_segments_.back());
    free_ordinary_segments_.pop_back();
    ++reuses_;
    return segment;
  }

  ++allocations_;
  const auto capacity = dedicated ? minimum_capacity : ordinary_segment_size_;
  return std::make_unique<Segment>(capacity, dedicated);
}

void ChunkPool::recycle(SegmentPtr segment) noexcept {
  if (segment == nullptr) {
    return;
  }
  std::lock_guard lock(mutex_);
  ++recycles_;
  if (!segment->dedicated && segment->bytes.size() == ordinary_segment_size_ &&
      free_ordinary_segments_.size() < max_cached_ordinary_segments_) {
    // Recycling is called from ChunkBatch's destructor and is noexcept. A
    // cache-vector growth failure must simply drop this optional cached
    // segment, not terminate the database process while unwinding.
    try {
      free_ordinary_segments_.push_back(std::move(segment));
    } catch (const std::bad_alloc &) {
      // `segment` still owns the allocation when vector growth fails. Let it
      // be released at function exit; recycling is an optimization only.
    } catch (const std::length_error &) {
      // Treat an exhausted cache index identically to a full cache.
    }
  }
}

ChunkPoolStats ChunkPool::stats() const {
  std::lock_guard lock(mutex_);
  return ChunkPoolStats{
      .acquisitions             = acquisitions_,
      .allocations              = allocations_,
      .reuses                   = reuses_,
      .recycles                 = recycles_,
      .cached_ordinary_segments = free_ordinary_segments_.size(),
  };
}

ChunkBatch::~ChunkBatch() {
  recycle_segments(descriptor_segments_);
  recycle_segments(payload_segments_);
}

void ChunkBatch::recycle_segments(
    std::vector<OwnedSegment> &segments) noexcept {
  if (pool_ != nullptr) {
    for (auto &segment : segments) {
      pool_->recycle(std::move(segment.segment));
    }
  }
  segments.clear();
}

const ValidatedTxnView &ChunkBatch::view() const noexcept {
  assert(view_.has_value());
  return *view_;
}

PackedBatchInput ChunkBatch::input() const noexcept {
  return PackedBatchInput{header_bytes(header_), descriptor_views_,
                          payload_views_};
}

std::size_t ChunkBatch::used_bytes() const noexcept {
  std::size_t total = sizeof(header_);
  for (const auto &segment : descriptor_segments_) {
    total += segment.used;
  }
  for (const auto &segment : payload_segments_) {
    total += segment.used;
  }
  return total;
}

std::size_t ChunkBatch::reserved_bytes() const noexcept {
  std::size_t total = sizeof(header_);
  for (const auto &segment : descriptor_segments_) {
    total += segment.segment->bytes.size();
  }
  for (const auto &segment : payload_segments_) {
    total += segment.segment->bytes.size();
  }
  return total;
}

namespace {
std::atomic<std::uint64_t> next_builder_cookie{1};
}

ChunkBatchBuilder::ChunkBatchBuilder(std::shared_ptr<ChunkPool> pool,
                                     OrderedTxnMeta meta)
    : pool_(std::move(pool)), meta_(std::move(meta)),
      builder_cookie_(
          next_builder_cookie.fetch_add(1, std::memory_order_relaxed)) {
  if (pool_ == nullptr) {
    throw std::invalid_argument("ChunkBatchBuilder requires a ChunkPool");
  }
  meta_.commit_seq = kUnassignedCommitSeq;
  if (builder_cookie_ == 0) {
    builder_cookie_ =
        next_builder_cookie.fetch_add(1, std::memory_order_relaxed);
  }
  batch_.reset(new ChunkBatch(pool_));
}

BatchBuildStatus ChunkBatchBuilder::check_sequence(EventSeq sequence) const {
  if (has_sequence_ && sequence <= last_sequence_) {
    return BatchBuildStatus::failure(
        BatchBuildErrorCode::NonIncreasingEventSequence);
  }
  return BatchBuildStatus::success();
}

void ChunkBatchBuilder::accept_sequence(EventSeq sequence) noexcept {
  last_sequence_ = sequence;
  has_sequence_  = true;
}

void ChunkBatchBuilder::record_lineage() {
  auto token = next_lineage_token_++;
  if (token == 0) {
    token = next_lineage_token_++;
  }
  lineage_.push_back(token);
}

ChunkCheckpoint ChunkBatchBuilder::checkpoint() const noexcept {
  ChunkCheckpoint checkpoint;
  if (batch_ == nullptr) {
    return checkpoint;
  }
  checkpoint.builder_cookie_           = builder_cookie_;
  checkpoint.descriptor_segment_count_ = batch_->descriptor_segments_.size();
  checkpoint.descriptor_tail_used_ =
      batch_->descriptor_segments_.empty()
          ? 0
          : batch_->descriptor_segments_.back().used;
  checkpoint.payload_segment_count_ = batch_->payload_segments_.size();
  checkpoint.payload_tail_used_     = batch_->payload_segments_.empty()
                                          ? 0
                                          : batch_->payload_segments_.back().used;
  checkpoint.read_count_            = read_count_;
  checkpoint.write_count_           = write_count_;
  checkpoint.last_sequence_         = last_sequence_;
  checkpoint.has_sequence_          = has_sequence_;
  checkpoint.lineage_size_          = lineage_.size();
  checkpoint.lineage_token_         = lineage_.empty() ? 0 : lineage_.back();
  return checkpoint;
}

BatchBuildStatus
ChunkBatchBuilder::rollback(const ChunkCheckpoint &checkpoint_value) {
  if (batch_ == nullptr ||
      checkpoint_value.builder_cookie_ != builder_cookie_ ||
      checkpoint_value.builder_cookie_ == 0 ||
      checkpoint_value.descriptor_segment_count_ >
          batch_->descriptor_segments_.size() ||
      checkpoint_value.payload_segment_count_ >
          batch_->payload_segments_.size() ||
      checkpoint_value.read_count_ > read_count_ ||
      checkpoint_value.write_count_ > write_count_ ||
      checkpoint_value.lineage_size_ > lineage_.size() ||
      (checkpoint_value.lineage_size_ == 0 &&
       checkpoint_value.lineage_token_ != 0) ||
      (checkpoint_value.lineage_size_ != 0 &&
       lineage_[checkpoint_value.lineage_size_ - 1] !=
           checkpoint_value.lineage_token_)) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::InvalidCheckpoint);
  }
  if ((checkpoint_value.descriptor_segment_count_ == 0 &&
       checkpoint_value.descriptor_tail_used_ != 0) ||
      (checkpoint_value.payload_segment_count_ == 0 &&
       checkpoint_value.payload_tail_used_ != 0) ||
      (checkpoint_value.descriptor_tail_used_ % kDescriptorSize) != 0) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::InvalidCheckpoint);
  }
  if (checkpoint_value.descriptor_segment_count_ != 0) {
    const auto &tail =
        batch_
            ->descriptor_segments_[checkpoint_value.descriptor_segment_count_ -
                                   1];
    if (checkpoint_value.descriptor_tail_used_ > tail.used) {
      return BatchBuildStatus::failure(BatchBuildErrorCode::InvalidCheckpoint);
    }
  }
  if (checkpoint_value.payload_segment_count_ != 0) {
    const auto &tail =
        batch_->payload_segments_[checkpoint_value.payload_segment_count_ - 1];
    if (checkpoint_value.payload_tail_used_ > tail.used) {
      return BatchBuildStatus::failure(BatchBuildErrorCode::InvalidCheckpoint);
    }
  }

  restore(checkpoint_value);
  return BatchBuildStatus::success();
}

void ChunkBatchBuilder::restore(
    const ChunkCheckpoint &checkpoint_value) noexcept {
  while (batch_->descriptor_segments_.size() >
         checkpoint_value.descriptor_segment_count_) {
    pool_->recycle(std::move(batch_->descriptor_segments_.back().segment));
    batch_->descriptor_segments_.pop_back();
  }
  if (!batch_->descriptor_segments_.empty()) {
    auto &tail = batch_->descriptor_segments_.back();
    std::fill(
        tail.segment->bytes.begin() +
            static_cast<std::ptrdiff_t>(checkpoint_value.descriptor_tail_used_),
        tail.segment->bytes.begin() + static_cast<std::ptrdiff_t>(tail.used),
        std::byte{0});
    tail.used = checkpoint_value.descriptor_tail_used_;
  }

  while (batch_->payload_segments_.size() >
         checkpoint_value.payload_segment_count_) {
    pool_->recycle(std::move(batch_->payload_segments_.back().segment));
    batch_->payload_segments_.pop_back();
  }
  if (!batch_->payload_segments_.empty()) {
    batch_->payload_segments_.back().used = checkpoint_value.payload_tail_used_;
  }

  read_count_    = checkpoint_value.read_count_;
  write_count_   = checkpoint_value.write_count_;
  last_sequence_ = checkpoint_value.last_sequence_;
  has_sequence_  = checkpoint_value.has_sequence_;
  lineage_.resize(checkpoint_value.lineage_size_);
}

BatchBuildStatus ChunkBatchBuilder::ensure_payload_space(std::size_t bytes) {
  assert(batch_ != nullptr);
  assert(bytes <= pool_->ordinary_segment_size());
  if (!batch_->payload_segments_.empty()) {
    const auto &tail = batch_->payload_segments_.back();
    if (!tail.segment->dedicated &&
        tail.segment->bytes.size() - tail.used >= bytes) {
      return BatchBuildStatus::success();
    }
  }
  if (batch_->payload_segments_.size() >= kNullSegment) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::PayloadSegmentLimit);
  }

  auto segment = pool_->acquire(pool_->ordinary_segment_size(), false);
  try {
    batch_->payload_segments_.push_back(
        ChunkBatch::OwnedSegment{std::move(segment), 0});
  } catch (...) {
    pool_->recycle(std::move(segment));
    throw;
  }
  return BatchBuildStatus::success();
}

BatchBuildStatus ChunkBatchBuilder::store_blob(ByteView bytes,
                                               StoredBlobRef &ref) {
  ref         = {};
  ref.segment = kNullSegment;
  if (bytes.empty()) {
    return BatchBuildStatus::success();
  }
  if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::BlobTooLarge);
  }

  if (bytes.size() > pool_->ordinary_segment_size()) {
    if (batch_->payload_segments_.size() >= kNullSegment) {
      return BatchBuildStatus::failure(
          BatchBuildErrorCode::PayloadSegmentLimit);
    }
    auto segment = pool_->acquire(bytes.size(), true);
    std::memcpy(segment->bytes.data(), bytes.data(), bytes.size());
    const auto segment_index =
        static_cast<std::uint16_t>(batch_->payload_segments_.size());
    try {
      batch_->payload_segments_.push_back(
          ChunkBatch::OwnedSegment{std::move(segment), bytes.size()});
    } catch (...) {
      pool_->recycle(std::move(segment));
      throw;
    }
    ref.offset  = 0;
    ref.size    = static_cast<std::uint32_t>(bytes.size());
    ref.segment = segment_index;
    return BatchBuildStatus::success();
  }

  if (auto status = ensure_payload_space(bytes.size()); !status.ok()) {
    return status;
  }
  auto &tail = batch_->payload_segments_.back();
  const auto segment_index =
      static_cast<std::uint16_t>(batch_->payload_segments_.size() - 1);
  const auto offset = tail.used;
  std::memcpy(tail.segment->bytes.data() + offset, bytes.data(), bytes.size());
  tail.used += bytes.size();
  ref.offset  = static_cast<std::uint32_t>(offset);
  ref.size    = static_cast<std::uint32_t>(bytes.size());
  ref.segment = segment_index;
  return BatchBuildStatus::success();
}

BatchBuildStatus ChunkBatchBuilder::store_event_payloads(
    ByteView key, const std::optional<ByteView> &present_value,
    StoredBlobRef &key_ref, StoredBlobRef &value_ref) {
  key_ref           = {};
  key_ref.segment   = kNullSegment;
  value_ref         = {};
  value_ref.segment = kNullSegment;

  if (key.size() > std::numeric_limits<std::uint32_t>::max() ||
      (present_value.has_value() &&
       present_value->size() > std::numeric_limits<std::uint32_t>::max())) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::BlobTooLarge);
  }

  const auto value_size = present_value.has_value() ? present_value->size() : 0;
  const auto total_size = static_cast<std::uint64_t>(key.size()) +
                          static_cast<std::uint64_t>(value_size);
  if (total_size != 0 && total_size <= pool_->ordinary_segment_size()) {
    if (auto status =
            ensure_payload_space(static_cast<std::size_t>(total_size));
        !status.ok()) {
      return status;
    }
    auto &tail = batch_->payload_segments_.back();
    const auto segment_index =
        static_cast<std::uint16_t>(batch_->payload_segments_.size() - 1);
    if (!key.empty()) {
      const auto offset = tail.used;
      std::memcpy(tail.segment->bytes.data() + offset, key.data(), key.size());
      tail.used += key.size();
      key_ref =
          StoredBlobRef{static_cast<std::uint32_t>(offset),
                        static_cast<std::uint32_t>(key.size()), segment_index};
    }
    if (present_value.has_value() && !present_value->empty()) {
      const auto offset = tail.used;
      std::memcpy(tail.segment->bytes.data() + offset, present_value->data(),
                  present_value->size());
      tail.used += present_value->size();
      value_ref = StoredBlobRef{
          static_cast<std::uint32_t>(offset),
          static_cast<std::uint32_t>(present_value->size()), segment_index};
    }
    return BatchBuildStatus::success();
  }

  if (auto status = store_blob(key, key_ref); !status.ok()) {
    return status;
  }
  if (present_value.has_value()) {
    return store_blob(*present_value, value_ref);
  }
  return BatchBuildStatus::success();
}

BatchBuildStatus ChunkBatchBuilder::ensure_descriptor_space() {
  assert(batch_ != nullptr);
  if (!batch_->descriptor_segments_.empty()) {
    const auto &tail = batch_->descriptor_segments_.back();
    if (tail.segment->bytes.size() - tail.used >= kDescriptorSize) {
      return BatchBuildStatus::success();
    }
  }
  if (batch_->descriptor_segments_.size() ==
      std::numeric_limits<std::uint32_t>::max()) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::DescriptorSpanLimit);
  }
  auto segment = pool_->acquire(pool_->ordinary_segment_size(), false);
  try {
    batch_->descriptor_segments_.push_back(
        ChunkBatch::OwnedSegment{std::move(segment), 0});
  } catch (...) {
    pool_->recycle(std::move(segment));
    throw;
  }
  return BatchBuildStatus::success();
}

BatchBuildStatus ChunkBatchBuilder::append_descriptor(
    EventSeq sequence, PackedEventOp op, PackedValueState state,
    const StoredBlobRef &key_ref, const StoredBlobRef &value_ref) {
  if (auto status = ensure_descriptor_space(); !status.ok()) {
    return status;
  }
  auto &tail      = batch_->descriptor_segments_.back();
  auto descriptor = std::span<std::byte>(tail.segment->bytes)
                        .subspan(tail.used, kDescriptorSize);
  std::fill(descriptor.begin(), descriptor.end(), std::byte{0});
  write_u32_le(descriptor, offsetof(EventDescriptorV1, sequence_le), sequence);
  encode_blob_ref(descriptor, offsetof(EventDescriptorV1, key), key_ref);
  encode_blob_ref(descriptor, offsetof(EventDescriptorV1, value), value_ref);
  descriptor[offsetof(EventDescriptorV1, value_state)] =
      static_cast<std::byte>(state);
  descriptor[offsetof(EventDescriptorV1, op)] = static_cast<std::byte>(op);
  tail.used += kDescriptorSize;
  return BatchBuildStatus::success();
}

BatchBuildStatus
ChunkBatchBuilder::append_normalized_event(EventSeq sequence, PackedEventOp op,
                                           ByteView key, TaggedBytes result) {
  if (static_cast<std::uint64_t>(read_count_) +
          static_cast<std::uint64_t>(write_count_) ==
      std::numeric_limits<std::uint32_t>::max()) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::EventCountOverflow);
  }
  if ((op == PackedEventOp::Read &&
       read_count_ == std::numeric_limits<std::uint32_t>::max()) ||
      (op == PackedEventOp::Write &&
       write_count_ == std::numeric_limits<std::uint32_t>::max())) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::EventCountOverflow);
  }

  const auto rollback_point = checkpoint();
  record_lineage();
  const auto state = std::holds_alternative<PresentBytes>(result)
                         ? PackedValueState::Present
                         : PackedValueState::Absent;
  const std::optional<ByteView> present_value =
      std::holds_alternative<PresentBytes>(result)
          ? std::optional<ByteView>(std::get<PresentBytes>(result).bytes)
          : std::nullopt;
  StoredBlobRef key_ref;
  StoredBlobRef value_ref;
  try {
    auto status = store_event_payloads(key, present_value, key_ref, value_ref);
    if (!status.ok()) {
      restore(rollback_point);
      return status;
    }
    status = append_descriptor(sequence, op, state, key_ref, value_ref);
    if (!status.ok()) {
      restore(rollback_point);
      return status;
    }
  } catch (...) {
    restore(rollback_point);
    throw;
  }

  if (op == PackedEventOp::Read) {
    ++read_count_;
  } else {
    ++write_count_;
  }
  return BatchBuildStatus::success();
}

BatchBuildStatus ChunkBatchBuilder::append_read(EventSeq sequence, ByteView key,
                                                TaggedBytes result,
                                                ReadSource source) {
  if (batch_ == nullptr) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::AlreadySealed);
  }
  if (source != ReadSource::Snapshot && source != ReadSource::Local) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::InvalidReadSource);
  }
  if (auto status = check_sequence(sequence); !status.ok()) {
    return status;
  }
  if (source == ReadSource::Local) {
    record_lineage();
    accept_sequence(sequence);
    return BatchBuildStatus::success();
  }
  auto status = append_normalized_event(sequence, PackedEventOp::Read, key,
                                        std::move(result));
  if (status.ok()) {
    accept_sequence(sequence);
  }
  return status;
}

BatchBuildStatus ChunkBatchBuilder::append_write(EventSeq sequence,
                                                 ByteView key,
                                                 TaggedBytes result) {
  if (batch_ == nullptr) {
    return BatchBuildStatus::failure(BatchBuildErrorCode::AlreadySealed);
  }
  if (auto status = check_sequence(sequence); !status.ok()) {
    return status;
  }
  auto status = append_normalized_event(sequence, PackedEventOp::Write, key,
                                        std::move(result));
  if (status.ok()) {
    accept_sequence(sequence);
  }
  return status;
}

void ChunkBatchBuilder::abort() noexcept { batch_.reset(); }

ChunkBatchSealResult ChunkBatchBuilder::seal() {
  if (batch_ == nullptr) {
    return ChunkBatchSealResult{
        nullptr, BatchBuildError{BatchBuildErrorCode::AlreadySealed},
        std::nullopt};
  }
  if (batch_->descriptor_segments_.size() >
      std::numeric_limits<std::uint32_t>::max()) {
    return ChunkBatchSealResult{
        nullptr, BatchBuildError{BatchBuildErrorCode::DescriptorSpanLimit},
        std::nullopt};
  }
  if (batch_->payload_segments_.size() > kNullSegment) {
    return ChunkBatchSealResult{
        nullptr, BatchBuildError{BatchBuildErrorCode::PayloadSegmentLimit},
        std::nullopt};
  }

  batch_->descriptor_views_.clear();
  batch_->descriptor_views_.reserve(batch_->descriptor_segments_.size());
  for (const auto &segment : batch_->descriptor_segments_) {
    assert(segment.used != 0);
    batch_->descriptor_views_.emplace_back(segment.segment->bytes.data(),
                                           segment.used);
  }
  batch_->payload_views_.clear();
  batch_->payload_views_.reserve(batch_->payload_segments_.size());
  for (const auto &segment : batch_->payload_segments_) {
    assert(segment.used != 0);
    batch_->payload_views_.emplace_back(segment.segment->bytes.data(),
                                        segment.used);
  }

  const auto event_count = static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(read_count_) + write_count_);
  encode_header(batch_->header_, meta_, event_count, read_count_, write_count_,
                checked_u32_size(batch_->descriptor_views_.size()),
                checked_u32_size(batch_->payload_views_.size()));
  auto validation = validate_packed_batch(batch_->input());
  if (auto *error = std::get_if<BatchValidationError>(&validation)) {
    return ChunkBatchSealResult{nullptr, std::nullopt, *error};
  }
  batch_->view_ = std::get<ValidatedTxnView>(std::move(validation));
  return ChunkBatchSealResult{std::move(batch_), std::nullopt, std::nullopt};
}

} // namespace co_checker::online
