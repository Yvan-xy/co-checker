#include "checker/so_only/so_cl_checker.h"

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

std::optional<TxnId> getSOMax(const SOCLContext &ctx, SessionId session_id) {
  auto it = ctx.latest_txns_by_session.find(session_id);
  if (it == ctx.latest_txns_by_session.end()) {
    return std::nullopt;
  }
  return it->second;
}

void updateSnapshot(SOCLContext &ctx, const Transaction &txn) {
  Delta d;
  std::unordered_set<Key, HexValueHash> updated_keys;
  for (auto it = txn.event_ids.rbegin(); it != txn.event_ids.rend(); it++) {
    const Event &e = ctx.trace.getEvent(*it);
    if (isWrite(e)) {
      // not the latest write on e.key
      if (updated_keys.contains(e.key))
        continue;
      updated_keys.insert(e.key);
      d.push_back(std::make_pair(e.key, ctx.cur_snapshot.get(e.key)));
      ctx.cur_snapshot.set(e.key, e.value);
    }
  }
  ctx.cl.push_back(std::make_pair(txn.id, d));
  ctx.latest_txns_by_session[txn.session_id] = txn.id;
  return;
}

} // namespace

std::string_view SnapshotSOCLChecker::name() const { return "snapshot-so-cl"; }

common::Diagnostic
SnapshotSOCLChecker::make_diagnostic(std::string message,
                                     const common::Transaction &txn,
                                     const common::Event &event) const {
  auto diagnostic    = Checker::make_diagnostic(std::move(message), txn, event);
  diagnostic.checker = "snapshot-so-cl";
  return diagnostic;
}

CheckResult SnapshotSOCLChecker::run(const CheckerContext &ctx_) const {
  CheckResult result;
  SOCLContext ctx(std::move(ctx_));
  for (const auto &txn : ctx.trace.transactions) {
    auto t_max = getSOMax(ctx, txn.session_id);

    if (t_max == std::nullopt) {
      updateSnapshot(ctx, txn);
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

#ifdef STAT
      snp_distantce++;
#endif
      id--;

      const auto &old_vals = ctx.cl[id].second;
      for (const auto &[k, v] : old_vals) {
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

  if (ctx.stat) {
    printStat(ctx.trace.transactions.size());
  }

  return result;
}

} // namespace checker
