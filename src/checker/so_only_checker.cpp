#include "checker/so_only/so_only_checker.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <string>
#include <unordered_set>

#include <immer/map_transient.hpp>

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

std::optional<Value> getFromSnapshot(const SnapshotMap &snapshot,
                                     const Key &key) {
  const auto *value = snapshot.find(key);
  if (value == nullptr) {
    return std::nullopt;
  }
  return *value;
}

SnapshotMap updateCtx(SnapshotSOContext &ctx, const Transaction &txn,
                      const SnapshotMap &latest) {
  auto next = latest.transient();

  for (const auto &eid : txn.event_ids) {
    const auto &event = ctx.trace.events[eid];
    if (isWrite(event)) {
      if (event.is_delete) {
        next.set(event.key, std::nullopt);
      } else if (!event.is_miss && event.value.has_value()) {
        next.set(event.key, event.value);
      }
    }
  }

  auto snapshot = std::move(next).persistent();

  ctx.session_snapshots[txn.session_id]      = snapshot;
  ctx.latest_txns_by_session[txn.session_id] = txn.id;
  return snapshot;
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
  SnapshotMap latest;
  for (const auto &txn : ctx.trace.transactions) {
    // get the snapshot of t_max
    auto t_max = getSOMax(ctx, txn.session_id);
    SnapshotMap snapshot;
    if (t_max.has_value()) {
      auto it = ctx.session_snapshots.find(ctx.trace.getTxn(*t_max).session_id);
      assert(it != ctx.session_snapshots.end());
      snapshot = it->second;
    } else {
      latest = updateCtx(ctx, txn, latest);
      continue;
    }

    // build wrong_keys set
    std::unordered_set<Key, HexValueHash> wrong_keys;
    wrong_keys.reserve(txn.event_ids.size());
    SnapshotOpt reads;
    reads.data.reserve(txn.event_ids.size());
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
        const auto expected = getFromSnapshot(snapshot, event.key);
        const bool is_wrong = (event.is_miss && expected.has_value()) ||
                              (!event.is_miss && (!expected.has_value() ||
                                                  expected != event.value));
        if (is_wrong) {
          wrong_keys.insert(event.key);
        }
      }
    }

    bool viable = wrong_keys.empty();

    // traverse transactions between t_max and t
    for (auto i = *t_max + 1; !viable && i < txn.id; i++) {
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

    latest = updateCtx(ctx, txn, latest);
  }

  result.status =
      result.diagnostics.empty() ? CheckStatus::Pass : CheckStatus::Fail;
  return result;
}

} // namespace checker
