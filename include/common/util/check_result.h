#pragma once

#include <vector>

#include "common/diag/diagnostic.h"

namespace checker {

enum class CheckStatus {
  Pass,
  Fail,
  Inconclusive,
};

struct CheckResult {
  CheckStatus status = CheckStatus::Pass;
  common::DiagnosticList diagnostics;
};

[[nodiscard]] const char *to_string(CheckStatus status);

} // namespace checker
