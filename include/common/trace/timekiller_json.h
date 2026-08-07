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

// Converts a COTRACE1 stream to TimeKiller's key-value JSON representation.
// Input records and JSON output are processed sequentially. Only the exact
// dictionaries needed to assign stable key/value ids are retained in memory.
[[nodiscard]] Result<TimeKillerJsonStats>
write_timekiller_json(std::istream &input, std::ostream &output,
                      ParseOptions options = {});

} // namespace common
