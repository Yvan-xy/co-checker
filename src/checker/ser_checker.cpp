#include "checker/serializable/ser_checker.h"

#include <algorithm>
#include <cassert>
#include <unordered_set>

namespace checker {

std::string_view SerializableChecker::name() const { return "serializable"; }

common::Diagnostic
SerializableChecker::make_diagnostic(std::string message,
                                     const common::Transaction &txn,
                                     const common::Event &event) const {
  auto diagnostic    = Checker::make_diagnostic(std::move(message), txn, event);
  diagnostic.checker = "serializable";
  return diagnostic;
}

CheckResult SerializableChecker::run(const CheckerContext &ctx_) const {
  CheckResult result;
  SERContext ctx(std::move(ctx_));

  std::unordered_set<Key, HexValueHash> seen_keys;
  std::unordered_map<Key, std::optional<Value>, HexValueHash> seen_writes;

  for (const auto &txn : ctx.trace.transactions) {
    for (const auto eid : txn.event_ids) {
      const auto &event = ctx.trace.events[eid];
      if (event.op == EventOp::Write) {
        seen_writes[event.key] = event.value;
        seen_keys.insert(event.key);
      } else if (event.op == EventOp::Read) {
        if (seen_keys.find(event.key) != seen_keys.end()) {
          if (event.value != seen_writes[event.key]) {
            result.status = CheckStatus::Fail;
            auto diagnostic =
                make_diagnostic("read value does not match the write value in "
                                "the same transaction",
                                txn, event);
            result.diagnostics.push_back(std::move(diagnostic));
          }
          continue;
        }
        
        if (event.value != ctx.cur_snapshot.get(event.key)) {
          result.status   = CheckStatus::Fail;
          auto diagnostic = make_diagnostic(
              "read value does not match the current snapshot", txn, event);
          result.diagnostics.push_back(std::move(diagnostic));
        }
      } else {
        assert(false && "Unknown event operation");
      }
    }

    for (const auto &[key, value] : seen_writes) {
      ctx.cur_snapshot.set(key, value);
    }
  }

  result.status =
      result.diagnostics.empty() ? CheckStatus::Pass : CheckStatus::Fail;

  return result;
}
} // namespace checker