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
  [[nodiscard]] virtual common::Diagnostic
  make_diagnostic(std::string code, std::string message,
                  const common::Transaction &txn,
                  const common::Event &event) const {
    common::Diagnostic diagnostic;
    diagnostic.severity  = common::Severity::Error;
    diagnostic.checker   = "N/A";
    diagnostic.code      = std::move(code);
    diagnostic.message   = std::move(message);
    diagnostic.txn_id    = txn.id;
    diagnostic.event_id  = event.id;
    diagnostic.event_seq = event.seq;
    diagnostic.key       = event.key;
    return diagnostic;
  }
};

} // namespace checker
