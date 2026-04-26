#include "common/trace/trace_index.h"

#include <unordered_map>
#include <utility>

namespace common {

namespace {

Diagnostic index_error(std::string code, std::string message) {
  Diagnostic diagnostic;
  diagnostic.severity = Severity::Error;
  diagnostic.checker  = "index";
  diagnostic.code     = std::move(code);
  diagnostic.message  = std::move(message);
  return diagnostic;
}

} // namespace

Result<TraceIndex> TraceIndexBuilder::build(const Trace &trace) const {
  TraceIndex index;
  DiagnosticList diagnostics;

  for (const auto &txn : trace.transactions) {
    if (!index.txn_by_commit_ts.emplace(txn.commit_ts, txn.id).second) {
      auto diagnostic   = index_error("index.duplicate_commit_ts",
                                      "duplicate commit timestamp in trace");
      diagnostic.txn_id = txn.id;
      diagnostics.push_back(std::move(diagnostic));
      return Result<TraceIndex>::err(std::move(diagnostics));
    }

    std::unordered_map<Key, EventId, HexValueHash> final_write_by_key;
    for (const auto event_id : txn.event_ids) {
      if (event_id >= trace.events.size()) {
        auto diagnostic =
            index_error("index.invalid_event_id",
                        "transaction references an out-of-range event id");
        diagnostic.txn_id   = txn.id;
        diagnostic.event_id = event_id;
        diagnostics.push_back(std::move(diagnostic));
        return Result<TraceIndex>::err(std::move(diagnostics));
      }

      const auto &event = trace.events[event_id];
      if (event.txn_id != txn.id) {
        auto diagnostic     = index_error("index.event_owner_mismatch",
                                          "event owner does not match transaction");
        diagnostic.txn_id   = txn.id;
        diagnostic.event_id = event_id;
        diagnostics.push_back(std::move(diagnostic));
        return Result<TraceIndex>::err(std::move(diagnostics));
      }

      if (event.op != EventOp::Write) {
        continue;
      }

      final_write_by_key[event.key] = event.id;
    }

    for (const auto &[key, event_id] : final_write_by_key) {
      const auto &event = trace.events[event_id];
      Version version;
      version.writer      = txn.id;
      version.write_event = event.id;
      version.commit_ts   = txn.commit_ts;
      version.key         = key;
      version.value       = event.value;
      version.is_delete   = event.is_delete;
      index.versions_by_key[key].push_back(std::move(version));
    }
  }

  return Result<TraceIndex>::ok(std::move(index));
}

} // namespace common
