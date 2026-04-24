#pragma once

#include <iosfwd>

#include "common/diag/diagnostic.h"
#include "common/trace/trace.h"
#include "common/trace/trace_index.h"

namespace common {

void print_diagnostic(std::ostream &os, const Diagnostic &diagnostic);
void print_diagnostics(std::ostream &os, const DiagnosticList &diagnostics);
void dump_parsed_trace(std::ostream &os, const Trace &trace);
void dump_versions(std::ostream &os, const TraceIndex &index);

} // namespace common
