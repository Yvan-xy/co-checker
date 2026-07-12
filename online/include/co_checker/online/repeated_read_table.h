#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "co_checker/online/types.h"

namespace co_checker::online {

enum class RepeatedReadProbeMode : std::uint8_t {
  Scalar,
  SSE2,
  AVX2,
  NEON,
};

using RepeatedReadHash = std::uint64_t (*)(ByteView) noexcept;

[[nodiscard]] std::uint64_t repeated_read_hash(ByteView bytes) noexcept;
[[nodiscard]] std::vector<RepeatedReadProbeMode>
supported_repeated_read_probe_modes();
[[nodiscard]] RepeatedReadProbeMode default_repeated_read_probe_mode() noexcept;

enum class RepeatedReadInsertKind : std::uint8_t {
  Inserted,
  SameResult,
  ConflictingResult,
};

struct RepeatedReadInsertResult {
  RepeatedReadInsertKind kind = RepeatedReadInsertKind::Inserted;
  EventSeq first_sequence     = 0;
};

struct RepeatedReadTableMetrics {
  std::size_t size              = 0;
  std::size_t capacity          = 0;
  std::uint64_t probe_groups    = 0;
  std::uint64_t key_comparisons = 0;
  std::uint64_t resizes         = 0;
  RepeatedReadProbeMode mode    = RepeatedReadProbeMode::Scalar;
};

// Transaction-local insert-only table. All byte views are borrowed from the
// current transaction and are discarded by reset(); no view is retained in
// persistent checker state.
class RepeatedReadTable final {
public:
  explicit RepeatedReadTable(
      RepeatedReadProbeMode mode = default_repeated_read_probe_mode(),
      RepeatedReadHash hash      = repeated_read_hash);
  ~RepeatedReadTable();

  RepeatedReadTable(RepeatedReadTable &&) noexcept;
  RepeatedReadTable &operator=(RepeatedReadTable &&) noexcept;
  RepeatedReadTable(const RepeatedReadTable &)            = delete;
  RepeatedReadTable &operator=(const RepeatedReadTable &) = delete;

  // Clears borrowed observations while retaining scratch storage. expected
  // is an upper bound hint; the table still grows safely if it is exceeded.
  void reset(std::size_t expected);
  // Removes every borrowed ByteView without allocating. Call before the
  // producer-owned transaction storage can be recycled.
  void clear() noexcept;

  [[nodiscard]] RepeatedReadInsertResult insert(const EventView &read);
  [[nodiscard]] RepeatedReadTableMetrics metrics() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace co_checker::online
