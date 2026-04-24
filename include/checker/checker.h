#pragma once

#include <string_view>

#include "common/diag/logger.h"
#include "common/trace/trace.h"
#include "common/trace/trace_index.h"
#include "common/util/check_result.h"

namespace checker {

struct CheckerContext {
  const common::Trace &trace;
  const common::TraceIndex &index;
  common::Logger *logger = nullptr;
};

class Checker {
public:
  virtual ~Checker() = default;

  [[nodiscard]] virtual std::string_view name() const                    = 0;
  [[nodiscard]] virtual CheckResult run(const CheckerContext &ctx) const = 0;
};

} // namespace checker
