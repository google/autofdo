
// hierarchical_discriminator_test
//
// Targeted checks on the checked-in binary + perf.data:
//
// 1) test_unroll: a pos_counts sample whose packed discriminator has non-zero
//    copy_id (HasCopyID). Full-profile keys use the raw word; two-pass pass 1
//    uses StripMultiplicity (before CollapseCopyIDs). Assert the merged bucket
//    exists on test_unroll and PERFDATA MAX count matches manual MAX over full
//    keys stripping to that bucket.
//
// 2) test_vector_loop: a pos_counts sample with hierarchical multiplicity
//    (HasMultiplicity). Same two-pass strip + MAX count check on test_vector_loop.
//
// 3) Pass 2: SymbolMap::CollapseCopyIDs() (as at gcov write) strips copy_id to
//    base-only keys and SUMs ProfileInfo. For the same test_unroll copy_id
//    sample line+base, assert the collapsed bucket count equals the SUM of
//    pass-1 counts for all buckets sharing that (line, base).
#include "profile_creator.h"
#include "gcov_discriminator_encoding.h"
#include "source_info.h"
#include "symbol_map.h"

#include <algorithm>
#include <functional>
#include <string>

#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/match.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"

ABSL_DECLARE_FLAG(bool, use_lbr);
ABSL_DECLARE_FLAG(bool, use_discriminator_encoding);
ABSL_DECLARE_FLAG(bool, use_two_pass_aggregation);

namespace {

using ::devtools_crosstool_autofdo::GetBaseDiscriminator;
using ::devtools_crosstool_autofdo::GetCopyID;
using ::devtools_crosstool_autofdo::GetMultiplicity;
using ::devtools_crosstool_autofdo::HasCopyID;
using ::devtools_crosstool_autofdo::HasMultiplicity;
using ::devtools_crosstool_autofdo::ProfileCreator;
using ::devtools_crosstool_autofdo::SourceInfo;
using ::devtools_crosstool_autofdo::StripMultiplicity;
using ::devtools_crosstool_autofdo::PositionCountMap;
using ::devtools_crosstool_autofdo::Symbol;
using ::devtools_crosstool_autofdo::SymbolMap;

const char kTestDataDir[] = "/testdata/";

constexpr char kFixtureElf[] = "hierarchical_discriminator_test.x86_64";
constexpr char kFixturePerf[] =
    "hierarchical_discriminator_test.x86_64.perf.data";

std::string TestBinaryPath() {
  return absl::StrCat(::testing::SrcDir(), kTestDataDir, kFixtureElf);
}

std::string TestPerfDataPath() {
  return absl::StrCat(::testing::SrcDir(), kTestDataDir, kFixturePerf);
}

const Symbol *FindSymbolByStem(const SymbolMap &symbol_map,
                               absl::string_view name_stem) {
  const auto &name_to_symbol = symbol_map.map();
  const std::string exact(name_stem);
  auto direct = name_to_symbol.find(exact);
  if (direct != name_to_symbol.end()) return direct->second;
  for (const auto &entry : name_to_symbol) {
    if (absl::StrContains(entry.first, name_stem)) return entry.second;
  }
  return nullptr;
}

struct PosCountSample {
  uint32_t raw_packed_discriminator = 0;
  uint64_t pos_count_offset_key = 0;
};

uint64_t SampleCountAtBucket(const Symbol *symbol, uint32_t line_relative,
                             uint32_t packed_discriminator) {
  if (symbol == nullptr) return 0;
  const uint64_t key =
      SourceInfo::GenerateOffset(line_relative, packed_discriminator);
  auto it = symbol->pos_counts.find(key);
  if (it == symbol->pos_counts.end()) return 0;
  return it->second.count;
}

uint64_t MaxFullProfileCountMergingToStripBucket(const Symbol *full_symbol,
                                                 uint32_t line_relative,
                                                 uint32_t stripped_discriminator) {
  if (full_symbol == nullptr) return 0;
  uint64_t max_count = 0;
  for (const auto &row : full_symbol->pos_counts) {
    if (SourceInfo::GetLineNumberFromOffset(row.first) != line_relative)
      continue;
    const uint32_t full_packed =
        SourceInfo::GetDiscriminatorFromOffset(row.first);
    if (StripMultiplicity(full_packed) != stripped_discriminator) continue;
    max_count = std::max(max_count, row.second.count);
  }
  return max_count;
}

bool TwoPassHasBucket(const Symbol *two_pass_symbol, uint32_t line_relative,
                      uint32_t stripped_discriminator) {
  if (two_pass_symbol == nullptr) return false;
  for (const auto &row : two_pass_symbol->pos_counts) {
    if (SourceInfo::GetLineNumberFromOffset(row.first) == line_relative &&
        SourceInfo::GetDiscriminatorFromOffset(row.first) ==
            stripped_discriminator) {
      return true;
    }
  }
  return false;
}

// Pass 1 two-pass: bucket (line, StripMultiplicity(raw)) exists and .count
// equals MAX of full-profile .count over full keys on that line stripping there.
void VerifyPass1TwoPassBucketMaxMatchesFullProfile(
    const Symbol *full_symbol, const Symbol *two_pass_symbol,
    const PosCountSample &full_sample, absl::string_view label) {
  const uint32_t line_relative =
      SourceInfo::GetLineNumberFromOffset(full_sample.pos_count_offset_key);
  const uint32_t stripped =
      StripMultiplicity(full_sample.raw_packed_discriminator);
  ASSERT_TRUE(TwoPassHasBucket(two_pass_symbol, line_relative, stripped))
      << label << ": missing two-pass bucket (line_rel=" << line_relative
      << ", StripMultiplicity(raw)=" << stripped << ")";
  const uint64_t expected_max = MaxFullProfileCountMergingToStripBucket(
      full_symbol, line_relative, stripped);
  const uint64_t actual =
      SampleCountAtBucket(two_pass_symbol, line_relative, stripped);
  ASSERT_EQ(actual, expected_max)
      << label << ": two-pass count must equal MAX of full-profile counts for "
                  "full keys on this line stripping to this bucket";
}

// Pass 1 pos_counts: SUM counts for rows on line_relative whose packed
// discriminator has the same 8-bit base (these rows merge in CollapseCopyIDs).
uint64_t SumPass1CountsMergingToBaseOnLine(
    const PositionCountMap &pass1_pos_counts, uint32_t line_relative,
    uint32_t base_discriminator) {
  uint64_t sum = 0;
  for (const auto &row : pass1_pos_counts) {
    if (SourceInfo::GetLineNumberFromOffset(row.first) != line_relative) continue;
    const uint32_t packed =
        SourceInfo::GetDiscriminatorFromOffset(row.first);
    if (GetBaseDiscriminator(packed) != base_discriminator) continue;
    sum += row.second.count;
  }
  return sum;
}

// Snapshot pass-1 test_unroll pos_counts, run SymbolMap::CollapseCopyIDs(), then
// compare the collapsed (line, base-only) bucket to the manual SUM above.
// After CollapseCopyIDs: base-only bucket count equals SUM of pass-1 counts
// merged into that (line, base) on test_unroll for the captured copy_id row.
void VerifyCollapseCopyIdsSumsPass1IntoBaseOnlyUnrollBucket(
    SymbolMap *two_pass_map, const Symbol *unroll_two_pass_symbol,
    const PositionCountMap &unroll_pass1_pos_counts_snapshot,
    const PosCountSample &unroll_copy_id_sample) {
  const uint32_t line_relative = SourceInfo::GetLineNumberFromOffset(
      unroll_copy_id_sample.pos_count_offset_key);
  const uint32_t base_only = GetBaseDiscriminator(
      unroll_copy_id_sample.raw_packed_discriminator);
  const uint64_t expected_sum_after_collapse =
      SumPass1CountsMergingToBaseOnLine(unroll_pass1_pos_counts_snapshot,
                                        line_relative, base_only);

  two_pass_map->CollapseCopyIDs();

  ASSERT_TRUE(TwoPassHasBucket(unroll_two_pass_symbol, line_relative, base_only))
      << "CollapseCopyIDs: expected base-only bucket on test_unroll";
  const uint64_t actual_count = SampleCountAtBucket(
      unroll_two_pass_symbol, line_relative, base_only);
  ASSERT_EQ(actual_count, expected_sum_after_collapse)
      << "CollapseCopyIDs: pos_counts.count at (line_rel=" << line_relative
      << ", base=" << base_only
      << ") should equal SUM of pass-1 counts for that line and base";

  EXPECT_EQ(GetCopyID(base_only), 0u)
      << "after CollapseCopyIDs discriminator field should be base-only (no copy_id)";
}

// First symbol keyed like `stem` (any map entry whose name contains `stem`)
// with a pos_counts row satisfying `predicate`; pairs full + two-pass by exact
// profile name.
bool FindFirstMatchingSymbolPair(
    const SymbolMap &full_map, const SymbolMap &two_pass_map,
    absl::string_view name_substring,
    const std::function<bool(uint32_t packed)> &predicate,
    const Symbol **full_symbol_out, const Symbol **two_pass_symbol_out,
    std::string *matched_name_out) {
  for (const auto &name_and_symbol : full_map.map()) {
    if (!absl::StrContains(name_and_symbol.first, name_substring)) continue;
    const Symbol *full_sym = name_and_symbol.second;
    if (full_sym == nullptr) continue;
    auto two_pass_it = two_pass_map.map().find(name_and_symbol.first);
    if (two_pass_it == two_pass_map.map().end() ||
        two_pass_it->second == nullptr) {
      continue;
    }
    for (const auto &row : full_sym->pos_counts) {
      const uint32_t packed =
          SourceInfo::GetDiscriminatorFromOffset(row.first);
      if (!predicate(packed)) continue;
      *full_symbol_out = full_sym;
      *two_pass_symbol_out = two_pass_it->second;
      *matched_name_out = name_and_symbol.first;
      return true;
    }
  }
  return false;
}

// test_unroll*: first pos_counts hit with copy_id — pass-1 strip bucket .count
// vs full-profile MAX for that strip bucket.
void VerifyUnrollFirstCopyIdHitPass1StripAgainstFullProfile(
    const SymbolMap &full_map, const SymbolMap &two_pass_map,
    PosCountSample *copy_id_sample_out, const Symbol **two_pass_symbol_out) {
  const Symbol *full_sym = nullptr;
  const Symbol *tp_sym = nullptr;
  std::string matched_name;
  ASSERT_TRUE(FindFirstMatchingSymbolPair(
      full_map, two_pass_map, "test_unroll",
      [](uint32_t packed) { return HasCopyID(packed); }, &full_sym, &tp_sym,
      &matched_name))
      << "fixture must provide a pos_counts row on a symbol keyed like "
         "test_unroll* with HasCopyID (try rebuilding testdata if compiler "
         "moved discriminators to a clone symbol)";
  *two_pass_symbol_out = tp_sym;
  for (const auto &row : full_sym->pos_counts) {
    const uint32_t packed =
        SourceInfo::GetDiscriminatorFromOffset(row.first);
    if (!HasCopyID(packed)) continue;
    *copy_id_sample_out = {packed, row.first};
    ASSERT_GT(GetCopyID(packed), 0u);
    VerifyPass1TwoPassBucketMaxMatchesFullProfile(
        full_sym, tp_sym, *copy_id_sample_out,
        absl::StrCat("copy_id ", matched_name));
    return;
  }
  FAIL() << "internal error: predicate matched but scan did not";
}

// test_vector_loop*: first pos_counts hit with multiplicity — pass-1 strip
// bucket .count vs full-profile MAX.
void VerifyVectorLoopFirstMultiplicityHitPass1StripAgainstFullProfile(
    const SymbolMap &full_map, const SymbolMap &two_pass_map) {
  const Symbol *full_sym = nullptr;
  const Symbol *tp_sym = nullptr;
  std::string matched_name;
  ASSERT_TRUE(FindFirstMatchingSymbolPair(
      full_map, two_pass_map, "test_vector_loop",
      [](uint32_t packed) { return HasMultiplicity(packed); }, &full_sym,
      &tp_sym, &matched_name))
      << "fixture must provide a pos_counts row on a symbol keyed like "
         "test_vector_loop* with HasMultiplicity";

  for (const auto &row : full_sym->pos_counts) {
    const uint32_t packed =
        SourceInfo::GetDiscriminatorFromOffset(row.first);
    if (!HasMultiplicity(packed)) continue;
    const PosCountSample sample{packed, row.first};
    ASSERT_GT(GetMultiplicity(packed), 1u);
    VerifyPass1TwoPassBucketMaxMatchesFullProfile(
        full_sym, tp_sym, sample,
        absl::StrCat("multiplicity ", matched_name));
    return;
  }
  FAIL() << "internal error: predicate matched but scan did not";
}

TEST(GccHierarchicalDiscriminatorMultiplicityTest,
     UnrollCopyIdAndVectorMultiplicityTwoPass) {
  ProfileCreator profile_creator(TestBinaryPath());
  ASSERT_TRUE(profile_creator.ReadSample(TestPerfDataPath(), "perf"));

  absl::SetFlag(&FLAGS_use_lbr, false);
  absl::SetFlag(&FLAGS_use_discriminator_encoding, false);
  absl::SetFlag(&FLAGS_use_two_pass_aggregation, false);

  SymbolMap full_map(TestBinaryPath());
  full_map.ReadLoadableExecSegmentInfo(/*is_kernel=*/false);
  ASSERT_TRUE(profile_creator.ComputeProfile(&full_map,
                                               /*check_lbr_entry=*/false));

  ASSERT_NE(FindSymbolByStem(full_map, "test_unroll"), nullptr)
      << "expected test_unroll (or clone) in profile map";
  ASSERT_NE(FindSymbolByStem(full_map, "test_vector_loop"), nullptr)
      << "expected test_vector_loop (or clone) in profile map";

  // Two-pass pass 1 (ComputeProfile only).
  absl::SetFlag(&FLAGS_use_discriminator_encoding, true);
  absl::SetFlag(&FLAGS_use_two_pass_aggregation, true);

  SymbolMap two_pass_map(TestBinaryPath());
  two_pass_map.ReadLoadableExecSegmentInfo(/*is_kernel=*/false);
  ASSERT_TRUE(profile_creator.ComputeProfile(&two_pass_map,
                                             /*check_lbr_entry=*/false));

  ASSERT_NE(FindSymbolByStem(two_pass_map, "test_unroll"), nullptr);
  ASSERT_NE(FindSymbolByStem(two_pass_map, "test_vector_loop"), nullptr);

  PosCountSample unroll_copy_id_sample{};
  const Symbol *unroll_two_pass_for_copy_id = nullptr;
  VerifyUnrollFirstCopyIdHitPass1StripAgainstFullProfile(
      full_map, two_pass_map, &unroll_copy_id_sample, &unroll_two_pass_for_copy_id);
  VerifyVectorLoopFirstMultiplicityHitPass1StripAgainstFullProfile(full_map,
                                                                   two_pass_map);

  // Pass 2 (whole map): compare CollapseCopyIDs output for the same profile
  // symbol that held the copy_id row — base-only bucket vs SUM of pass-1 rows.
  const PositionCountMap pass1_pos_counts_before_collapse =
      unroll_two_pass_for_copy_id->pos_counts;
  VerifyCollapseCopyIdsSumsPass1IntoBaseOnlyUnrollBucket(
      &two_pass_map, unroll_two_pass_for_copy_id, pass1_pos_counts_before_collapse,
      unroll_copy_id_sample);
}

}  // namespace
