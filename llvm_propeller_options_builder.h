// Automatically generated, please sync manually.

#ifndef DEVTOOLS_CROSSTOOL_AUTOFDO_LLVM_PROPELLER_OPTIONS_BUILDER_H_
#define DEVTOOLS_CROSSTOOL_AUTOFDO_LLVM_PROPELLER_OPTIONS_BUILDER_H_

#include <string>

#include "llvm_propeller_options.pb.h"

namespace devtools_crosstool_autofdo {

class PropellerOptionsBuilder {
 public:
  PropellerOptionsBuilder() = default;
  explicit PropellerOptionsBuilder(const PropellerOptions& data)
      : data_(data) {}
  explicit PropellerOptionsBuilder(PropellerOptions&& data) : data_(data) {}

  operator const PropellerOptions&() const { return data_; }

  PropellerOptionsBuilder& SetBinaryName(const std::string & value);
  PropellerOptionsBuilder& AddPerfNames(const std::string & value);
  PropellerOptionsBuilder& SetClusterOutName(const std::string & value);
  PropellerOptionsBuilder& SetSymbolOrderOutName(const std::string & value);
  PropellerOptionsBuilder& SetProfiledBinaryName(const std::string & value);
  PropellerOptionsBuilder& SetIgnoreBuildId(bool value);
  PropellerOptionsBuilder& SetKeepFrontendIntermediateData(bool value);
  PropellerOptionsBuilder& SetCodeLayoutParams(
      const PropellerCodeLayoutParameters& value);
  PropellerOptionsBuilder& SetCodeLayoutParamsFallthroughWeight(uint32_t value);
  PropellerOptionsBuilder& SetCodeLayoutParamsForwardJumpWeight(uint32_t value);
  PropellerOptionsBuilder& SetCodeLayoutParamsBackwardJumpWeight(
      uint32_t value);
  PropellerOptionsBuilder& SetCodeLayoutParamsForwardJumpDistance(
      uint32_t value);
  PropellerOptionsBuilder& SetCodeLayoutParamsBackwardJumpDistance(
      uint32_t value);
  PropellerOptionsBuilder& SetCodeLayoutParamsChainSplit(bool value);
  PropellerOptionsBuilder& SetCodeLayoutParamsChainSplitThreshold(
      uint32_t value);
  PropellerOptionsBuilder& SetVerboseClusterOutput(bool value);
  PropellerOptionsBuilder& SetCfgDumpDirName(const std::string & value);
  PropellerOptionsBuilder& SetHttp(bool value);
  PropellerOptionsBuilder& SetCodeLayoutParamsCallChainClustering(bool value);
  PropellerOptionsBuilder& SetCodeLayoutParamsClusterMergeSizeThreshold(uint32_t value);
  PropellerOptionsBuilder& SetCodeLayoutParamsReorderHotBlocks(bool value);
  PropellerOptionsBuilder& SetCodeLayoutParamsSplitFunctions(bool value);
  PropellerOptionsBuilder& SetCodeLayoutParamsInterFunctionReordering(bool value);

 private:
  PropellerOptions data_;
};

class PropellerCodeLayoutParametersBuilder {
 public:
  static constexpr uint64_t kProtoBuilderTypeInfo = 1;

  PropellerCodeLayoutParametersBuilder() = default;
  explicit PropellerCodeLayoutParametersBuilder(
      const PropellerCodeLayoutParameters& data)
      : data_(data) {}
  explicit PropellerCodeLayoutParametersBuilder(
      PropellerCodeLayoutParameters&& data)
      : data_(data) {}

  operator const PropellerCodeLayoutParameters&() const {  // NOLINT
    return data_;
  }

  PropellerCodeLayoutParametersBuilder& SetFallthroughWeight(uint32_t value);
  PropellerCodeLayoutParametersBuilder& SetForwardJumpWeight(uint32_t value);
  PropellerCodeLayoutParametersBuilder& SetBackwardJumpWeight(uint32_t value);
  PropellerCodeLayoutParametersBuilder& SetForwardJumpDistance(uint32_t value);
  PropellerCodeLayoutParametersBuilder& SetBackwardJumpDistance(uint32_t value);
  PropellerCodeLayoutParametersBuilder& SetChainSplit(bool value);
  PropellerCodeLayoutParametersBuilder& SetChainSplitThreshold(uint32_t value);
  PropellerCodeLayoutParametersBuilder& SetCallChainClustering(bool value);
  PropellerCodeLayoutParametersBuilder& SetClusterMergeSizeThreshold(
      uint32_t value);
  PropellerCodeLayoutParametersBuilder& SetSplitFunctions(bool value);
  PropellerCodeLayoutParametersBuilder& SetReorderHotBlocks(bool value);
  PropellerCodeLayoutParametersBuilder& SetInterFunctionReordering(bool value);

 private:
  PropellerCodeLayoutParameters data_;
};

}  // namespace devtools_crosstool_autofdo

#endif
