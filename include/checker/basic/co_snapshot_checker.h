#pragma once

#include "checker/checker.h"

namespace checker {

class CoSnapshotBasicChecker final : public Checker {
public:
  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] CheckResult run(const CheckerContext &ctx) const override;
  [[nodiscard]] common::Diagnostic
  make_diagnostic(std::string message, const common::Transaction &txn,
                  const common::Event &event) const override;
};

} // namespace checker
