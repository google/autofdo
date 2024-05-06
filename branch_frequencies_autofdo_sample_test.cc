#include "branch_frequencies_autofdo_sample.h"

#include <cstdint>

#include "binary_address_branch.h"
#include "branch_frequencies.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"
#include "util/testing/status_matchers.h"

namespace devtools_crosstool_autofdo {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::IsEmpty;
using ::testing::IsSupersetOf;
using ::testing::Pair;
using ::testing::UnorderedElementsAre;
using ::testing::status::IsOkAndHolds;

TEST(BranchFrequenciesToAutofdoSample, ConvertsBranches) {
  EXPECT_THAT(
      ConvertBranchFrequenciesToAutofdoRangeSample(
          BranchFrequencies{
              .taken_branch_counters = {{{.from = 100, .to = 200}, 100},
                                        {{.from = 128, .to = 300}, 2000}}},
          [](int64_t address) -> absl::StatusOr<bool> { return true; },
          [](int64_t address) -> int64_t { return address; },
          /*instruction_size=*/4),
      IsOkAndHolds(Field("branch_counts", &AutofdoRangeSample::branch_counts,
                         UnorderedElementsAre(Pair(Pair(100, 200), 100),
                                              Pair(Pair(128, 300), 2000)))));
}

TEST(BranchFrequenciesToAutofdoSample, TranslatesAddresses) {
  EXPECT_THAT(
      ConvertBranchFrequenciesToAutofdoRangeSample(
          BranchFrequencies{
              .taken_branch_counters = {{{.from = 100, .to = 200}, 100}}},
          [](int64_t address) -> absl::StatusOr<bool> {
            return address == 100;
          },
          [](int64_t address) -> int64_t { return address + 1000; },
          /*instruction_size=*/4),
      IsOkAndHolds(Field("branch_counts", &AutofdoRangeSample::branch_counts,
                         ElementsAre(Pair(Pair(1100, 1200), 100)))));
}

TEST(BranchFrequenciesToAutofdoSample, PropagatesCounters) {
  EXPECT_THAT(
      ConvertBranchFrequenciesToAutofdoRangeSample(
          BranchFrequencies{
              .taken_branch_counters = {{{.from = 12, .to = 1000}, 500},
                                        {{.from = 12, .to = 1200}, 500},
                                        {{.from = 1300, .to = 12}, 1000}}},
          [](int64_t address) -> absl::StatusOr<bool> {
            absl::flat_hash_set<int64_t> kBranches = {12, 1300};
            return kBranches.contains(address);
          },
          [](int64_t address) -> int64_t { return address; },
          /*instruction_size=*/4),
      IsOkAndHolds(Field("range_counts", &AutofdoRangeSample::range_counts,
                         UnorderedElementsAre(Pair(Pair(12, 12), 1000),
                                              Pair(Pair(1000, 1196), 500),
                                              Pair(Pair(1200, 1300), 1000)))));
}

TEST(BranchFrequenciesToAutofdoSample, RespectsInstructionSize) {
  EXPECT_THAT(
      ConvertBranchFrequenciesToAutofdoRangeSample(
          BranchFrequencies{
              .taken_branch_counters = {{{.from = 1000, .to = 100}, 100},
                                        {{.from = 200, .to = 1000}, 50},
                                        {{.from = 300, .to = 1000}, 50}},
              .not_taken_branch_counters = {{{.address = 200}, 50}}},
          [](int64_t address) -> absl::StatusOr<bool> {
            return address == 200 || address == 300;
          },
          [](int64_t address) -> int64_t { return address; },
          /*instruction_size=*/1),
      IsOkAndHolds(Field("range_counts", &AutofdoRangeSample::range_counts,

                         IsSupersetOf({Pair(Pair(100, 200), 100),
                                       Pair(Pair(201, 300), 50)}))));
}

TEST(BranchFrequenciesToAutofdoSample, OverscansBranchTargets) {
  EXPECT_THAT(
      ConvertBranchFrequenciesToAutofdoRangeSample(
          BranchFrequencies{
              .taken_branch_counters = {{{.from = 100, .to = 1000}, 100}}},
          [](int64_t address) -> absl::StatusOr<bool> {
            return address == 100;
          },
          [](int64_t address) -> int64_t { return address; },
          /*instruction_size=*/1),
      IsOkAndHolds(Field("range_counts", &AutofdoRangeSample::range_counts,
                         ElementsAre(Pair(Pair(1000, 5095), 100)))));
}

TEST(BranchFrequenciesToAutofdoSample, HandlesInvalidAddresses) {
  EXPECT_THAT(
      ConvertBranchFrequenciesToAutofdoRangeSample(
          BranchFrequencies{
              .taken_branch_counters =
                  {{{.from = kInvalidBinaryAddress, .to = 100}, 100},
                   {{.from = 200, .to = kInvalidBinaryAddress}, 50},
                   {{.from = 300, .to = kInvalidBinaryAddress}, 50}},
              .not_taken_branch_counters = {{{.address = kInvalidBinaryAddress},
                                             1000},
                                            {{.address = 200}, 50}}},
          [](int64_t address) -> absl::StatusOr<bool> {
            if (address == 200 || address == 300) return true;
            if (address == kInvalidBinaryAddress)
              return absl::InvalidArgumentError("Invalid address");
            return false;
          },
          [](int64_t address) { return address; },
          /*instruction_size=*/4),
      IsOkAndHolds(AllOf(
          Field("branch_counts", &AutofdoRangeSample::branch_counts, IsEmpty()),
          Field("range_counts", &AutofdoRangeSample::range_counts,
                UnorderedElementsAre(Pair(Pair(100, 200), 100),
                                     Pair(Pair(204, 300), 50))))));
}

TEST(BranchFrequenciesToAutofdoSample, ConvertsProfiles) {
  EXPECT_THAT(
      ConvertBranchFrequenciesToAutofdoRangeSample(
          BranchFrequencies{
              .taken_branch_counters = {{{.from = 0x900, .to = 0x100}, 100},
                                        {{.from = 0x2FC, .to = 0x400}, 2000},
                                        {{.from = 0x3FC, .to = 0x500}, 8000},
                                        {{.from = 0x5FC, .to = 0x200}, 9900}},
              .not_taken_branch_counters = {{{.address = 0x1FC}, 100},
                                            {{.address = 0x2FC}, 8000},
                                            {{.address = 0x4FC}, 2000},
                                            {{.address = 0x5FC}, 100}}},
          [](int64_t address) -> absl::StatusOr<bool> {
            static const absl::flat_hash_set<int64_t> kBranches = {
                0x1FC, 0x2FC, 0x3FC, 0x5FC, 0x6FC, 0x900};
            return kBranches.contains(address);
          },
          [](int64_t address) -> int64_t { return address; },
          /*instruction_size=*/4),
      IsOkAndHolds(
          AllOf(Field("branch_counts", &AutofdoRangeSample::branch_counts,
                      UnorderedElementsAre(Pair(Pair(0x900, 0x100), 100),
                                           Pair(Pair(0x2FC, 0x400), 2000),
                                           Pair(Pair(0x3FC, 0x500), 8000),
                                           Pair(Pair(0x5FC, 0x200), 9900))),
                Field("range_counts", &AutofdoRangeSample::range_counts,
                      UnorderedElementsAre(Pair(Pair(0x100, 0x1FC), 100),
                                           Pair(Pair(0x200, 0x2FC), 10000),
                                           Pair(Pair(0x300, 0x3FC), 8000),
                                           Pair(Pair(0x400, 0x4FC), 2000),
                                           Pair(Pair(0x500, 0x5FC), 10000),
                                           Pair(Pair(0x600, 0x6FC), 100))))));
}

}  // namespace
}  // namespace devtools_crosstool_autofdo
