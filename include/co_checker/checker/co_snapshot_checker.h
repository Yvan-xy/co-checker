#pragma once

#include "co_checker/checker/checker.h"

namespace co_checker {

class CoSnapshotChecker final : public Checker {
public:
    [[nodiscard]] std::string_view name() const override;
    [[nodiscard]] CheckResult run(const CheckerContext& ctx) const override;
};

}  // namespace co_checker
