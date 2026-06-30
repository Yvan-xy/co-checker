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

std::optional<TxnId> getSOMax(const SOWWContext &ctx, SessionId session_id) {
  auto it = ctx.latest_txns_by_session.find(session_id);
  if (it != ctx.latest_txns_by_session.end()) {
    return it->second;
  }
  return std::nullopt;
}

bool writesCurrentWriteKey(
    const Trace &trace, const SOWWDelta &delta,
    const std::unordered_set<Key, HexValueHash> &current_write_keys) {
  if (current_write_keys.empty()) {
    return false;
  }

  for (const auto &[eid, old_value] : delta) {
    (void)old_value;
    if (current_write_keys.contains(trace.getEvent(eid).key)) {
      return true;
    }
  }
  return false;
}

#ifdef STAT
std::optional<TxnId> getSOWWMaxForStats(
    const SOWWContext &ctx, const std::optional<TxnId> so_max,
    const std::unordered_set<Key, HexValueHash> &current_write_keys) {
  auto res = so_max;
  for (auto it = ctx.cl.rbegin(); it != ctx.cl.rend(); ++it) {
    if (so_max.has_value() && it->first <= *so_max) {
      break;
    }
    if (writesCurrentWriteKey(ctx.trace, it->second, current_write_keys)) {
      res = it->first;
      break;
    }
  }
  return res;
}
#endif

void updateSnapshot(SOWWContext &ctx, const Transaction &txn,
                    const std::vector<EventId> &writes) {
  SOWWDelta d;
  for (const auto eid : writes) {
    const Event &e = ctx.trace.getEvent(eid);

    // update current snapshot
    d.push_back(DeltaEntry{eid, ctx.cur_snapshot.get(e.key)});
    ctx.cur_snapshot.set(e.key, e.value);
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
  std::vector<EventId> writes;
  std::unordered_set<Key, HexValueHash> seen_writes;

  for (const auto &txn : ctx.trace.transactions) {
    collectFinalWrites(ctx.trace, txn, writes, seen_writes);
    auto so_max = getSOMax(ctx, txn.session_id);

    if (so_max == std::nullopt && writes.empty()) {
      updateSnapshot(ctx, txn, writes);
      continue;
    }

#ifdef STAT
    if (ctx.stat) {
      const auto t_max = getSOWWMaxForStats(ctx, so_max, seen_writes);
      if (t_max.has_value()) {
        t_tmax_distance += txn.id - *t_max;
      }
    }
#endif

    // build wrong_keys set
    std::vector<Diagnostic> pending_read_diagnostics;
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
          pending_read_diagnostics.push_back(std::move(diagnostic));
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

    bool viable          = false;
    bool has_lower_bound = so_max.has_value();
    auto id              = ctx.cl.size();
    while (true) {
      if (wrong_keys.empty()) {
        viable = true;
        break;
      }

      if (id == 0) {
        viable = so_max == std::nullopt;
        break;
      }

      const auto &[rollback_txn_id, old_vals] = ctx.cl[id - 1];
      if (so_max.has_value() && rollback_txn_id == *so_max) {
        break;
      }
      if (writesCurrentWriteKey(ctx.trace, old_vals, seen_writes)) {
        has_lower_bound = true;
        break;
      }

#ifdef STAT
      snp_distantce++;
#endif

      id--;
      for (const auto &[eid, v] : old_vals) {
        const auto &k      = ctx.trace.getEvent(eid).key;
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

    if (has_lower_bound) {
      result.diagnostics.insert(result.diagnostics.end(),
                                pending_read_diagnostics.begin(),
                                pending_read_diagnostics.end());
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
