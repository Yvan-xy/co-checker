#pragma once

#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "checker/checker.h"
#include "common/trace/snapshot.h"

namespace checker {
using namespace common;

struct SERContext final : public CheckerContext {
  SnapshotOpt cur_snapshot;

  template <typename T>
  SERContext(T &&ctx) noexcept : CheckerContext(std::forward<T>(ctx)) {}
};

class SerializableChecker final : public Checker {
public:
  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] CheckResult run(const CheckerContext &ctx) const override;
  [[nodiscard]] common::Diagnostic
  make_diagnostic(std::string message, const common::Transaction &txn,
                  const common::Event &event) const override;
};

} // namespace checker