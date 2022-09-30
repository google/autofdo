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
  PropellerOptionsBuilder& SetSplitOnly(bool value);
  PropellerOptionsBuilder& SetLayoutOnly(bool value);
  PropellerOptionsBuilder& SetHttp(bool value);

 private:
  PropellerOptions data_;
};

}  // namespace devtools_crosstool_autofdo

#endif
