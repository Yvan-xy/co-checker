#pragma once

#include "checker/checker.h"

namespace checker {

// Snapshot isolation checker, which only use session order (SO) to eliminate
// redundent checks.
class SnapshotSOChecker final : public Checker {
public:
  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] CheckResult run(const CheckerContext &ctx) const override;
};

} // namespace checker
