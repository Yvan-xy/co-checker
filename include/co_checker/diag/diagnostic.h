#pragma once

#include <optional>
#include <string>
#include <vector>

#include "co_checker/trace/types.h"

namespace co_checker {

enum class Severity {
    Info,
    Warning,
    Error,
};

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string checker;
    std::string code;
    std::string message;
    std::optional<TxnId> txn_id;
    std::optional<EventId> event_id;
    std::optional<EventSeq> event_seq;
    std::optional<Key> key;
};

using DiagnosticList = std::vector<Diagnostic>;

[[nodiscard]] const char* to_string(Severity severity);

}  // namespace co_checker
