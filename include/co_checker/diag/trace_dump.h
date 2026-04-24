#pragma once

#include <iosfwd>

#include "co_checker/checker/check_result.h"
#include "co_checker/diag/diagnostic.h"
#include "co_checker/trace/trace.h"
#include "co_checker/trace/trace_index.h"

namespace co_checker {

void print_diagnostic(std::ostream& os, const Diagnostic& diagnostic);
void print_diagnostics(std::ostream& os, const DiagnosticList& diagnostics);
void dump_parsed_trace(std::ostream& os, const Trace& trace);
void dump_versions(std::ostream& os, const TraceIndex& index);

}  // namespace co_checker
