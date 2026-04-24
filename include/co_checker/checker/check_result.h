#pragma once

#include <vector>

#include "co_checker/diag/diagnostic.h"

namespace co_checker {

enum class CheckStatus {
    Pass,
    Fail,
    Inconclusive,
};

struct CheckResult {
    CheckStatus status = CheckStatus::Pass;
    DiagnosticList diagnostics;
};

[[nodiscard]] const char* to_string(CheckStatus status);

}  // namespace co_checker
