#include "llvm_propeller_telemetry_reporter.h"

#include <utility>
#include <vector>

namespace devtools_crosstool_autofdo {
namespace {
// Returns the global registry of Propeller telemetry reporters.
std::vector<PropellerTelemetryReporter>& GetPropellerTelemetryReporters() {
  static auto* const reporters = new std::vector<PropellerTelemetryReporter>;
  return *reporters;
}
}  // namespace

void RegisterPropellerTelemetryReporter(PropellerTelemetryReporter reporter) {
  GetPropellerTelemetryReporters().push_back(std::move(reporter));
}

void InvokePropellerTelemetryReporters(const BinaryContent& binary_content,
                                       const PropellerStats& propeller_stats) {
  for (const PropellerTelemetryReporter& reporter :
       GetPropellerTelemetryReporters()) {
    reporter(binary_content, propeller_stats);
  }
}

void UnregisterAllPropellerTelemetryReportersForTest() {
  GetPropellerTelemetryReporters().clear();
}
}  // namespace devtools_crosstool_autofdo
