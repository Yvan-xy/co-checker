#pragma once

#include <optional>
#include <unordered_map>

#include <immer/map.hpp>

#include "checker/checker.h"
#include "common/trace/trace_index.h"

namespace checker {

using namespace common;

using SnapshotMap = immer::map<Key, std::optional<Value>, HexValueHash>;

struct SnapshotSOContext final : public CheckerContext {
  std::unordered_map<SessionId, SnapshotMap> session_snapshots;
  std::unordered_map<SessionId, TxnId> latest_txns_by_session;

  template <typename T>
  SnapshotSOContext(T &&ctx) noexcept : CheckerContext(std::forward<T>(ctx)) {}
};

// Snapshot isolation checker. It only use session order (SO) to eliminate
// redundent checks.
class SnapshotSOChecker final : public Checker {
public:
  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] CheckResult run(const CheckerContext &ctx) const override;
  [[nodiscard]] common::Diagnostic
  make_diagnostic(std::string message, const common::Transaction &txn,
                  const common::Event &event) const override;

private:
};

} // namespace checker
