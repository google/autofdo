#include "llvm_propeller_profile_generator.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_file_perf_data_provider.h"
#include "llvm_propeller_perf_data_provider.h"
#include "llvm_propeller_profile.h"
#include "llvm_propeller_profile_computer.h"
#include "llvm_propeller_profile_writer.h"
#include "llvm_propeller_telemetry_reporter.h"
#include "status_consumer_registry.h"
#include "status_provider.h"
#include "third_party/abseil/absl/log/log.h"
#include "third_party/abseil/absl/memory/memory.h"
#include "third_party/abseil/absl/status/status.h"
#include "status_macros.h"

namespace devtools_crosstool_autofdo {
absl::Status GeneratePropellerProfiles(const PropellerOptions &opts) {
  return GeneratePropellerProfiles(
      opts, std::make_unique<FilePerfDataProvider>(std::vector<std::string>(
                opts.perf_names().begin(), opts.perf_names().end())));
}

absl::Status GeneratePropellerProfiles(
    const PropellerOptions &opts,
    std::unique_ptr<PerfDataProvider> perf_data_provider) {
  MultiStatusProvider main_status("generate propeller profiles");

  // RegistryStopper calls "Stop" on all registered status consumers before
  // exiting. Because the stopper "stops" consumers that have a reference to
  // main_status, we must declared "stopper" after "main_status" so "stopper" is
  // deleted before "main_status".
  struct RegistryStopper {
    ~RegistryStopper() {
      if (registry) registry->Stop();
    }
    StatusConsumerRegistry *registry = nullptr;
  } stopper;
  if (opts.http()) {
    stopper.registry =
        &devtools_crosstool_autofdo::StatusConsumerRegistry::GetInstance();
    stopper.registry->Start(main_status);
  }

  auto *read_binary_status =
      new DefaultStatusProvider("read binary and the bb-address-map section");
  main_status.AddStatusProvider(10, absl::WrapUnique(read_binary_status));
  auto *map_profile_status =
      new DefaultStatusProvider("map profiles to control flow graphs");
  main_status.AddStatusProvider(500, absl::WrapUnique(map_profile_status));
  auto *code_layout_status = new DefaultStatusProvider("codelayout");
  main_status.AddStatusProvider(50, absl::WrapUnique(code_layout_status));
  auto *write_file_status = new DefaultStatusProvider("result_writer");
  main_status.AddStatusProvider(1, absl::WrapUnique(write_file_status));

  ASSIGN_OR_RETURN(
      std::unique_ptr<PropellerProfileComputer> profile_computer,
      PropellerProfileComputer::Create(opts, std::move(perf_data_provider)));
  read_binary_status->SetDone();

  ASSIGN_OR_RETURN(
      PropellerProfile profile,
      profile_computer->ComputeProfile(map_profile_status, code_layout_status));

  PropellerProfileWriter(opts).Write(profile);
  write_file_status->SetDone();
  LOG(INFO) << profile_computer->stats().DebugString();
  InvokePropellerTelemetryReporters(profile_computer->binary_content(),
                                    profile_computer->stats());
  return absl::OkStatus();
}
}  // namespace devtools_crosstool_autofdo
