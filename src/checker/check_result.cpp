#include "common/util/check_result.h"

namespace checker {

const char *to_string(CheckStatus status) {
  switch (status) {
  case CheckStatus::Pass:
    return "pass";
  case CheckStatus::Fail:
    return "fail";
  case CheckStatus::Inconclusive:
    return "inconclusive";
  }
  return "unknown";
}

} // namespace checker
