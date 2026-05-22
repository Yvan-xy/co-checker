#include "checker/so_ww/so_ww_checker.h"

#include <algorithm>
#include <cassert>
#include <unordered_set>

#define STAT 1

#ifdef STAT
#include <iostream>
static uint64_t snp_distantce;
static uint64_t t_tmax_distance;
#endif

namespace checker {

namespace {

inline void printStat(uint64_t txn_num) {
#ifdef STAT
  double average_snp_dist    = (double)snp_distantce / txn_num;
  double average_t_tmax_dist = (double)t_tmax_distance / txn_num;
  std::cout << "snp_distance: " << snp_distantce << std::endl;
  std::cout << "t_tmax_distance: " << t_tmax_distance << std::endl;
  std::cout << "txn_num: " << txn_num << std::endl;
  std::cout << "average_snp_dist: " << average_snp_dist << std::endl;
  std::cout << "average_t_tmax_dist: " << average_t_tmax_dist << std::endl;
#endif
}

void collectFinalWrites(const Trace &trace, const Transaction &txn,
                        std::vector<EventId> &writes,
                        std::unordered_set<Key, HexValueHash> &seen) {
  writes.clear();
  seen.clear();
  for (auto it = txn.event_ids.rbegin(); it != txn.event_ids.rend(); it++) {
    const Event &e = trace.getEvent(*it);
    if (!isWrite(e) || !seen.insert(e.key).second) {
      continue;
    }
    writes.push_back(*it);
  }
}

inline std::optional<TxnId> getSOWWMax(const SOWWContext &ctx,
                                       SessionId session_id,
                                       const std::vector<EventId> &writes) {
  std::optional<TxnId> res = std::nullopt;
  auto it                  = ctx.latest_txns_by_session.find(session_id);
  if (it != ctx.latest_txns_by_session.end()) {
    res = it->second;
  }

  // find latest writer
  for (const auto eid : writes) {
    const Event &e = ctx.trace.getEvent(eid);
    const auto &it = ctx.latext_writer.find(e.key);
    if (it == ctx.latext_writer.end())
      continue;

    TxnId w = it->second;
    res     = (res == std::nullopt) ? w : std::max(w, *res);
  }

  return res;
}

void updateSnapshot(SOWWContext &ctx, const Transaction &txn,
                    const std::vector<EventId> &writes) {
  SOWWDelta d;
  for (const auto eid : writes) {
    const Event &e = ctx.trace.getEvent(eid);

    // update current snapshot
    d.push_back(DeltaEntry{eid, ctx.cur_snapshot.get(e.key)});
    ctx.cur_snapshot.set(e.key, e.value);

    // update latest writers
    ctx.latext_writer[e.key] = txn.id;
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
  std::vector<EventId> writes;
  std::unordered_set<Key, HexValueHash> seen_writes;

  for (const auto &txn : ctx.trace.transactions) {
    collectFinalWrites(ctx.trace, txn, writes, seen_writes);
    auto t_max = getSOWWMax(ctx, txn.session_id, writes);

    if (t_max == std::nullopt) {
      updateSnapshot(ctx, txn, writes);
      continue;
    }

#ifdef STAT
    t_tmax_distance += txn.id - *t_max;
#endif

    // build wrong_keys set
    std::unordered_set<Key, HexValueHash> wrong_keys;
    wrong_keys.reserve(txn.event_ids.size());
    SnapshotOpt reads;
    reads.data.reserve(txn.event_ids.size());
    for (const auto &eid : txn.event_ids) {
      const auto &event = ctx.trace.events[eid];
      if (isRead(event)) {
        auto [read_it, inserted] =
            reads.data.try_emplace(event.key, event.value);
        if (!inserted && read_it->second != event.value) {
          auto diagnostic = make_diagnostic(
              "inconsistent read of the same key in the same transaction", txn,
              event);
          result.diagnostics.push_back(diagnostic);
        }
        const auto expected_it = ctx.cur_snapshot.data.find(event.key);
        const bool expected_has_value =
            expected_it != ctx.cur_snapshot.data.end() &&
            expected_it->second.has_value();
        const bool is_wrong =
            (event.is_miss && expected_has_value) ||
            (!event.is_miss &&
             (!expected_has_value || expected_it->second != event.value));
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

#ifdef STAT
      snp_distantce++;
#endif

      id--;

      const auto &old_vals = ctx.cl[id].second;
      for (const auto &[eid, v] : old_vals) {
        const auto &k = ctx.trace.getEvent(eid).key;
        const auto read_it = reads.data.find(k);
        if (read_it == reads.data.end()) {
          continue;
        }
        if (read_it->second == v) {
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

    updateSnapshot(ctx, txn, writes);
  }

  result.status =
      result.diagnostics.empty() ? CheckStatus::Pass : CheckStatus::Fail;

  if (ctx.stat) {
    printStat(ctx.trace.transactions.size());
  }

  return result;
}

} // namespace checker
