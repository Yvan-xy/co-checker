#pragma once

#include "checker/checker.h"

namespace checker {

class CoSnapshotBasicChecker final : public Checker {
public:
  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] CheckResult run(const CheckerContext &ctx) const override;
};

} // namespace checker
