#include "checker/so_ww/so_ww_checker.h"

#include <algorithm>
#include <cassert>
#include <unordered_set>

namespace checker {

namespace {

inline std::optional<TxnId> getSOWWMax(const SOWWContext &ctx,
                                       SessionId session_id,
                                       const Transaction &txn) {
  std::optional<TxnId> res = std::nullopt;
  auto it                  = ctx.latest_txns_by_session.find(session_id);
  if (it != ctx.latest_txns_by_session.end()) {
    res = it->second;
  }

  // find latest writer
  std::unordered_set<Key, HexValueHash> visited;
  for (const auto eid : txn.event_ids) {
    const Event &e = ctx.trace.getEvent(eid);
    if (!isWrite(e) || visited.contains(e.key))
      continue;

    visited.insert(e.key);
    const auto &it = ctx.latext_writer.find(e.key);
    if (it == ctx.latext_writer.end())
      continue;

    TxnId w = it->second;
    res     = (res == std::nullopt) ? w : std::max(w, *res);
  }

  return res;
}

void updateSnapshot(SOWWContext &ctx, const Transaction &txn) {
  SOWWDelta d;
  std::unordered_set<Key, HexValueHash> updated_keys;
  for (auto it = txn.event_ids.rbegin(); it != txn.event_ids.rend(); it++) {
    const Event &e = ctx.trace.getEvent(*it);
    if (isWrite(e)) {
      // not the latest write on e.key
      if (updated_keys.contains(e.key))
        continue;
      updated_keys.insert(e.key);

      // update current snapshot
      d.push_back(DeltaEntry{*it, ctx.cur_snapshot.get(e.key)});
      ctx.cur_snapshot.set(e.key, e.value);

      // update latest writers
      ctx.latext_writer[e.key] = txn.id;
    }
  }
  ctx.cl.push_back(std::make_pair(txn.id, d));
  ctx.latest_txns_by_session[txn.session_id] = txn.id;
  return;
}

} // namespace

std::string_view SnapshotSOWWChecker::name() const { return "snapshot-so-ww"; }

common::Diagnostic
SnapshotSOWWChecker::make_diagnostic(std::string message,
                                     const common::Transaction &txn,
                                     const common::Event &event) const {
  auto diagnostic    = Checker::make_diagnostic(std::move(message), txn, event);
  diagnostic.checker = "snapshot-so-ww";
  return diagnostic;
}

CheckResult SnapshotSOWWChecker::run(const CheckerContext &ctx_) const {
  CheckResult result;
  SOWWContext ctx(std::move(ctx_));
  ctx.cur_snapshot.data.reserve(ctx.trace.events.size());
  ctx.cl.reserve(ctx.trace.transactions.size());
  ctx.latest_txns_by_session.reserve(ctx.trace.transactions.size());
  ctx.latext_writer.reserve(ctx.trace.events.size());

  for (const auto &txn : ctx.trace.transactions) {
    auto t_max = getSOWWMax(ctx, txn.session_id, txn);

    if (t_max == std::nullopt) {
      updateSnapshot(ctx, txn);
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
        const auto expected = ctx.cur_snapshot.get(event.key);
        const bool is_wrong = (event.is_miss && expected.has_value()) ||
                              (!event.is_miss && (!expected.has_value() ||
                                                  expected != event.value));
        if (is_wrong) {
          wrong_keys.insert(event.key);
        }
      }
    }

    bool viable = false;
    auto id     = ctx.cl.size();
    while (true) {
      if (wrong_keys.empty()) {
        viable = true;
        break;
      }

      if (id == 0 || ctx.cl[id - 1].first == *t_max) {
        break;
      }

      id--;

      const auto &old_vals = ctx.cl[id].second;
      for (const auto &[eid, v] : old_vals) {
        const auto &k = ctx.trace.getEvent(eid).key;
        if (!reads.has_key(k)) {
          continue;
        }
        if (reads.get(k) == v) {
          wrong_keys.erase(k);
        } else {
          wrong_keys.insert(k);
        }
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

    updateSnapshot(ctx, txn);
  }

  result.status =
      result.diagnostics.empty() ? CheckStatus::Pass : CheckStatus::Fail;
  return result;
}

} // namespace checker
