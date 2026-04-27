#pragma once

#include <iosfwd>
#include <string>
#include <string_view>

#include "common/diag/result.h"
#include "common/trace/trace.h"
#include "common/trace/trace_parser.h"

namespace common {

inline constexpr std::string_view kBinaryTraceMagic     = "COTRACE1";
inline constexpr std::string_view kBinaryTraceExtension = ".trace";

[[nodiscard]] bool is_binary_trace_stream(std::istream &input);

[[nodiscard]] Result<Trace> read_binary_trace_stream(std::istream &input,
                                                     ParseOptions options = {});

[[nodiscard]] Result<Trace> read_binary_trace_file(std::string_view path,
                                                   ParseOptions options = {});

[[nodiscard]] bool write_binary_trace_stream(std::ostream &output,
                                             const Trace &trace,
                                             std::string *error = nullptr);

[[nodiscard]] bool write_binary_trace_file(std::string_view path,
                                           const Trace &trace,
                                           std::string *error = nullptr);

} // namespace common
