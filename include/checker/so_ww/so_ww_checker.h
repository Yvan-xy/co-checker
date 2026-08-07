#pragma once

#include <unordered_map>
#include <utility>
#include <vector>

#include "checker/checker.h"

namespace checker {

using namespace common;

struct DeltaEntry {
  KeyId key_id;
  ValueId old_value_id;
};

using SOWWDelta      = std::vector<DeltaEntry>;
using SOWWChangeList = std::vector<std::pair<TxnId, SOWWDelta>>;

struct SOWWContext final : public CheckerContext {
  std::vector<ValueId> cur_snapshot;
  SOWWChangeList cl;
  std::unordered_map<SessionId, TxnId> latest_txns_by_session;

  template <typename T>
  SOWWContext(T &&ctx) noexcept : CheckerContext(std::forward<T>(ctx)) {}
};

class SnapshotSOWWChecker final : public Checker {
public:
  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] CheckResult run(const CheckerContext &ctx) const override;
  [[nodiscard]] common::Diagnostic
  make_diagnostic(std::string message, const common::Transaction &txn,
                  const common::Event &event) const override;
};

} // namespace checker
