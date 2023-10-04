// Automatically generated, synced manually

#ifndef AUTOFDO_LLVM_PROPELLER_OPTIONS_BUILDER_H_
#define AUTOFDO_LLVM_PROPELLER_OPTIONS_BUILDER_H_

#include <string>
#include "llvm_propeller_options.pb.h"
#include "third_party/abseil/absl/base/attributes.h"
#include "third_party/abseil/absl/strings/string_view.h"

namespace devtools_crosstool_autofdo {

class PropellerOptionsBuilder {
 public:
  static constexpr uint64_t kProtoBuilderTypeInfo = 1;

  PropellerOptionsBuilder() = default;
  explicit PropellerOptionsBuilder(const PropellerOptions& data) : data_(data) {}
  explicit PropellerOptionsBuilder(PropellerOptions&& data) : data_(data) {}

  operator const PropellerOptions&() const {
    return data_;
  }

  PropellerOptionsBuilder& SetBinaryName(const std::string& value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& AddPerfNames(const std::string& value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetClusterOutName(const std::string& value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetSymbolOrderOutName(const std::string& value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetProfiledBinaryName(const std::string& value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetIgnoreBuildId(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParams(const PropellerCodeLayoutParameters& value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParamsFallthroughWeight(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParamsForwardJumpWeight(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParamsBackwardJumpWeight(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParamsForwardJumpDistance(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParamsBackwardJumpDistance(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParamsChainSplit(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParamsChainSplitThreshold(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParamsCallChainClustering(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParamsClusterMergeSizeThreshold(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParamsSplitFunctions(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParamsReorderHotBlocks(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCodeLayoutParamsInterFunctionReordering(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetVerboseClusterOutput(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetCfgDumpDirName(const std::string& value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetHttp(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetOutputModuleName(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerOptionsBuilder& SetFilterNonTextFunctions(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;

 private:
  PropellerOptions data_;
};

class PropellerCodeLayoutParametersBuilder {
 public:
  static constexpr uint64_t kProtoBuilderTypeInfo = 1;

  PropellerCodeLayoutParametersBuilder() = default;
  explicit PropellerCodeLayoutParametersBuilder(const PropellerCodeLayoutParameters& data) : data_(data) {}
  explicit PropellerCodeLayoutParametersBuilder(PropellerCodeLayoutParameters&& data) : data_(data) {}

  operator const PropellerCodeLayoutParameters&() const {
    return data_;
  }

  PropellerCodeLayoutParametersBuilder& SetFallthroughWeight(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerCodeLayoutParametersBuilder& SetForwardJumpWeight(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerCodeLayoutParametersBuilder& SetBackwardJumpWeight(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerCodeLayoutParametersBuilder& SetForwardJumpDistance(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerCodeLayoutParametersBuilder& SetBackwardJumpDistance(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerCodeLayoutParametersBuilder& SetChainSplit(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerCodeLayoutParametersBuilder& SetChainSplitThreshold(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerCodeLayoutParametersBuilder& SetCallChainClustering(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerCodeLayoutParametersBuilder& SetClusterMergeSizeThreshold(uint32_t value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerCodeLayoutParametersBuilder& SetSplitFunctions(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerCodeLayoutParametersBuilder& SetReorderHotBlocks(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;
  PropellerCodeLayoutParametersBuilder& SetInterFunctionReordering(bool value) ABSL_ATTRIBUTE_LIFETIME_BOUND;

 private:
  PropellerCodeLayoutParameters data_;
};

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_LLVM_PROPELLER_OPTIONS_BUILDER_H_
