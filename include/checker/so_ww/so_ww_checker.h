#pragma once

#include <utility>

#include "checker/checker.h"
#include "common/trace/snapshot.h"

namespace checker {

using namespace common;

typedef std::vector<std::pair<Key, std::optional<Value>>> Delta;
typedef std::vector<std::pair<TxnId, Delta>> ChangeList;

struct SOWWContext final : public CheckerContext {
  SnapshotOpt cur_snapshot;
  ChangeList cl;
  std::unordered_map<SessionId, TxnId> latest_txns_by_session;
  std::unordered_map<Key, TxnId, HexValueHash> latext_writer;

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
