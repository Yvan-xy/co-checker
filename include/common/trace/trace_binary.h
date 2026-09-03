#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

#include "common/diag/result.h"
#include "common/trace/trace.h"
#include "common/trace/trace_parser.h"

namespace common {

inline constexpr std::string_view kBinaryTraceMagic     = "COTRACE1";
inline constexpr std::string_view kBinaryTraceExtension = ".trace";
inline constexpr std::uint32_t kBinaryTraceRawFlags     = 0;
inline constexpr std::uint32_t kBinaryTraceDenseIdsFlag = 1U << 0U;

struct DenseBinaryTraceStats {
  std::uint64_t transactions = 0;
  std::uint64_t events       = 0;
  std::uint64_t keys         = 0;
  std::uint64_t values       = 0;
  std::uint64_t output_bytes = 0;
};

[[nodiscard]] bool is_binary_trace_stream(std::istream &input);

[[nodiscard]] Result<Trace> read_binary_trace_stream(std::istream &input,
                                                     ParseOptions options = {});

[[nodiscard]] Result<Trace> read_binary_trace_file(std::string_view path,
                                                   ParseOptions options = {});

// Converts a raw COTRACE1 stream to its dense-ID representation without
// materializing the complete trace. The dense representation uses the same
// header and transaction records, with kBinaryTraceDenseIdsFlag set, and
// fixed-width key/value IDs in each event record.
[[nodiscard]] Result<DenseBinaryTraceStats>
write_dense_binary_trace(std::istream &input, std::ostream &output,
                         ParseOptions options = {});

[[nodiscard]] bool write_binary_trace_stream(std::ostream &output,
                                             const Trace &trace,
                                             std::string *error = nullptr);

[[nodiscard]] bool write_binary_trace_file(std::string_view path,
                                           const Trace &trace,
                                           std::string *error = nullptr);

} // namespace common
