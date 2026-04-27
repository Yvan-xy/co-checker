#include "checker/so_only/so_only_checker.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <unordered_set>

#include <iostream>

namespace checker {

namespace {

std::optional<TxnId> getSOMax(const SnapshotSOContext &ctx,
                              SessionId session_id) {
  auto it = ctx.latest_txns_by_session.find(session_id);
  if (it == ctx.latest_txns_by_session.end()) {
    return std::nullopt;
  }
  return it->second;
}

void updateCtx(SnapshotSOContext &ctx, const Transaction &txn,
               Snapshot &snapshot) {

  for (const auto &eid : txn.event_ids) {
    const auto &event = ctx.trace.events[eid];
    if (isWrite(event)) {
      if (event.is_delete) {
        snapshot.erase(event.key);
      } else if (!event.is_miss && event.value.has_value()) {
        snapshot.set(event.key, *(event.value));
      }
    }
  }
  ctx.session_snapshots[txn.session_id].copy_from(snapshot);
  ctx.latest_txns_by_session[txn.session_id] = txn.id;
}

} // namespace

common::Diagnostic
SnapshotSOChecker::make_diagnostic(std::string message,
                                   const common::Transaction &txn,
                                   const common::Event &event) const {
  auto diagnostic    = Checker::make_diagnostic(std::move(message), txn, event);
  diagnostic.checker = "co-si-so-only";
  return diagnostic;
}

std::string_view SnapshotSOChecker::name() const { return "co-si-so-only"; }

CheckResult SnapshotSOChecker::run(const CheckerContext &ctx_) const {
  CheckResult result;
  SnapshotSOContext ctx(std::move(ctx_));
  Snapshot latest;
  for (const auto &txn : ctx.trace.transactions) {
    // get the snapshot of t_max
    auto t_max = getSOMax(ctx, txn.session_id);
    Snapshot snapshot;
    if (t_max.has_value()) {
      // print_snapshot_map(std::cout, ctx.session_snapshots);
      auto it = ctx.session_snapshots.find(ctx.trace.getTxn(*t_max).session_id);
      assert(it != ctx.session_snapshots.end());
      snapshot = it->second;
    } else {
      updateCtx(ctx, txn, latest);
      continue;
    }

    // build wrong_keys set
    std::unordered_set<Key, HexValueHash> wrong_keys;
    SnapshotOpt reads;
    for (const auto &eid : txn.event_ids) {
      const auto &event = ctx.trace.events[eid];
      if (isRead(event)) {
        if (reads.has_key(event.key) && (reads.get(event.key) != event.value)) {
          auto diagnostic = make_diagnostic(
              "inconsistent read of the same key in the same transaction", txn,
              event);
          result.diagnostics.push_back(diagnostic);
        }
        reads.set(event.key, event.value);
        bool is_wrong = false;
        is_wrong |= event.is_miss && snapshot.get(event.key).has_value();
        is_wrong |= !event.is_miss && !snapshot.get(event.key).has_value();
        is_wrong |= !event.is_miss && snapshot.get(event.key).has_value() &&
                    (snapshot.get(event.key) != event.value);
        if (is_wrong)
          wrong_keys.insert(event.key);
      }
    }

    bool viable = false;

    // traverse transactions between t_max and t
    for (auto i = *t_max + 1; i < txn.id; i++) {
      const Transaction &tmp = ctx.trace.transactions[i];
      // check all writes in tmp. If all writes are consistent with snapshot,
      // then Snapshot of tmp is consistent with txn.
      for (const auto &eid : tmp.event_ids) {
        const auto &e = ctx.trace.events[eid];
        if (isRead(e))
          continue;

        // skip this write if it is not read by txn
        if (!reads.has_key(e.key))
          continue;

        auto val = reads.get(e.key);
        if (val == e.value) {
          wrong_keys.erase(e.key);
        } else {
          wrong_keys.insert(e.key);
        }
      }
      if (wrong_keys.empty()) {
        viable = true;
        break;
      }
    }

    if (!viable) {
      for (const auto &key : wrong_keys) {
        auto diagnostic = make_diagnostic("Violation detected ", txn,
                                          ctx.trace.getEvent(txn.event_ids[0]));
        diagnostic.key  = key;
        result.diagnostics.push_back(diagnostic);
      }
    }

    updateCtx(ctx, txn, latest);
  }

  result.status =
      result.diagnostics.empty() ? CheckStatus::Pass : CheckStatus::Fail;
  return result;
}

} // namespace checker
