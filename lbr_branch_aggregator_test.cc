#include "lbr_branch_aggregator.h"

#include <memory>
#include <utility>

#include "branch_aggregation.h"
#include "lbr_aggregation.h"
#include "lbr_aggregator.h"
#include "llvm_propeller_binary_address_mapper.h"
#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_statistics.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "util/testing/status_matchers.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/status/statusor.h"

namespace devtools_crosstool_autofdo {
namespace {
using ::testing::AllOf;
using ::testing::DoAll;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::FieldsAre;
using ::testing::Pair;
using ::testing::Return;
using ::testing::SetArgReferee;
using ::testing::UnorderedElementsAre;
using ::testing::status::IsOk;
using ::testing::status::IsOkAndHolds;
using ::testing::status::StatusIs;

class MockLbrAggregator : public LbrAggregator {
 public:
  MOCK_METHOD(absl::StatusOr<LbrAggregation>, AggregateLbrData,
              (const PropellerOptions& options,
               const BinaryContent& binary_content, PropellerStats& stats));
};

TEST(LbrBranchAggregator, GetBranchEndpointAddressesPropagatesErrors) {
  const PropellerOptions options;
  const BinaryContent binary_content;
  PropellerStats stats;
  auto mock_aggregator = std::make_unique<MockLbrAggregator>();
  EXPECT_CALL(*mock_aggregator, AggregateLbrData)
      .WillOnce(Return(absl::InternalError("")));

  EXPECT_THAT(
      LbrBranchAggregator(std::move(mock_aggregator), options, binary_content)
          .GetBranchEndpointAddresses(),
      StatusIs(absl::StatusCode::kInternal));
}

TEST(LbrBranchAggregator, GetBranchEndpointAddresses) {
  const PropellerOptions options;
  const BinaryContent binary_content;
  PropellerStats stats;

  EXPECT_THAT(
      LbrBranchAggregator({.branch_counters = {{{.from = 1, .to = 2}, 1},
                                               {{.from = 3, .to = 3}, 1}},
                           .fallthrough_counters = {{{.from = 3, .to = 3}, 1},
                                                    {{.from = 4, .to = 5}, 1}}})
          .GetBranchEndpointAddresses(),
      IsOkAndHolds(UnorderedElementsAre(1, 2, 3, 4, 5)));
}

TEST(LbrBranchAggregator, AggregatePropagatesErrors) {
  const PropellerOptions options;
  const BinaryContent binary_content;
  PropellerStats stats;
  BinaryAddressMapper binary_address_mapper(
      /*selected_functions=*/{}, /*bb_addr_map=*/{}, /*bb_handles=*/{},
      /*symbol_info_map=*/{});
  auto mock_aggregator = std::make_unique<MockLbrAggregator>();
  EXPECT_CALL(*mock_aggregator, AggregateLbrData)
      .WillOnce(Return(absl::InternalError("")));

  EXPECT_THAT(
      LbrBranchAggregator(std::move(mock_aggregator), options, binary_content)
          .Aggregate(binary_address_mapper, stats),
      StatusIs(absl::StatusCode::kInternal));
}

TEST(LbrBranchAggregator, ConvertsLbrAggregations) {
  const PropellerOptions options;
  const BinaryContent binary_content;
  PropellerStats stats;
  BinaryAddressMapper binary_address_mapper(
      /*selected_functions=*/{}, /*bb_addr_map=*/{}, /*bb_handles=*/{},
      /*symbol_info_map=*/{});

  auto mock_aggregator = std::make_unique<MockLbrAggregator>();
  EXPECT_CALL(*mock_aggregator, AggregateLbrData)
      .WillOnce(Return(LbrAggregation{
          .branch_counters = {{{.from = 1, .to = 2}, 3}},
          .fallthrough_counters = {{{.from = 4, .to = 5}, 6}},
      }));

  EXPECT_THAT(
      LbrBranchAggregator(std::move(mock_aggregator), options, binary_content)
          .Aggregate(binary_address_mapper, stats),
      IsOkAndHolds(
          AllOf(Field("branch_counters", &BranchAggregation::branch_counters,
                      ElementsAre(Pair(FieldsAre(1, 2), 3))),
                Field("fallthrough_counters",
                      &BranchAggregation::fallthrough_counters,
                      ElementsAre(Pair(FieldsAre(4, 5), 6))))));
}

TEST(LbrBranchAggregator, AggregatePropagatesStats) {
  const PropellerOptions options;
  const BinaryContent binary_content;
  PropellerStats stats;
  BinaryAddressMapper binary_address_mapper(
      /*selected_functions=*/{}, /*bb_addr_map=*/{}, /*bb_handles=*/{},
      /*symbol_info_map=*/{});

  auto mock_aggregator = std::make_unique<MockLbrAggregator>();
  EXPECT_CALL(*mock_aggregator, AggregateLbrData)
      .WillOnce(
          DoAll(SetArgReferee<2>(PropellerStats{
                    .profile_stats = {.binary_mmap_num = 1,
                                      .perf_file_parsed = 2,
                                      .br_counters_accumulated = 3},
                    .disassembly_stats = {.could_not_disassemble = {4, 5},
                                          .may_affect_control_flow = {6, 7},
                                          .cant_affect_control_flow = {8, 9}}}),

                Return(LbrAggregation{})));
  LbrBranchAggregator aggregator(std::move(mock_aggregator), options,
                                 binary_content);

  // Aggregate twice and check that the stats are doubled.
  EXPECT_THAT(aggregator.Aggregate(binary_address_mapper, stats), IsOk());
  EXPECT_THAT(aggregator.Aggregate(binary_address_mapper, stats), IsOk());

  EXPECT_THAT(
      stats,
      AllOf(Field("profile_stats", &PropellerStats::profile_stats,
                  FieldsAre(2, 4, 6)),
            Field("disassembly_stats", &PropellerStats::disassembly_stats,
                  FieldsAre(FieldsAre(8, 10), FieldsAre(12, 14),
                            FieldsAre(16, 18)))));
}

}  // namespace
}  // namespace devtools_crosstool_autofdo
