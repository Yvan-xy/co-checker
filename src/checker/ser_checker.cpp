#include "checker/serializable/ser_checker.h"

#include <cassert>
#include <string>
#include <unordered_map>

namespace checker {

namespace {

bool readMatchesExpected(const Event &read,
                         const std::optional<Value> &expected) {
  assert(isRead(read));
  if (!expected.has_value()) {
    return read.is_miss;
  }
  return !read.is_miss && read.value.has_value() && read.value == expected;
}

void updateSnapshot(
    SnapshotOpt &snapshot,
    const std::unordered_map<Key, std::optional<Value>, HexValueHash> &writes) {
  for (const auto &[key, value] : writes) {
    snapshot.set(key, value);
  }
}

} // namespace

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

  // Validate a serial execution in parsed commit order.
  for (const auto &txn : ctx.trace.transactions) {
    std::unordered_map<Key, std::optional<Value>, HexValueHash> local_writes;
    local_writes.reserve(txn.event_ids.size());

    for (const auto eid : txn.event_ids) {
      const auto &event = ctx.trace.events[eid];
      if (event.op == EventOp::Write) {
        local_writes[event.key] = event.value;
      } else if (event.op == EventOp::Read) {
        const auto local_write = local_writes.find(event.key);
        const auto expected    = local_write != local_writes.end()
                                     ? local_write->second
                                     : ctx.cur_snapshot.get(event.key);
        if (!readMatchesExpected(event, expected)) {
          auto diagnostic = make_diagnostic(
              local_write != local_writes.end()
                  ? "read value does not match the prior write in the same "
                    "transaction"
                  : "read value does not match the commit-order serial "
                    "snapshot",
              txn, event);
          result.diagnostics.push_back(std::move(diagnostic));
        }
      } else {
        assert(false && "Unknown event operation");
      }
    }

    updateSnapshot(ctx.cur_snapshot, local_writes);
  }

  result.status =
      result.diagnostics.empty() ? CheckStatus::Pass : CheckStatus::Fail;

  return result;
}
} // namespace checker
