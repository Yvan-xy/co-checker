#include "checker/basic/co_snapshot_checker.h"

#include <algorithm>
#include <string>

namespace common = common;

namespace checker {

namespace {

common::Diagnostic make_diagnostic(std::string code, std::string message,
                                   const common::Transaction &txn,
                                   const common::Event &event) {
  common::Diagnostic diagnostic;
  diagnostic.severity  = common::Severity::Error;
  diagnostic.checker   = "co-si";
  diagnostic.code      = std::move(code);
  diagnostic.message   = std::move(message);
  diagnostic.txn_id    = txn.id;
  diagnostic.event_id  = event.id;
  diagnostic.event_seq = event.seq;
  diagnostic.key       = event.key;
  return diagnostic;
}

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

std::string_view CoSnapshotBasicChecker::name() const { return "co-si-basic"; }

CheckResult CoSnapshotBasicChecker::run(const CheckerContext &ctx) const {
  CheckResult result;

  for (const auto &event : ctx.trace.events) {
    if (event.op != common::EventOp::Read) {
      continue;
    }

    if (event.txn_id >= ctx.trace.transactions.size()) {
      common::Diagnostic diagnostic;
      diagnostic.severity = common::Severity::Error;
      diagnostic.checker  = "co-si";
      diagnostic.code     = "co_si.invalid_event_owner";
      diagnostic.message  = "read event references an out-of-range transaction";
      diagnostic.event_id = event.id;
      diagnostic.event_seq = event.seq;
      diagnostic.key       = event.key;
      result.diagnostics.push_back(std::move(diagnostic));
      continue;
    }

    const auto &txn                = ctx.trace.transactions[event.txn_id];
    const auto versions_it         = ctx.index.versions_by_key.find(event.key);
    const common::Version *visible = nullptr;
    if (versions_it != ctx.index.versions_by_key.end()) {
      visible = find_visible_version(versions_it->second, txn.start_ts);
    }

    if (visible == nullptr || visible->is_delete) {
      if (!event.is_miss) {
        result.diagnostics.push_back(make_diagnostic(
            "co_si.unexpected_value",
            "read returned a value, but no visible committed version exists",
            txn, event));
      }
      continue;
    }

    if (!visible->value.has_value()) {
      result.diagnostics.push_back(make_diagnostic(
          "co_si.invalid_version", "visible non-delete version has no value",
          txn, event));
      continue;
    }

    if (event.is_miss) {
      result.diagnostics.push_back(make_diagnostic(
          "co_si.unexpected_miss",
          "read missed a key with a visible committed version", txn, event));
      continue;
    }

    if (!event.value.has_value()) {
      result.diagnostics.push_back(
          make_diagnostic("co_si.missing_read_value",
                          "read did not include value_hex", txn, event));
      continue;
    }

    if (*event.value != *visible->value) {
      result.diagnostics.push_back(make_diagnostic(
          "co_si.stale_read",
          "read value does not match latest visible committed version", txn,
          event));
    }
  }

  result.status =
      result.diagnostics.empty() ? CheckStatus::Pass : CheckStatus::Fail;
  return result;
}

} // namespace checker
