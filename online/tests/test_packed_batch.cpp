#include "co_checker/online/packed_batch.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace co_checker::online;

static_assert(std::ranges::input_range<ValidatedTxnView::EventRange>);
static_assert(
    std::same_as<decltype(std::declval<const ValidatedTxnView &>().meta()),
                 const OrderedTxnMeta &>);

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view expression, const char *file,
           int line) {
  if (condition) {
    return;
  }
  std::ostringstream message;
  message << file << ':' << line << ": check failed: " << expression;
  throw TestFailure(message.str());
}

#define CHECK(expression)                                                      \
  check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

[[nodiscard]] std::vector<std::byte> bytes(std::string_view text) {
  std::vector<std::byte> result;
  result.reserve(text.size());
  for (const auto character : text) {
    result.push_back(
        static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  return result;
}

[[nodiscard]] std::string hex(ByteView value) {
  constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(value.size() * 2);
  for (const auto byte : value) {
    const auto number = std::to_integer<std::uint8_t>(byte);
    result.push_back(digits[number >> 4U]);
    result.push_back(digits[number & 0x0fU]);
  }
  return result;
}

[[nodiscard]] OrderedTxnMeta sample_meta(bool tie_breaker = true) {
  OrderedTxnMeta meta;
  meta.commit_seq           = 0x0102030405060708ULL;
  meta.transaction_id.words = {0x1112131415161718ULL, 0x2122232425262728ULL};
  meta.attempt_id.words     = {0x3132333435363738ULL, 0x4142434445464748ULL};
  meta.session_id.runtime_epoch.words   = {0x5152535455565758ULL,
                                           0x6162636465666768ULL};
  meta.session_id.producer_id           = 0x71727374U;
  meta.producer_id                      = 0x81828384U;
  meta.session_id.local_session_counter = 0x9192939495969798ULL;
  meta.session_seq                      = 0xa1a2a3a4a5a6a7a8ULL;
  meta.commit_token.cluster_epoch       = 0xb1b2b3b4b5b6b7b8ULL;
  meta.commit_token.timestamp           = 0xc1c2c3c4c5c6c7c8ULL;
  if (tie_breaker) {
    meta.commit_token.authoritative_tie_breaker = 0xd1d2d3d4d5d6d7d8ULL;
  }
  meta.producer_seq = 0xe1e2e3e4e5e6e7e8ULL;
  return meta;
}

[[nodiscard]] bool same_meta(const OrderedTxnMeta &lhs,
                             const OrderedTxnMeta &rhs) {
  return lhs.commit_seq == rhs.commit_seq &&
         lhs.transaction_id == rhs.transaction_id &&
         lhs.attempt_id == rhs.attempt_id && lhs.session_id == rhs.session_id &&
         lhs.session_seq == rhs.session_seq &&
         lhs.commit_token == rhs.commit_token &&
         lhs.producer_id == rhs.producer_id &&
         lhs.producer_seq == rhs.producer_seq;
}

[[nodiscard]] OrderedTxnMeta captured_meta(OrderedTxnMeta meta) {
  meta.commit_seq = kUnassignedCommitSeq;
  return meta;
}

struct OwnedEvent {
  EventSeq sequence = 0;
  EventOp op        = EventOp::Read;
  ValueState state  = ValueState::Absent;
  std::vector<std::byte> key;
  std::vector<std::byte> value;

  friend bool operator==(const OwnedEvent &, const OwnedEvent &) = default;
};

[[nodiscard]] std::vector<OwnedEvent>
collect_events(const ValidatedTxnView &view) {
  std::vector<OwnedEvent> result;
  result.reserve(view.event_count());
  for (const auto event : view.events()) {
    result.push_back(OwnedEvent{
        event.sequence,
        event.op,
        event.value_state,
        std::vector<std::byte>(event.key.begin(), event.key.end()),
        std::vector<std::byte>(event.value.begin(), event.value.end()),
    });
  }
  return result;
}

void require_status(const BatchBuildStatus &status) { CHECK(status.ok()); }

struct MutableBatch {
  std::vector<std::byte> header;
  std::vector<std::vector<std::byte>> descriptors;
  std::vector<std::vector<std::byte>> payloads;
  std::vector<ByteView> descriptor_views;
  std::vector<ByteView> payload_views;

  explicit MutableBatch(const PackedBatchInput &input)
      : header(input.header.begin(), input.header.end()) {
    for (const auto descriptor : input.descriptor_spans) {
      descriptors.emplace_back(descriptor.begin(), descriptor.end());
    }
    for (const auto payload : input.payload_segments) {
      payloads.emplace_back(payload.begin(), payload.end());
    }
    refresh();
  }

  MutableBatch(const MutableBatch &other)
      : header(other.header), descriptors(other.descriptors),
        payloads(other.payloads) {
    refresh();
  }

  MutableBatch &operator=(const MutableBatch &other) {
    if (this != &other) {
      header      = other.header;
      descriptors = other.descriptors;
      payloads    = other.payloads;
      refresh();
    }
    return *this;
  }

  void refresh() {
    descriptor_views.clear();
    for (const auto &descriptor : descriptors) {
      descriptor_views.emplace_back(descriptor);
    }
    payload_views.clear();
    for (const auto &payload : payloads) {
      payload_views.emplace_back(payload);
    }
  }

  [[nodiscard]] PackedBatchInput input() const {
    return PackedBatchInput{header, descriptor_views, payload_views};
  }
};

void put_u16(std::vector<std::byte> &target, std::size_t offset,
             std::uint16_t value) {
  for (std::size_t index = 0; index < 2; ++index) {
    target[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned>(index * 8U)));
  }
}

void put_u32(std::vector<std::byte> &target, std::size_t offset,
             std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    target[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned>(index * 8U)));
  }
}

void put_u64(std::vector<std::byte> &target, std::size_t offset,
             std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    target[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned>(index * 8U)));
  }
}

void expect_error(const PackedBatchInput &input,
                  BatchValidationErrorCode expected,
                  std::optional<std::uint64_t> event_index = std::nullopt) {
  for (const auto mode : supported_packed_validation_modes()) {
    const auto result = validate_packed_batch_with_mode(input, mode);
    const auto *error = std::get_if<BatchValidationError>(&result);
    CHECK(error != nullptr);
    CHECK(error->code == expected);
    CHECK(error->event_index == event_index);
  }
}

void require_validation_mode_agreement(const PackedBatchInput &input) {
  const auto scalar =
      validate_packed_batch_with_mode(input, PackedValidationMode::Scalar);
  for (const auto mode : supported_packed_validation_modes()) {
    const auto actual = validate_packed_batch_with_mode(input, mode);
    CHECK(actual.index() == scalar.index());
    if (const auto *expected_error =
            std::get_if<BatchValidationError>(&scalar)) {
      const auto *actual_error = std::get_if<BatchValidationError>(&actual);
      CHECK(actual_error != nullptr);
      CHECK(*actual_error == *expected_error);
    } else {
      const auto *expected_view = std::get_if<ValidatedTxnView>(&scalar);
      const auto *actual_view   = std::get_if<ValidatedTxnView>(&actual);
      CHECK(expected_view != nullptr);
      CHECK(actual_view != nullptr);
      CHECK(same_meta(actual_view->meta(), expected_view->meta()));
      CHECK(collect_events(*actual_view) == collect_events(*expected_view));
    }
  }
}

void test_layout_and_empty_batches() {
  static_assert(sizeof(BlobRefV1) == 12);
  static_assert(sizeof(EventDescriptorV1) == 32);
  static_assert(sizeof(BatchHeaderV1) == 168);

  VectorBatchBuilder vector_builder(sample_meta(false));
  auto vector_result = vector_builder.seal();
  CHECK(vector_result.ok());
  CHECK(same_meta(vector_result.batch->view().meta(),
                  captured_meta(sample_meta(false))));
  CHECK(vector_result.batch->view().event_count() == 0);
  CHECK(vector_result.batch->view().events().begin() ==
        vector_result.batch->view().events().end());
  CHECK(vector_result.batch->input().descriptor_spans.empty());
  CHECK(vector_result.batch->input().payload_segments.empty());
  CHECK(std::holds_alternative<ValidatedTxnView>(
      validate_packed_batch(vector_result.batch->input())));

  auto pool = std::make_shared<ChunkPool>(64);
  ChunkBatchBuilder chunk_builder(pool, sample_meta(false));
  auto chunk_result = chunk_builder.seal();
  CHECK(chunk_result.ok());
  CHECK(same_meta(chunk_result.batch->view().meta(),
                  captured_meta(sample_meta(false))));
  CHECK(chunk_result.batch->view().event_count() == 0);
  CHECK(chunk_result.batch->input().descriptor_spans.empty());
  CHECK(chunk_result.batch->input().payload_segments.empty());
}

void test_tagged_values_local_filter_and_zero_copy() {
  const auto key_a       = bytes("alpha");
  const auto key_b       = bytes("beta");
  const auto key_local   = bytes("local");
  const auto key_c       = bytes("gamma");
  const auto key_d       = bytes("delta");
  const auto local_value = bytes("not-published");

  VectorBatchBuilder builder(sample_meta());
  require_status(builder.reserve(1, 1));
  require_status(
      builder.append_read(1, key_a, PresentBytes{{}}, ReadSource::Snapshot));
  require_status(builder.append_read(2, key_b, Absent{}, ReadSource::Snapshot));
  require_status(builder.append_read(3, key_local, PresentBytes{local_value},
                                     ReadSource::Local));
  require_status(builder.append_write(4, key_c, PresentBytes{{}}));
  require_status(builder.append_write(5, key_d, Absent{}));

  auto sealed = builder.seal();
  CHECK(sealed.ok());
  const auto &view = sealed.batch->view();
  CHECK(view.event_count() == 4);
  CHECK(view.read_count() == 2);
  CHECK(view.write_count() == 2);

  const auto events = collect_events(view);
  CHECK(events.size() == 4);
  CHECK(events[0].sequence == 1);
  CHECK(events[0].state == ValueState::Present);
  CHECK(events[0].value.empty());
  CHECK(events[1].sequence == 2);
  CHECK(events[1].state == ValueState::Absent);
  CHECK(events[1].value.empty());
  CHECK(events[2].sequence == 4);
  CHECK(events[2].op == EventOp::Write);
  CHECK(events[2].state == ValueState::Present);
  CHECK(events[2].value.empty());
  CHECK(events[3].sequence == 5);
  CHECK(events[3].state == ValueState::Absent);
  CHECK(std::none_of(events.begin(), events.end(), [&](const auto &event) {
    return event.key == key_local;
  }));

  const auto input = sealed.batch->input();
  CHECK(reinterpret_cast<std::uintptr_t>(input.descriptor_spans[0].data()) %
            64U ==
        0U);
  CHECK(input.payload_segments.size() == 1);
  const auto first = *view.events().begin();
  CHECK(first.key.data() == input.payload_segments[0].data());

  const auto after_seal = builder.append_write(6, key_a, Absent{});
  CHECK(!after_seal.ok());
  CHECK(after_seal.error->code == BatchBuildErrorCode::AlreadySealed);
  const auto second_seal = builder.seal();
  CHECK(!second_seal.ok());
  CHECK(second_seal.build_error->code == BatchBuildErrorCode::AlreadySealed);
}

void test_golden_little_endian_encoding() {
  const std::array<std::byte, 2> key{std::byte{0xaa}, std::byte{0xbb}};
  const std::array<std::byte, 3> value{std::byte{0xcc}, std::byte{0xdd},
                                       std::byte{0xee}};

  VectorBatchBuilder builder(sample_meta());
  require_status(builder.append_read(0x01020304U, key, PresentBytes{value},
                                     ReadSource::Snapshot));
  auto sealed = builder.seal();
  CHECK(sealed.ok());
  const auto input = sealed.batch->input();
  CHECK(input.descriptor_spans.size() == 1);
  CHECK(input.payload_segments.size() == 1);

  constexpr std::string_view expected_header =
      "434f424154434831"  // magic: COBATCH1
      "0100"              // version
      "a800"              // header size: 168
      "2000"              // descriptor size: 32
      "0c00"              // blob ref size: 12
      "0000000000000000"  // feature bits
      "01000000"          // tie-breaker metadata flag
      "00000000"          // reserved0
      "01000000"          // event count
      "01000000"          // read count
      "00000000"          // write count
      "01000000"          // descriptor span count
      "01000000"          // payload segment count
      "00000000"          // reserved1
      "ffffffffffffffff"  // unassigned commit-sequence sentinel
      "1817161514131211"  // transaction uid word 0
      "2827262524232221"  // transaction uid word 1
      "3837363534333231"  // attempt uid word 0
      "4847464544434241"  // attempt uid word 1
      "5857565554535251"  // runtime epoch word 0
      "6867666564636261"  // runtime epoch word 1
      "74737271"          // session producer id
      "84838281"          // producer id
      "9897969594939291"  // local session counter
      "a8a7a6a5a4a3a2a1"  // session sequence
      "b8b7b6b5b4b3b2b1"  // commit-token cluster epoch
      "c8c7c6c5c4c3c2c1"  // commit-token timestamp
      "d8d7d6d5d4d3d2d1"  // authoritative tie-breaker
      "e8e7e6e5e4e3e2e1"; // producer sequence
  constexpr std::string_view expected_descriptor = "04030201" // sequence
                                                   "01"       // read
                                                   "01"       // present
                                                   "00"       // flags
                                                   "00"       // reserved
                                                   "00000000" // key offset
                                                   "02000000" // key size
                                                   "0000"     // key segment
                                                   "0000"     // key reserved
                                                   "02000000" // value offset
                                                   "03000000" // value size
                                                   "0000"     // value segment
                                                   "0000";    // value reserved

  CHECK(hex(input.header) == expected_header);
  CHECK(hex(input.descriptor_spans[0]) == expected_descriptor);
  CHECK(hex(input.payload_segments[0]) == "aabbccddee");
  CHECK(same_meta(sealed.batch->view().meta(), captured_meta(sample_meta())));

  auto ordered_meta       = sample_meta();
  ordered_meta.commit_seq = 42;
  const auto ordered      = sealed.batch->view().with_meta(ordered_meta);
  CHECK(same_meta(ordered.meta(), ordered_meta));
  CHECK((*ordered.events().begin()).key.data() ==
        (*sealed.batch->view().events().begin()).key.data());
  const auto ordered_by_sequence = sealed.batch->view().with_commit_seq(43);
  CHECK(ordered_by_sequence.meta().commit_seq == 43);
  CHECK((*ordered_by_sequence.events().begin()).value.data() ==
        (*sealed.batch->view().events().begin()).value.data());
}

std::unique_ptr<VectorBatch> make_malformed_seed() {
  const auto key1   = bytes("key-one");
  const auto key2   = bytes("key-two");
  const auto value1 = bytes("value-one");
  const auto value2 = bytes("value-two");
  VectorBatchBuilder builder(sample_meta());
  require_status(builder.append_read(10, key1, PresentBytes{value1},
                                     ReadSource::Snapshot));
  require_status(builder.append_write(20, key2, PresentBytes{value2}));
  auto result = builder.seal();
  CHECK(result.ok());
  return std::move(result.batch);
}

void test_malformed_batches() {
  auto seed = make_malformed_seed();
  const MutableBatch base(seed->input());

  {
    auto broken = base;
    broken.header.pop_back();
    expect_error(broken.input(), BatchValidationErrorCode::HeaderSizeMismatch);
  }
  {
    auto broken      = base;
    broken.header[0] = std::byte{0};
    expect_error(broken.input(), BatchValidationErrorCode::InvalidMagic);
  }
  {
    auto broken = base;
    put_u16(broken.header, offsetof(BatchHeaderV1, version_le), 2);
    expect_error(broken.input(), BatchValidationErrorCode::UnsupportedVersion);
  }
  for (const auto offset : {offsetof(BatchHeaderV1, header_size_le),
                            offsetof(BatchHeaderV1, descriptor_size_le),
                            offsetof(BatchHeaderV1, blob_ref_size_le)}) {
    auto broken = base;
    put_u16(broken.header, offset, 0);
    expect_error(broken.input(),
                 BatchValidationErrorCode::HeaderLayoutMismatch);
  }
  {
    auto broken = base;
    put_u64(broken.header, offsetof(BatchHeaderV1, feature_bits_le), 1);
    expect_error(broken.input(), BatchValidationErrorCode::UnsupportedFeatures);
  }
  {
    auto broken = base;
    put_u32(broken.header, offsetof(BatchHeaderV1, metadata_flags_le), 2);
    expect_error(broken.input(),
                 BatchValidationErrorCode::UnsupportedMetadataFlags);
  }
  for (const auto offset : {offsetof(BatchHeaderV1, reserved0_le),
                            offsetof(BatchHeaderV1, reserved1_le)}) {
    auto broken = base;
    put_u32(broken.header, offset, 1);
    expect_error(broken.input(),
                 BatchValidationErrorCode::NonZeroHeaderReserved);
  }
  {
    auto broken = base;
    put_u32(broken.header, offsetof(BatchHeaderV1, metadata_flags_le), 0);
    expect_error(broken.input(),
                 BatchValidationErrorCode::NonCanonicalMetadata);
  }
  {
    auto broken = base;
    put_u64(broken.header, offsetof(BatchHeaderV1, commit_seq_le), 1);
    expect_error(broken.input(),
                 BatchValidationErrorCode::NonCanonicalCommitSequence);
  }
  {
    auto broken = base;
    put_u32(broken.header, offsetof(BatchHeaderV1, descriptor_span_count_le),
            0);
    expect_error(broken.input(),
                 BatchValidationErrorCode::DescriptorSpanCountMismatch);
  }
  {
    auto broken = base;
    put_u32(broken.header, offsetof(BatchHeaderV1, payload_segment_count_le),
            0);
    expect_error(broken.input(),
                 BatchValidationErrorCode::PayloadSegmentCountMismatch);
  }
  {
    auto broken = base;
    broken.payload_views.assign(static_cast<std::size_t>(kNullSegment) + 1,
                                ByteView{});
    put_u32(broken.header, offsetof(BatchHeaderV1, payload_segment_count_le),
            static_cast<std::uint32_t>(broken.payload_views.size()));
    expect_error(broken.input(),
                 BatchValidationErrorCode::TooManyPayloadSegments);
  }
  {
    auto broken                = base;
    broken.descriptor_views[0] = {};
    expect_error(broken.input(), BatchValidationErrorCode::EmptyDescriptorSpan);
  }
  {
    auto broken = base;
    broken.descriptors[0].pop_back();
    broken.refresh();
    expect_error(broken.input(),
                 BatchValidationErrorCode::MisalignedDescriptorSpan);
  }
  {
    auto broken             = base;
    broken.payload_views[0] = {};
    expect_error(broken.input(), BatchValidationErrorCode::EmptyPayloadSegment);
  }
  {
    auto broken = base;
    put_u32(broken.header, offsetof(BatchHeaderV1, event_count_le), 3);
    put_u32(broken.header, offsetof(BatchHeaderV1, read_count_le), 2);
    put_u32(broken.header, offsetof(BatchHeaderV1, write_count_le), 1);
    expect_error(broken.input(), BatchValidationErrorCode::EventCountMismatch);
  }
  {
    auto broken = base;
    put_u32(broken.header, offsetof(BatchHeaderV1, read_count_le), 2);
    put_u32(broken.header, offsetof(BatchHeaderV1, write_count_le), 0);
    expect_error(broken.input(),
                 BatchValidationErrorCode::OperationCountMismatch);
  }
  {
    auto broken = base;
    put_u32(broken.descriptors[0],
            sizeof(EventDescriptorV1) +
                offsetof(EventDescriptorV1, sequence_le),
            10);
    broken.refresh();
    expect_error(broken.input(),
                 BatchValidationErrorCode::NonIncreasingEventSequence, 1);
  }
  {
    auto broken                                            = base;
    broken.descriptors[0][offsetof(EventDescriptorV1, op)] = std::byte{0};
    broken.refresh();
    expect_error(broken.input(), BatchValidationErrorCode::InvalidOperation, 0);
  }
  {
    auto broken = base;
    broken.descriptors[0][offsetof(EventDescriptorV1, value_state)] =
        std::byte{0};
    broken.refresh();
    expect_error(broken.input(), BatchValidationErrorCode::InvalidValueState,
                 0);
  }
  {
    auto broken                                               = base;
    broken.descriptors[0][offsetof(EventDescriptorV1, flags)] = std::byte{1};
    broken.refresh();
    expect_error(broken.input(),
                 BatchValidationErrorCode::NonZeroDescriptorFlags, 0);
  }
  {
    auto broken                                                  = base;
    broken.descriptors[0][offsetof(EventDescriptorV1, reserved)] = std::byte{1};
    broken.refresh();
    expect_error(broken.input(),
                 BatchValidationErrorCode::NonZeroDescriptorReserved, 0);
  }
  {
    auto broken = base;
    put_u16(broken.descriptors[0],
            offsetof(EventDescriptorV1, key) + offsetof(BlobRefV1, reserved_le),
            1);
    broken.refresh();
    expect_error(broken.input(), BatchValidationErrorCode::NonZeroBlobReserved,
                 0);
  }
  {
    auto broken = base;
    put_u32(broken.descriptors[0],
            offsetof(EventDescriptorV1, key) + offsetof(BlobRefV1, offset_le),
            1);
    put_u32(broken.descriptors[0],
            offsetof(EventDescriptorV1, key) + offsetof(BlobRefV1, size_le), 0);
    put_u16(broken.descriptors[0],
            offsetof(EventDescriptorV1, key) + offsetof(BlobRefV1, segment_le),
            kNullSegment);
    broken.refresh();
    expect_error(broken.input(),
                 BatchValidationErrorCode::NonCanonicalNullReference, 0);
  }
  {
    auto broken = base;
    put_u32(broken.descriptors[0],
            offsetof(EventDescriptorV1, key) + offsetof(BlobRefV1, size_le), 0);
    broken.refresh();
    expect_error(broken.input(),
                 BatchValidationErrorCode::NonCanonicalEmptyReference, 0);
  }
  {
    auto broken = base;
    put_u16(broken.descriptors[0],
            offsetof(EventDescriptorV1, key) + offsetof(BlobRefV1, segment_le),
            1);
    broken.refresh();
    expect_error(broken.input(),
                 BatchValidationErrorCode::PayloadSegmentOutOfRange, 0);
  }
  {
    auto broken = base;
    put_u32(broken.descriptors[0],
            offsetof(EventDescriptorV1, key) + offsetof(BlobRefV1, offset_le),
            0xfffffff0U);
    put_u32(broken.descriptors[0],
            offsetof(EventDescriptorV1, key) + offsetof(BlobRefV1, size_le),
            0x40U);
    broken.refresh();
    expect_error(broken.input(), BatchValidationErrorCode::PayloadBounds, 0);
  }
  {
    auto broken = base;
    broken.descriptors[0][offsetof(EventDescriptorV1, value_state)] =
        static_cast<std::byte>(PackedValueState::Absent);
    broken.refresh();
    expect_error(broken.input(),
                 BatchValidationErrorCode::AbsentValueHasPayload, 0);
  }
}

void test_unaligned_and_randomized_decoder_inputs() {
  auto seed             = make_malformed_seed();
  const auto seed_input = seed->input();

  std::vector<std::byte> unaligned_header(seed_input.header.size() + 1);
  std::copy(seed_input.header.begin(), seed_input.header.end(),
            unaligned_header.begin() + 1);
  std::vector<std::vector<std::byte>> unaligned_descriptor_storage;
  std::vector<ByteView> unaligned_descriptors;
  for (const auto descriptor : seed_input.descriptor_spans) {
    unaligned_descriptor_storage.emplace_back(descriptor.size() + 1);
    auto &storage = unaligned_descriptor_storage.back();
    std::copy(descriptor.begin(), descriptor.end(), storage.begin() + 1);
  }
  for (const auto &storage : unaligned_descriptor_storage) {
    unaligned_descriptors.emplace_back(storage.data() + 1, storage.size() - 1);
  }
  std::vector<std::vector<std::byte>> unaligned_payload_storage;
  std::vector<ByteView> unaligned_payloads;
  for (const auto payload : seed_input.payload_segments) {
    unaligned_payload_storage.emplace_back(payload.size() + 1);
    auto &storage = unaligned_payload_storage.back();
    std::copy(payload.begin(), payload.end(), storage.begin() + 1);
  }
  for (const auto &storage : unaligned_payload_storage) {
    unaligned_payloads.emplace_back(storage.data() + 1, storage.size() - 1);
  }
  const auto unaligned_input =
      PackedBatchInput{ByteView(unaligned_header).subspan(1),
                       unaligned_descriptors, unaligned_payloads};
  require_validation_mode_agreement(unaligned_input);
  auto unaligned_result      = validate_packed_batch(unaligned_input);
  const auto *unaligned_view = std::get_if<ValidatedTxnView>(&unaligned_result);
  CHECK(unaligned_view != nullptr);
  CHECK(collect_events(*unaligned_view) == collect_events(seed->view()));

  // Decoder mutation smoke test. Arbitrary header/descriptor changes must
  // either be rejected or produce a safely traversable validated view.
  std::mt19937_64 random(0xbadc0decafef00dULL);
  const MutableBatch base(seed_input);
  for (std::size_t iteration = 0; iteration < 5000; ++iteration) {
    auto mutated              = base;
    const auto mutation_count = static_cast<std::size_t>(1U + random() % 4U);
    for (std::size_t mutation = 0; mutation < mutation_count; ++mutation) {
      if ((random() & 1U) == 0) {
        const auto offset =
            static_cast<std::size_t>(random() % mutated.header.size());
        mutated.header[offset] ^=
            static_cast<std::byte>(1U << static_cast<unsigned>(random() % 8U));
      } else {
        auto &descriptor = mutated.descriptors[0];
        const auto offset =
            static_cast<std::size_t>(random() % descriptor.size());
        descriptor[offset] ^=
            static_cast<std::byte>(1U << static_cast<unsigned>(random() % 8U));
      }
    }
    mutated.refresh();
    require_validation_mode_agreement(mutated.input());
    auto result = validate_packed_batch(mutated.input());
    if (const auto *view = std::get_if<ValidatedTxnView>(&result)) {
      CHECK(collect_events(*view).size() == view->event_count());
    }
  }
}

void test_chunk_boundaries_and_oversized_segments() {
  auto pool = std::make_shared<ChunkPool>(64, 32);
  ChunkBatchBuilder builder(pool, sample_meta());

  const std::vector<std::byte> key30(30, std::byte{0x11});
  const std::vector<std::byte> value34(34, std::byte{0x12});
  const std::vector<std::byte> key1(1, std::byte{0x21});
  const std::vector<std::byte> value65(65, std::byte{0x22});
  const std::vector<std::byte> key64(64, std::byte{0x31});

  require_status(builder.append_read(1, key30, PresentBytes{value34},
                                     ReadSource::Snapshot));
  require_status(builder.append_write(2, key1, PresentBytes{value65}));
  require_status(builder.append_write(3, key64, Absent{}));
  auto sealed = builder.seal();
  CHECK(sealed.ok());

  const auto input = sealed.batch->input();
  CHECK(input.descriptor_spans.size() == 2);
  CHECK(reinterpret_cast<std::uintptr_t>(input.descriptor_spans[0].data()) %
            64U ==
        0U);
  CHECK(reinterpret_cast<std::uintptr_t>(input.descriptor_spans[1].data()) %
            64U ==
        0U);
  CHECK(input.descriptor_spans[0].size() == 64);
  CHECK(input.descriptor_spans[1].size() == 32);
  CHECK(input.payload_segments.size() == 4);
  CHECK(input.payload_segments[0].size() == 64);
  CHECK(input.payload_segments[1].size() == 1);
  CHECK(input.payload_segments[2].size() == 65);
  CHECK(input.payload_segments[3].size() == 64);

  const auto events = collect_events(sealed.batch->view());
  CHECK(events.size() == 3);
  CHECK(events[0].key == key30);
  CHECK(events[0].value == value34);
  CHECK(events[1].key == key1);
  CHECK(events[1].value == value65);
  CHECK(events[2].key == key64);
  CHECK(events[2].state == ValueState::Absent);
  CHECK(sealed.batch->reserved_bytes() > sealed.batch->used_bytes());
}

void test_chunk_savepoint_recycle_and_abort() {
  auto pool = std::make_shared<ChunkPool>(32, 64);
  const std::vector<std::byte> key(16, std::byte{0x41});
  const std::vector<std::byte> value(16, std::byte{0x42});

  ChunkCheckpoint foreign_checkpoint;
  {
    ChunkBatchBuilder builder(pool, sample_meta());
    require_status(builder.append_write(1, key, PresentBytes{value}));
    foreign_checkpoint = builder.checkpoint();
    require_status(builder.append_write(2, key, PresentBytes{value}));
    const auto invalidated_later_checkpoint = builder.checkpoint();
    require_status(builder.append_write(3, key, PresentBytes{value}));
    const auto before_rollback = pool->stats();
    require_status(builder.rollback(foreign_checkpoint));
    const auto after_rollback = pool->stats();
    CHECK(after_rollback.recycles > before_rollback.recycles);

    // Rollback restores the event-sequence checkpoint, so the discarded
    // sequence number can be reused without repacking retained descriptors.
    require_status(builder.append_read(2, key, Absent{}, ReadSource::Snapshot));
    const auto stale_rollback = builder.rollback(invalidated_later_checkpoint);
    CHECK(!stale_rollback.ok());
    CHECK(stale_rollback.error->code == BatchBuildErrorCode::InvalidCheckpoint);
    auto sealed = builder.seal();
    CHECK(sealed.ok());
    const auto events = collect_events(sealed.batch->view());
    CHECK(events.size() == 2);
    CHECK(events[0].sequence == 1);
    CHECK(events[1].sequence == 2);
    CHECK(events[1].op == EventOp::Read);
    sealed.batch.reset();
  }

  const auto after_first_batch = pool->stats();
  CHECK(after_first_batch.cached_ordinary_segments > 0);
  {
    ChunkBatchBuilder other(pool, sample_meta());
    const auto invalid = other.rollback(foreign_checkpoint);
    CHECK(!invalid.ok());
    CHECK(invalid.error->code == BatchBuildErrorCode::InvalidCheckpoint);
    require_status(other.append_write(1, key, PresentBytes{value}));
    other.abort();
  }
  const auto after_abort = pool->stats();
  CHECK(after_abort.reuses > 0);
  CHECK(after_abort.recycles > after_first_batch.recycles);

  bool rejected_small_chunk = false;
  try {
    [[maybe_unused]] ChunkPool invalid(31);
  } catch (const std::invalid_argument &) {
    rejected_small_chunk = true;
  }
  CHECK(rejected_small_chunk);

  // Truncation inside the descriptor and payload tails must expose only the
  // replacement event; stale bytes beyond the restored offsets are not part
  // of the sealed segment views.
  {
    auto tail_pool = std::make_shared<ChunkPool>(64, 8);
    ChunkBatchBuilder builder(tail_pool, sample_meta());
    const auto tiny_key        = bytes("a");
    const auto tiny_value      = bytes("b");
    const auto discarded_key   = bytes("discarded-key");
    const auto discarded_value = bytes("discarded-value");
    const auto replacement_key = bytes("c");
    require_status(builder.append_write(1, tiny_key, PresentBytes{tiny_value}));
    const auto tail_checkpoint = builder.checkpoint();
    require_status(
        builder.append_write(2, discarded_key, PresentBytes{discarded_value}));
    require_status(builder.rollback(tail_checkpoint));
    require_status(builder.append_read(2, replacement_key, Absent{},
                                       ReadSource::Snapshot));
    auto sealed = builder.seal();
    CHECK(sealed.ok());
    CHECK(sealed.batch->input().descriptor_spans.size() == 1);
    CHECK(sealed.batch->input().descriptor_spans[0].size() == 64);
    CHECK(sealed.batch->input().payload_segments.size() == 1);
    CHECK(sealed.batch->input().payload_segments[0].size() == 3);
    const auto events = collect_events(sealed.batch->view());
    CHECK(events.size() == 2);
    CHECK(events[1].key == replacement_key);
    CHECK(events[1].state == ValueState::Absent);
  }
}

void test_builder_sequence_validation() {
  const auto key = bytes("key");
  VectorBatchBuilder vector_builder(sample_meta());
  require_status(
      vector_builder.append_read(5, key, Absent{}, ReadSource::Local));
  const auto vector_bad = vector_builder.append_write(4, key, Absent{});
  CHECK(!vector_bad.ok());
  CHECK(vector_bad.error->code ==
        BatchBuildErrorCode::NonIncreasingEventSequence);
  const auto vector_source_bad = vector_builder.append_read(
      6, key, Absent{}, static_cast<ReadSource>(0xff));
  CHECK(!vector_source_bad.ok());
  CHECK(vector_source_bad.error->code ==
        BatchBuildErrorCode::InvalidReadSource);

  auto pool = std::make_shared<ChunkPool>(64);
  ChunkBatchBuilder chunk_builder(pool, sample_meta());
  require_status(
      chunk_builder.append_read(5, key, Absent{}, ReadSource::Local));
  const auto chunk_bad = chunk_builder.append_write(5, key, Absent{});
  CHECK(!chunk_bad.ok());
  CHECK(chunk_bad.error->code ==
        BatchBuildErrorCode::NonIncreasingEventSequence);
}

[[nodiscard]] std::vector<std::byte> random_bytes(std::mt19937_64 &random,
                                                  std::size_t size) {
  std::vector<std::byte> result(size);
  for (auto &byte : result) {
    byte = static_cast<std::byte>(random() & 0xffU);
  }
  return result;
}

void test_randomized_vector_chunk_conformance() {
  std::mt19937_64 random(0x5eed5eed12345678ULL);
  for (std::uint64_t iteration = 0; iteration < 200; ++iteration) {
    auto meta = sample_meta((iteration % 2U) == 0);
    meta.commit_seq += iteration;
    meta.session_seq += iteration;
    meta.producer_seq += iteration;

    VectorBatchBuilder vector_builder(meta);
    auto pool = std::make_shared<ChunkPool>(
        static_cast<std::size_t>(32U + (iteration % 97U)), 256);
    ChunkBatchBuilder chunk_builder(pool, meta);
    std::vector<OwnedEvent> expected;

    const auto generated_event_count =
        static_cast<std::uint32_t>(random() % 80U);
    EventSeq sequence = 0;
    for (std::uint32_t index = 0; index < generated_event_count; ++index) {
      sequence += static_cast<EventSeq>(1U + (random() % 3U));
      const auto key =
          random_bytes(random, static_cast<std::size_t>(random() % 24U));
      const bool present = (random() & 1U) != 0;
      const auto value =
          random_bytes(random, static_cast<std::size_t>(random() % 96U));
      const bool read = (random() & 1U) != 0;

      if (read) {
        const auto source =
            (random() % 5U) == 0 ? ReadSource::Local : ReadSource::Snapshot;
        const TaggedBytes tagged =
            present ? TaggedBytes{PresentBytes{value}} : TaggedBytes{Absent{}};
        require_status(
            vector_builder.append_read(sequence, key, tagged, source));
        require_status(
            chunk_builder.append_read(sequence, key, tagged, source));
        if (source == ReadSource::Snapshot) {
          expected.push_back(
              OwnedEvent{sequence, EventOp::Read,
                         present ? ValueState::Present : ValueState::Absent,
                         key, present ? value : std::vector<std::byte>{}});
        }
      } else {
        const TaggedBytes tagged =
            present ? TaggedBytes{PresentBytes{value}} : TaggedBytes{Absent{}};
        require_status(vector_builder.append_write(sequence, key, tagged));
        require_status(chunk_builder.append_write(sequence, key, tagged));
        expected.push_back(
            OwnedEvent{sequence, EventOp::Write,
                       present ? ValueState::Present : ValueState::Absent, key,
                       present ? value : std::vector<std::byte>{}});
      }
    }

    auto vector_sealed = vector_builder.seal();
    auto chunk_sealed  = chunk_builder.seal();
    CHECK(vector_sealed.ok());
    CHECK(chunk_sealed.ok());
    CHECK(same_meta(vector_sealed.batch->view().meta(), captured_meta(meta)));
    CHECK(same_meta(chunk_sealed.batch->view().meta(), captured_meta(meta)));
    CHECK(collect_events(vector_sealed.batch->view()) == expected);
    CHECK(collect_events(chunk_sealed.batch->view()) == expected);
    CHECK(collect_events(vector_sealed.batch->view()) ==
          collect_events(chunk_sealed.batch->view()));
    CHECK(std::holds_alternative<ValidatedTxnView>(
        validate_packed_batch(vector_sealed.batch->input())));
    CHECK(std::holds_alternative<ValidatedTxnView>(
        validate_packed_batch(chunk_sealed.batch->input())));
    require_validation_mode_agreement(vector_sealed.batch->input());
    require_validation_mode_agreement(chunk_sealed.batch->input());
  }
}

} // namespace

int run_packed_batch_tests() {
  try {
    test_layout_and_empty_batches();
    test_tagged_values_local_filter_and_zero_copy();
    test_golden_little_endian_encoding();
    test_malformed_batches();
    test_unaligned_and_randomized_decoder_inputs();
    test_chunk_boundaries_and_oversized_segments();
    test_chunk_savepoint_recycle_and_abort();
    test_builder_sequence_validation();
    test_randomized_vector_chunk_conformance();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "packed batch tests failed: " << error.what() << '\n';
    return 1;
  }
}

#ifdef CO_CHECKER_PACKED_BATCH_STANDALONE_TEST_MAIN
int main() { return run_packed_batch_tests(); }
#endif
