#include "llvm_propeller_binary_address_mapper.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_binary_content.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_statistics.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/container/flat_hash_map.h"
#include "third_party/abseil/absl/container/flat_hash_set.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "third_party/abseil/absl/time/time.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELFTypes.h"
#include "util/testing/status_matchers.h"

namespace devtools_crosstool_autofdo {
namespace {

using ::testing::_;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::FieldsAre;
using ::testing::IsEmpty;
using ::testing::Key;
using ::testing::Not;
using ::testing::Optional;
using ::testing::Pair;
using ::testing::ResultOf;
using ::testing::UnorderedElementsAre;

using ::llvm::object::BBAddrMap;

MATCHER_P2(BbAddrMapIs, function_address_matcher, bb_entries_matcher, "") {
  return ExplainMatchResult(function_address_matcher, arg.getFunctionAddress(),
                            result_listener) &&
         ExplainMatchResult(bb_entries_matcher, arg.getBBEntries(),
                            result_listener);
}

MATCHER_P4(BbEntryIs, id, offset, size, metadata, "") {
  return arg.ID == id && arg.Offset == offset && arg.Size == size &&
         arg.MD == metadata;
}

std::string GetAutoFdoTestDataFilePath(absl::string_view filename) {
  const std::string testdata_filepath =
      absl::StrCat(::testing::SrcDir(),
                   "/testdata/", filename);
  return testdata_filepath;
}

static absl::flat_hash_map<llvm::StringRef, BBAddrMap>
GetBBAddrMapByFunctionName(const BinaryAddressMapper &binary_address_mapper) {
  absl::flat_hash_map<llvm::StringRef, BBAddrMap> bb_addr_map_by_func_name;
  for (const auto &[function_index, symbol_info] :
       binary_address_mapper.symbol_info_map()) {
    for (llvm::StringRef alias : symbol_info.aliases)
      bb_addr_map_by_func_name.insert(
          {alias, binary_address_mapper.bb_addr_map()[function_index]});
  }
  return bb_addr_map_by_func_name;
}

TEST(LlvmBinaryAddressMapper, BbAddrMapExist) {
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryContent> binary_content,
      GetBinaryContent(GetAutoFdoTestDataFilePath("propeller_sample.bin")));
  PropellerStats stats;
  PropellerOptions options;
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryAddressMapper> binary_address_mapper,
      BuildBinaryAddressMapper(options, *binary_content, stats));
  EXPECT_THAT(binary_address_mapper->bb_addr_map(), Not(IsEmpty()));
}

TEST(LlvmBinaryAddressMapper, BbAddrMapReadSymbolTable) {
  ASSERT_OK_AND_ASSIGN(
      auto binary_content,
      GetBinaryContent(GetAutoFdoTestDataFilePath("propeller_sample.bin")));
  PropellerStats stats;
  PropellerOptions options;
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryAddressMapper> binary_address_mapper,
      BuildBinaryAddressMapper(options, *binary_content, stats));
  EXPECT_THAT(
      binary_address_mapper->symbol_info_map(),
      Contains(Pair(_, FieldsAre(ElementsAre("sample1_func"), ".text"))));
}

TEST(LlvmBinaryAddressMapper, SkipEntryIfSymbolNotInSymtab) {
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryContent> binary_content,
      GetBinaryContent(GetAutoFdoTestDataFilePath("propeller_from_icu_genrb")));
  PropellerStats stats;
  PropellerOptions options;

  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryAddressMapper> binary_address_mapper,
      BuildBinaryAddressMapper(options, *binary_content, stats));
  EXPECT_THAT(binary_address_mapper->selected_functions(), Not(IsEmpty()));
  EXPECT_GT(stats.bbaddrmap_stats.bbaddrmap_function_does_not_have_symtab_entry,
            1);
}

TEST(LlvmBinaryAddressMapper, ReadBbAddrMap) {
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryContent> binary_content,
      GetBinaryContent(GetAutoFdoTestDataFilePath("propeller_sample.bin")));
  PropellerStats stats;
  PropellerOptions options;
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryAddressMapper> binary_address_mapper,
      BuildBinaryAddressMapper(options, *binary_content, stats));
  EXPECT_THAT(binary_address_mapper->selected_functions(), Not(IsEmpty()));
  auto bb_addr_map_by_func_name =
      GetBBAddrMapByFunctionName(*binary_address_mapper);
  EXPECT_THAT(bb_addr_map_by_func_name,
              Contains(Pair("compute_flag", BbAddrMapIs(_, Not(IsEmpty())))));
  // Regenerating propeller_sample.bin may trigger a change here.
  // Use `llvm-readobj --bb-addr-map propeller_sample.bin` to capture the
  // expected data.
  EXPECT_THAT(
      bb_addr_map_by_func_name,
      UnorderedElementsAre(
          Pair("main",
               BbAddrMapIs(
                   0x1820,
                   ElementsAre(BbEntryIs(0, 0x0, 0x30,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = false,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = true}),
                               BbEntryIs(1, 0x30, 0xD,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = false,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = true}),
                               BbEntryIs(2, 0x3D, 0x24,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = false,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = true}),
                               BbEntryIs(3, 0x61, 0x2E,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = false,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = true}),
                               BbEntryIs(4, 0x8F, 0x1A,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = false,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = true}),
                               BbEntryIs(5, 0xA9, 0x34,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = false,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = true}),
                               BbEntryIs(6, 0xDD, 0x5,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = false,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = true}),
                               BbEntryIs(7, 0xE2, 0xE,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = false,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = false}),
                               BbEntryIs(8, 0xF0, 0x8,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = true,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = false})))),
          Pair("sample1_func",
               BbAddrMapIs(0x1810, ElementsAre(BbEntryIs(
                                       0, 0x0, 0x6,
                                       BBAddrMap::BBEntry::Metadata{
                                           .HasReturn = true,
                                           .HasTailCall = false,
                                           .IsEHPad = false,
                                           .CanFallThrough = false})))),
          Pair("compute_flag",
               BbAddrMapIs(
                   0x17D0,
                   ElementsAre(BbEntryIs(0, 0x0, 0x19,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = false,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = true}),
                               BbEntryIs(1, 0x19, 0x10,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = false,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = false}),
                               BbEntryIs(2, 0x29, 0x8,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = false,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = true}),
                               BbEntryIs(3, 0x31, 0x5,
                                         BBAddrMap::BBEntry::Metadata{
                                             .HasReturn = true,
                                             .HasTailCall = false,
                                             .IsEHPad = false,
                                             .CanFallThrough = false})))),
          Pair("this_is_very_code",
               BbAddrMapIs(0x1770, ElementsAre(BbEntryIs(
                                       0, 0x0, 0x5D,
                                       BBAddrMap::BBEntry::Metadata{
                                           .HasReturn = true,
                                           .HasTailCall = false,
                                           .IsEHPad = false,
                                           .CanFallThrough = false}))))));
}

TEST(LlvmBinaryAddressMapper, DuplicateSymbolsDropped) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(GetAutoFdoTestDataFilePath(
                           "propeller_duplicate_symbols.bin")));
  PropellerStats stats;
  PropellerOptions options;

  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryAddressMapper> binary_address_mapper,
      BuildBinaryAddressMapper(options, *binary_content, stats));
  EXPECT_THAT(binary_address_mapper->selected_functions(), Not(IsEmpty()));
  // Multiple symbols have the "sample1_func1" name hence none of them will be
  // kept. Other functions are not affected.
  EXPECT_THAT(
      GetBBAddrMapByFunctionName(*binary_address_mapper),
      AllOf(Not(Contains(Key("sample1_func"))),
            Contains(Pair("compute_flag", BbAddrMapIs(_, Not(IsEmpty()))))));
  EXPECT_EQ(stats.bbaddrmap_stats.duplicate_symbols, 1);
}

TEST(LlvmBinaryAddressMapper, NoneDotTextSymbolsDropped) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(GetAutoFdoTestDataFilePath(
                           "propeller_sample_section.bin")));
  PropellerStats stats;
  PropellerOptions options;

  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryAddressMapper> binary_address_mapper,
      BuildBinaryAddressMapper(options, *binary_content, stats));
  EXPECT_THAT(binary_address_mapper->selected_functions(), Not(IsEmpty()));
  // "anycall" is inside ".anycall.anysection", so it should not be processed by
  // propeller. ".text.unlikely" function symbols are processed. Other functions
  // are not affected.
  EXPECT_THAT(
      GetBBAddrMapByFunctionName(*binary_address_mapper),
      AllOf(Not(Contains(Key("anycall"))),
            Contains(Pair("unlikelycall", BbAddrMapIs(_, Not(IsEmpty())))),
            Contains(Pair("compute_flag", BbAddrMapIs(_, Not(IsEmpty()))))));
}

TEST(LlvmBinaryAddressMapper, NonDotTextSymbolsKept) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(GetAutoFdoTestDataFilePath(
                           "propeller_sample_section.bin")));
  PropellerStats stats;
  PropellerOptions options;
  options.set_filter_non_text_functions(false);

  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryAddressMapper> binary_address_mapper,
      BuildBinaryAddressMapper(options, *binary_content, stats));
  EXPECT_THAT(binary_address_mapper->selected_functions(), Not(IsEmpty()));
  // Check that all functions are processed regardless of their section name.
  EXPECT_THAT(
      GetBBAddrMapByFunctionName(*binary_address_mapper),
      AllOf(Contains(Pair("anycall", BbAddrMapIs(_, Not(IsEmpty())))),
            Contains(Pair("unlikelycall", BbAddrMapIs(_, Not(IsEmpty())))),
            Contains(Pair("compute_flag", BbAddrMapIs(_, Not(IsEmpty()))))));
}

TEST(LlvmBinaryAddressMapper, DuplicateUniqNames) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(GetAutoFdoTestDataFilePath(
                           "duplicate_unique_names.out")));
  PropellerStats stats;
  PropellerOptions options;
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryAddressMapper> binary_address_mapper,
      BuildBinaryAddressMapper(options, *binary_content, stats));

  EXPECT_THAT(binary_address_mapper->selected_functions(), Not(IsEmpty()));
  // We have 3 duplicated symbols, the last 2 are marked as duplicate_symbols.
  // 11: 0000000000001880     6 FUNC    LOCAL  DEFAULT   14
  //                     _ZL3foov.__uniq.148988607218547176184555965669372770545
  // 13: 00000000000018a0     6 FUNC    LOCAL  DEFAULT   1
  //                     _ZL3foov.__uniq.148988607218547176184555965669372770545
  // 15: 00000000000018f0     6 FUNC    LOCAL  DEFAULT   14
  //                     _ZL3foov.__uniq.148988607218547176184555965669372770545
  EXPECT_EQ(stats.bbaddrmap_stats.duplicate_symbols, 2);
}

TEST(LlvmBinaryAddressMapper, CheckNoHotFunctions) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(GetAutoFdoTestDataFilePath(
                           "propeller_sample_section.bin")));
  absl::flat_hash_set<uint64_t> hot_addresses = {
      // call from main to compute_flag.
      0x201900, 0x201870};

  PropellerStats stats;
  PropellerOptions options;
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryAddressMapper> binary_address_mapper,
      BuildBinaryAddressMapper(options, *binary_content, stats,
                               &hot_addresses));

  // main is hot and sample1_func is cold.
  EXPECT_THAT(GetBBAddrMapByFunctionName(*binary_address_mapper),
              AllOf(Contains(Pair("main", BbAddrMapIs(_, Not(IsEmpty())))),
                    Not(Contains(Key("sample1_func")))));
}

TEST(LlvmBinaryAddressMapper, FindBbHandleIndexUsingBinaryAddress) {
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BinaryContent> binary_content,
                       GetBinaryContent(GetAutoFdoTestDataFilePath(
                           "propeller_clang_v0_labels.binary")));
  PropellerStats stats;
  PropellerOptions options;
  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BinaryAddressMapper> binary_address_mapper,
      BuildBinaryAddressMapper(options, *binary_content, stats,
                               /*hot_addresses=*/nullptr));
  EXPECT_THAT(binary_address_mapper->selected_functions(), Not(IsEmpty()));
  // At address 0x000001b3d0a8, we have the following symbols all of size zero.
  //   BB.447 BB.448 BB.449 BB.450 BB.451 BB.452 BB.453 BB.454 BB.455
  //   BB.456 BB.457 BB.458 BB.459 BB.460

  auto bb_index_from_handle_index = [&](int index) {
    return binary_address_mapper->bb_handles()[index].bb_index;
  };
  EXPECT_THAT(binary_address_mapper->FindBbHandleIndexUsingBinaryAddress(
                  0x1b3d0a8, BranchDirection::kTo),
              Optional(ResultOf(bb_index_from_handle_index, 447)));
  // At address 0x000001b3f5b0: we have the following symbols:
  //   Func<_ZN5clang18CompilerInvocation14CreateFromArgs...> BB.0 {size: 0x9a}
  EXPECT_THAT(binary_address_mapper->FindBbHandleIndexUsingBinaryAddress(
                  0x1b3f5b0, BranchDirection::kTo),
              Optional(ResultOf(bb_index_from_handle_index, 0)));
  // At address 0x1e63500: we have the following symbols:
  //   Func<_ZN4llvm22FoldingSetIteratorImplC2EPPv> BB.0 {size: 0}
  //                                                BB.1 {size: 0x8}
  EXPECT_THAT(binary_address_mapper->FindBbHandleIndexUsingBinaryAddress(
                  0x1e63500, BranchDirection::kTo),
              Optional(ResultOf(bb_index_from_handle_index, 0)));
  EXPECT_THAT(binary_address_mapper->FindBbHandleIndexUsingBinaryAddress(
                  0x1e63500, BranchDirection::kFrom),
              Optional(ResultOf(bb_index_from_handle_index, 1)));
}
}  // namespace
}  // namespace devtools_crosstool_autofdo
