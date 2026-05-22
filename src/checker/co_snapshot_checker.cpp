#include "checker/basic/co_snapshot_checker.h"

#include <algorithm>
#include <sstream>
#include <string>

#include "common/trace/trace_index.h"

namespace checker {

namespace {

const common::Version *
find_visible_version(const std::vector<common::Version> &versions,
                     const common::Timestamp start_ts) {
  const auto it = std::upper_bound(
      versions.begin(), versions.end(), start_ts,
      [](const common::Timestamp ts, const common::Version &version) {
        return ts < version.commit_ts;
      });

  if (it == versions.begin()) {
    return nullptr;
  }
  return &*std::prev(it);
}

} // namespace

common::Diagnostic
CoSnapshotBasicChecker::make_diagnostic(std::string message,
                                        const common::Transaction &txn,
                                        const common::Event &event) const {
  auto diagnostic    = Checker::make_diagnostic(std::move(message), txn, event);
  diagnostic.checker = "co-si-basic";
  return diagnostic;
}

std::string_view CoSnapshotBasicChecker::name() const { return "co-si-basic"; }

CheckResult CoSnapshotBasicChecker::run(const CheckerContext &ctx) const {
  CheckResult result;
  common::TraceIndexBuilder index_builder;
  auto index = index_builder.build(ctx.trace);
  if (!index) {
    result.status      = CheckStatus::Fail;
    result.diagnostics = index.error();
    return result;
  }
  const auto &trace_index = index.value();

  for (const auto &[key, versions] : trace_index.versions_by_key) {
    // Version chains are in commit order. The immediately previous writer is
    // enough to detect whether any same-key writer committed after this
    // transaction's snapshot started.
    for (std::size_t i = 1; i < versions.size(); ++i) {
      const auto &previous = versions[i - 1];
      const auto &current  = versions[i];
      const auto &txn      = ctx.trace.transactions[current.writer];

      if (previous.commit_ts <= txn.start_ts) {
        continue;
      }

      std::ostringstream message;
      message << "write-write conflict with overlapping committed transaction"
              << " previous_txn=" << previous.writer
              << " previous_commit_ts=" << previous.commit_ts
              << " current_start_ts=" << txn.start_ts;

      auto diagnostic = make_diagnostic(message.str(), txn,
                                        ctx.trace.events[current.write_event]);
      diagnostic.key  = key;
      result.diagnostics.push_back(std::move(diagnostic));
    }
  }

  for (const auto &event : ctx.trace.events) {
    if (event.op != common::EventOp::Read) {
      continue;
    }

    if (event.txn_id >= ctx.trace.transactions.size()) {
      common::Diagnostic diagnostic;
      diagnostic.severity = common::Severity::Error;
      diagnostic.checker  = "co-si";
      diagnostic.message  = "read event references an out-of-range transaction";
      diagnostic.event_id = event.id;
      diagnostic.event_seq = event.seq;
      diagnostic.key       = event.key;
      result.diagnostics.push_back(std::move(diagnostic));
      continue;
    }

    const auto &txn        = ctx.trace.transactions[event.txn_id];
    const auto versions_it = trace_index.versions_by_key.find(event.key);
    const common::Version *visible = nullptr;
    if (versions_it != trace_index.versions_by_key.end()) {
      visible = find_visible_version(versions_it->second, txn.start_ts);
    }

    if (visible == nullptr || visible->is_delete) {
      if (!event.is_miss) {
        result.diagnostics.push_back(make_diagnostic(
            "read returned a value, but no visible committed version exists",
            txn, event));
      }
      continue;
    }

    if (!visible->value.has_value()) {
      result.diagnostics.push_back(make_diagnostic(
          "visible non-delete version has no value", txn, event));
      continue;
    }

    if (event.is_miss) {
      result.diagnostics.push_back(make_diagnostic(
          "read missed a key with a visible committed version", txn, event));
      continue;
    }

    if (!event.value.has_value()) {
      result.diagnostics.push_back(
          make_diagnostic("read did not include value_hex", txn, event));
      continue;
    }

    if (*event.value != *visible->value) {
      result.diagnostics.push_back(make_diagnostic(
          "read value does not match latest visible committed version", txn,
          event));
    }
  }

  result.status =
      result.diagnostics.empty() ? CheckStatus::Pass : CheckStatus::Fail;
  return result;
}

} // namespace checker
