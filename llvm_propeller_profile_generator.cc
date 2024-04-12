#include "llvm_propeller_profile_generator.h"

#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "branch_aggregator.h"
#include "frequencies_branch_aggregator.h"
#include "lbr_branch_aggregator.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_file_perf_data_provider.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_perf_branch_frequencies_aggregator.h"
#include "llvm_propeller_perf_data_provider.h"
#include "llvm_propeller_perf_lbr_aggregator.h"
#include "llvm_propeller_profile.h"
#include "llvm_propeller_profile_computer.h"
#include "llvm_propeller_profile_writer.h"
#include "llvm_propeller_telemetry_reporter.h"
#include "status_consumer_registry.h"
#include "status_provider.h"
#include "third_party/abseil/absl/algorithm/container.h"
#include "third_party/abseil/absl/base/attributes.h"
#include "third_party/abseil/absl/base/nullability.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/functional/function_ref.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "base/logging.h"

namespace devtools_crosstool_autofdo
{
  namespace
  {
    // RegistryStopper calls "Stop" on all registered status consumers before
    // exiting.
    class RegistryStopper
    {
    public:
      ~RegistryStopper() { registry_.Stop(); }

      // Constructs a registry stopper from non-null pointers to a registry and a
      // status provider, both of which must outlive the call.
      explicit RegistryStopper(absl::Nonnull<StatusConsumerRegistry *> registry
                                   ABSL_ATTRIBUTE_LIFETIME_BOUND,
                               absl::Nonnull<MultiStatusProvider *> status_provider
                                   ABSL_ATTRIBUTE_LIFETIME_BOUND)
          : registry_(*registry)
      {
        registry_.Start(*status_provider);
      }

      // RegistryStopper is neither copyable nor movable.
      RegistryStopper(const RegistryStopper &) = delete;
      RegistryStopper &operator=(const RegistryStopper &) = delete;

    private:
      StatusConsumerRegistry &registry_;
    };

    // The status providers for different phases of Propeller profile generation
    struct StatusProviders
    {
      absl::Nonnull<DefaultStatusProvider *> binary_reading;
      absl::Nonnull<DefaultStatusProvider *> profile_mapping;
      absl::Nonnull<DefaultStatusProvider *> code_layout;
      absl::Nonnull<DefaultStatusProvider *> file_writing;
    };

    // Adds status providers for each phase of Propeller profile generation to
    // `main_status` and returns references to the status provider of each phase.
    StatusProviders AddStatusProviders(
        MultiStatusProvider &main_status ABSL_ATTRIBUTE_LIFETIME_BOUND)
    {
      auto read_binary_status = std::make_unique<DefaultStatusProvider>(
          "read binary and the bb-address-map section");
      auto map_profile_status = std::make_unique<DefaultStatusProvider>(
          "map profiles to control flow graphs");
      auto code_layout_status =
          std::make_unique<DefaultStatusProvider>("codelayout");
      auto write_file_status =
          std::make_unique<DefaultStatusProvider>("result_writer");

      StatusProviders statuses = {
          .binary_reading = read_binary_status.get(),
          .profile_mapping = map_profile_status.get(),
          .code_layout = code_layout_status.get(),
          .file_writing = write_file_status.get(),
      };

      main_status.AddStatusProvider(10, std::move(read_binary_status));
      main_status.AddStatusProvider(500, std::move(map_profile_status));
      main_status.AddStatusProvider(50, std::move(code_layout_status));
      main_status.AddStatusProvider(1, std::move(write_file_status));

      return statuses;
    }

    // Determines the type of the provided input profiles, returning an error if
    // profile types are heterogeneous. For backwards compatibility reasons, assumes
    // that unspecified profile types are PERF_LBR.
    absl::StatusOr<ProfileType> GetProfileType(const PropellerOptions &opts)
    {
      if (opts.input_profiles().empty())
        return absl::InvalidArgumentError("no input profiles provided");

      absl::flat_hash_set<ProfileType> profile_types;
      absl::c_transform(
          opts.input_profiles(), std::inserter(profile_types, profile_types.end()),
          [](const InputProfile &profile)
          {
            if (profile.type() == ProfileType::PROFILE_TYPE_UNSPECIFIED)
              return ProfileType::PERF_LBR;
            return profile.type();
          });

      if (profile_types.size() > 1)
      {
        return absl::InvalidArgumentError("heterogeneous profile types");
      }

      return *profile_types.begin();
    }

    // Creates a perf data provider for the perf files in `opts.input_profiles`.
    // Assumes that all input profile types are Perf LBR/SPE or unspecified.
    std::unique_ptr<GenericFilePerfDataProvider> CreatePerfDataProvider(
        const PropellerOptions &opts)
    {
      std::vector<std::string> profile_names;
      absl::c_transform(opts.input_profiles(), std::back_inserter(profile_names),
                        [](const InputProfile &profile)
                        { return profile.name(); });

      return std::make_unique<GenericFilePerfDataProvider>(
          std::move(profile_names));
    }

    // Generates propeller profiles for the provided options. Obtains a
    // BranchAggregator from `create_aggregator` and uses it to compute the profile.
    absl::Status GeneratePropellerProfiles(
        const PropellerOptions &opts,
        absl::FunctionRef<absl::StatusOr<std::unique_ptr<BranchAggregator>>(
            PropellerOptions, const BinaryContent &)>
            create_aggregator)
    {
      MultiStatusProvider main_status("generate propeller profiles");

      // Because the stopper "stops" consumers that have a reference to
      // main_status, we must declared "stopper" after "main_status" so "stopper" is
      // deleted before "main_status".
      std::optional<RegistryStopper> stopper;
      if (opts.http())
      {
        stopper.emplace(/*registry=*/&devtools_crosstool_autofdo::
                            StatusConsumerRegistry::GetInstance(),
                        /*status_provider=*/&main_status);
      }

      StatusProviders statuses = AddStatusProviders(main_status);

      ASSIGN_OR_RETURN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(opts.binary_name()));
      statuses.binary_reading->SetDone();

      ASSIGN_OR_RETURN(std::unique_ptr<BranchAggregator> aggregator,
                       create_aggregator(opts, *binary_content));
      ASSIGN_OR_RETURN(
          std::unique_ptr<PropellerProfileComputer> profile_computer,
          PropellerProfileComputer::Create(opts, std::move(aggregator)));

      ASSIGN_OR_RETURN(PropellerProfile profile,
                       profile_computer->ComputeProfile(statuses.profile_mapping,
                                                        statuses.code_layout));

      PropellerProfileWriter(opts).Write(profile);
      statuses.file_writing->SetDone();
      LOG(INFO) << profile_computer->stats().DebugString();
      InvokePropellerTelemetryReporters(profile_computer->binary_content(),
                                        profile_computer->stats());
      return absl::OkStatus();
    }
  } // namespace

  absl::Status GeneratePropellerProfiles(
      const PropellerOptions &opts)
  {
    if (!opts.profile_names().empty())
      return absl::InvalidArgumentError("profile_names is deprecated");

    ASSIGN_OR_RETURN(ProfileType profile_type, GetProfileType(opts));

    return GeneratePropellerProfiles(opts, CreatePerfDataProvider(opts),
                                     profile_type);
  }

  absl::Status GeneratePropellerProfiles(
      const PropellerOptions &opts,
      std::unique_ptr<PerfDataProvider> perf_data_provider,
      ProfileType profile_type)
  {
    if (profile_type == ProfileType::PERF_LBR)
    {
      return GeneratePropellerProfiles(opts, [&perf_data_provider](
                                                 PropellerOptions opts,
                                                 const BinaryContent &content)
                                       { return std::make_unique<LbrBranchAggregator>(
                                             std::make_unique<PerfLbrAggregator>(std::move(perf_data_provider)),
                                             opts, std::move(content)); });
    }
    if (profile_type == ProfileType::PERF_SPE)
    {
      return GeneratePropellerProfiles(
          opts, [&perf_data_provider](PropellerOptions opts,
                                      const BinaryContent &content)
          { return std::make_unique<FrequenciesBranchAggregator>(
                std::make_unique<PerfBranchFrequenciesAggregator>(
                    std::move(perf_data_provider)),
                opts, content); });
    }
    return absl::InvalidArgumentError(
        absl::StrCat("unsupported profile type ", profile_type));
  }

} // namespace devtools_crosstool_autofdo
