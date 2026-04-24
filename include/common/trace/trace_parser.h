#pragma once

#include <istream>
#include <string_view>

#include "common/diag/result.h"
#include "common/trace/trace.h"

namespace common {

struct ParseOptions {
  bool require_commit_order       = true;
  bool reject_duplicate_commit_ts = true;
};

class TraceParser {
public:
  explicit TraceParser(ParseOptions options = {});

  [[nodiscard]] Result<Trace> parse_file(std::string_view path) const;
  [[nodiscard]] Result<Trace> parse_stream(std::istream &input) const;

private:
  ParseOptions options_;
};

} // namespace common
