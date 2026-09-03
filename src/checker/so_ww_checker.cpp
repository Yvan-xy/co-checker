#include "checker/so_ww/so_ww_checker.h"

#include <algorithm>
#include <cassert>
#include <cstdint>

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
                        std::vector<std::uint32_t> &seen_write_epochs,
                        const std::uint32_t epoch) {
  writes.clear();
  for (auto it = txn.event_ids.rbegin(); it != txn.event_ids.rend(); it++) {
    const Event &e = trace.getEvent(*it);
    if (!isWrite(e)) {
      continue;
    }
    const auto key_id = trace.getEventKeyId(*it);
    if (seen_write_epochs[key_id] == epoch) {
      continue;
    }
    seen_write_epochs[key_id] = epoch;
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

bool writesCurrentWriteKey(const SOWWDelta &delta,
                           const std::vector<std::uint32_t> &seen_write_epochs,
                           const std::uint32_t epoch) {
  for (const auto &[key_id, old_value_id] : delta) {
    (void)old_value_id;
    if (seen_write_epochs[key_id] == epoch) {
      return true;
    }
  }
  return false;
}

#ifdef STAT
std::optional<TxnId>
getSOWWMaxForStats(const SOWWContext &ctx, const std::optional<TxnId> so_max,
                   const std::vector<std::uint32_t> &seen_write_epochs,
                   const std::uint32_t epoch) {
  auto res = so_max;
  for (auto it = ctx.cl.rbegin(); it != ctx.cl.rend(); ++it) {
    if (so_max.has_value() && it->first <= *so_max) {
      break;
    }
    if (writesCurrentWriteKey(it->second, seen_write_epochs, epoch)) {
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
  d.reserve(writes.size());
  for (const auto eid : writes) {
    const auto key_id = ctx.trace.getEventKeyId(eid);

    d.push_back(DeltaEntry{key_id, ctx.cur_snapshot[key_id]});
    ctx.cur_snapshot[key_id] = ctx.trace.getEventValueId(eid);
  }
  ctx.cl.emplace_back(txn.id, std::move(d));
  ctx.latest_txns_by_session[txn.session_id] = txn.id;
}

bool hasDenseIds(const Trace &trace) {
  return trace.event_key_ids.size() == trace.events.size() &&
         trace.event_value_ids.size() == trace.events.size();
}

CheckResult missingDenseIdsResult() {
  CheckResult result;
  result.status = CheckStatus::Inconclusive;
  Diagnostic diagnostic;
  diagnostic.severity = Severity::Error;
  diagnostic.checker  = "snapshot-so-ww";
  diagnostic.message  = "trace is missing dense key/value IDs";
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

void useDenseKeyDiagnostic(Diagnostic &diagnostic, const Trace &trace,
                           const KeyId key_id) {
  if (trace.has_raw_payload) {
    return;
  }
  diagnostic.key.reset();
  diagnostic.key_id = key_id;
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
  if (!hasDenseIds(ctx_.trace)) {
    return missingDenseIdsResult();
  }

  CheckResult result;
  SOWWContext ctx(std::move(ctx_));
  ctx.cur_snapshot.assign(ctx.trace.key_count, kNullValueId);
  ctx.cl.reserve(ctx.trace.transactions.size());
  ctx.latest_txns_by_session.reserve(ctx.trace.transactions.size());
  std::vector<EventId> writes;
  std::vector<std::uint32_t> seen_write_epochs(ctx.trace.key_count, 0);
  std::vector<std::uint32_t> read_epochs(ctx.trace.key_count, 0);
  std::vector<std::uint32_t> wrong_epochs(ctx.trace.key_count, 0);
  std::vector<ValueId> read_values(ctx.trace.key_count, kNullValueId);
  std::vector<EventId> read_events(ctx.trace.key_count, 0);
  std::vector<KeyId> touched_read_keys;

  for (const auto &txn : ctx.trace.transactions) {
    const auto epoch = static_cast<std::uint32_t>(txn.id + 1U);
    collectFinalWrites(ctx.trace, txn, writes, seen_write_epochs, epoch);
    auto so_max = getSOMax(ctx, txn.session_id);

    if (so_max == std::nullopt && writes.empty()) {
      updateSnapshot(ctx, txn, writes);
      continue;
    }

#ifdef STAT
    if (ctx.stat) {
      const auto t_max =
          getSOWWMaxForStats(ctx, so_max, seen_write_epochs, epoch);
      if (t_max.has_value()) {
        t_tmax_distance += txn.id - *t_max;
      }
    }
#endif

    std::vector<Diagnostic> pending_read_diagnostics;
    touched_read_keys.clear();
    touched_read_keys.reserve(txn.event_ids.size());
    std::size_t wrong_key_count = 0;
    for (const auto &eid : txn.event_ids) {
      const auto &event = ctx.trace.events[eid];
      if (isRead(event)) {
        const auto key_id   = ctx.trace.getEventKeyId(eid);
        const auto value_id = ctx.trace.getEventValueId(eid);
        if (read_epochs[key_id] != epoch) {
          read_epochs[key_id] = epoch;
          read_values[key_id] = value_id;
          read_events[key_id] = eid;
          touched_read_keys.push_back(key_id);
        } else if (read_values[key_id] != value_id) {
          auto diagnostic = make_diagnostic(
              "inconsistent read of the same key in the same transaction", txn,
              event);
          useDenseKeyDiagnostic(diagnostic, ctx.trace, key_id);
          pending_read_diagnostics.push_back(std::move(diagnostic));
        }
        const auto expected_id        = ctx.cur_snapshot[key_id];
        const bool expected_has_value = expected_id != kNullValueId;
        const bool observed_has_value = value_id != kNullValueId;
        const bool is_wrong =
            event.is_miss ? expected_has_value
                          : (!expected_has_value || !observed_has_value ||
                             expected_id != value_id);
        if (is_wrong && wrong_epochs[key_id] != epoch) {
          wrong_epochs[key_id] = epoch;
          ++wrong_key_count;
        }
      }
    }

    bool viable          = false;
    bool has_lower_bound = so_max.has_value();
    auto id              = ctx.cl.size();
    while (true) {
      if (wrong_key_count == 0) {
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
      if (writesCurrentWriteKey(old_vals, seen_write_epochs, epoch)) {
        has_lower_bound = true;
        break;
      }

#ifdef STAT
      snp_distantce++;
#endif

      id--;
      for (const auto &[key_id, old_value_id] : old_vals) {
        if (read_epochs[key_id] != epoch) {
          continue;
        }
        const bool is_wrong = read_values[key_id] != old_value_id;
        if (is_wrong && wrong_epochs[key_id] != epoch) {
          wrong_epochs[key_id] = epoch;
          ++wrong_key_count;
        } else if (!is_wrong && wrong_epochs[key_id] == epoch) {
          wrong_epochs[key_id] = 0;
          --wrong_key_count;
        }
      }
    }

    if (has_lower_bound) {
      result.diagnostics.insert(result.diagnostics.end(),
                                pending_read_diagnostics.begin(),
                                pending_read_diagnostics.end());
    }

    if (!viable) {
      for (const auto key_id : touched_read_keys) {
        if (wrong_epochs[key_id] != epoch) {
          continue;
        }
        auto diagnostic = make_diagnostic("Violation detected ", txn,
                                          ctx.trace.getEvent(txn.event_ids[0]));
        if (ctx.trace.has_raw_payload) {
          diagnostic.key = ctx.trace.getEvent(read_events[key_id]).key;
        } else {
          diagnostic.key.reset();
          diagnostic.key_id = key_id;
        }
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
