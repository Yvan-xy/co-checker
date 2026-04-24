#pragma once

#include <istream>
#include <string_view>

#include "co_checker/diag/result.h"
#include "co_checker/trace/trace.h"

namespace co_checker {

struct ParseOptions {
    bool require_commit_order = true;
    bool reject_duplicate_commit_ts = true;
};

class TraceParser {
public:
    explicit TraceParser(ParseOptions options = {});

    [[nodiscard]] Result<Trace> parse_file(std::string_view path) const;
    [[nodiscard]] Result<Trace> parse_stream(std::istream& input) const;

private:
    ParseOptions options_;
};

}  // namespace co_checker
