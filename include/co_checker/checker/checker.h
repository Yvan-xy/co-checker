#pragma once

#include <string_view>

#include "co_checker/checker/check_result.h"
#include "co_checker/diag/logger.h"
#include "co_checker/trace/trace.h"
#include "co_checker/trace/trace_index.h"

namespace co_checker {

struct CheckerContext {
    const Trace& trace;
    const TraceIndex& index;
    Logger* logger = nullptr;
};

class Checker {
public:
    virtual ~Checker() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual CheckResult run(const CheckerContext& ctx) const = 0;
};

}  // namespace co_checker
