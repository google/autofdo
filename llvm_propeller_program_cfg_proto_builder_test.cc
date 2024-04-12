#include "llvm_propeller_program_cfg_proto_builder.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "llvm_propeller_cfg.pb.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_profile_computer.h"
#include "llvm_propeller_program_cfg.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "util/testing/status_matchers.h"

namespace devtools_crosstool_autofdo {
namespace {
TEST(LlvmPropellerProfileProtobufTest, AddCfgs) {
  const std::string binary =
      absl::StrCat(::testing::SrcDir(),
                   "/testdata/"
                   "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(::testing::SrcDir(),
                   "/testdata/"
                   "propeller_sample.perfdata");
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<PropellerProfileComputer> profile_computer,
      PropellerProfileComputer::Create(
          PropellerOptionsBuilder().SetBinaryName(binary).AddInputProfiles(
              InputProfileBuilder().SetName(perfdata))));

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<ProgramCfg> program_cfg,
                       profile_computer->GetProgramCfg());

  ProgramCfgProtoBuilder program_cfg_proto_builder;
  size_t number_of_cfgs = program_cfg->cfgs_by_index().size();
  program_cfg_proto_builder.AddCfgs(
      std::move(*std::move(program_cfg)).release_cfgs_by_index());
  EXPECT_GT(program_cfg_proto_builder.program_cfg_pb().cfg_size(), 0);
  EXPECT_EQ(program_cfg_proto_builder.program_cfg_pb().cfg_size(),
            number_of_cfgs);
}
}  // namespace
}  // namespace devtools_crosstool_autofdo
