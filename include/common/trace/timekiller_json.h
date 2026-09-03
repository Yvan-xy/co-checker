#pragma once

#include <cstdint>
#include <iosfwd>

#include "common/diag/result.h"
#include "common/trace/trace_parser.h"

namespace common {

struct TimeKillerJsonStats {
  std::uint64_t transactions = 0;
  std::uint64_t events       = 0;
  std::uint64_t keys         = 0;
  std::uint64_t values       = 0;
  std::uint64_t output_bytes = 0;
};

// Converts either raw or dense-ID COTRACE1 to TimeKiller's key-value JSON
// representation. Input records and JSON output are processed sequentially.
// Raw input retains only the dictionaries needed to assign stable IDs; dense
// input reuses its validated IDs directly.
[[nodiscard]] Result<TimeKillerJsonStats>
write_timekiller_json(std::istream &input, std::ostream &output,
                      ParseOptions options = {});

} // namespace common
