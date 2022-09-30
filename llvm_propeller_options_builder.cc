// Automatically generated, please sync manually.

#include "llvm_propeller_options_builder.h"

namespace devtools_crosstool_autofdo {

PropellerOptionsBuilder& PropellerOptionsBuilder::SetBinaryName(
    const std::string & value) {
  data_.set_binary_name(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::AddPerfNames(
    const std::string & value) {
  data_.add_perf_names(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetClusterOutName(
    const std::string & value) {
  data_.set_cluster_out_name(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetSymbolOrderOutName(
    const std::string & value) {
  data_.set_symbol_order_out_name(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetProfiledBinaryName(
    const std::string & value) {
  data_.set_profiled_binary_name(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetIgnoreBuildId(bool value) {
  data_.set_ignore_build_id(value);
  return *this;
}

PropellerOptionsBuilder&
PropellerOptionsBuilder::SetKeepFrontendIntermediateData(bool value) {
  data_.set_keep_frontend_intermediate_data(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParams(
    const PropellerCodeLayoutParameters& value) {
  *data_.mutable_code_layout_params() = value;
  return *this;
}

PropellerOptionsBuilder&
PropellerOptionsBuilder::SetCodeLayoutParamsFallthroughWeight(uint32_t value) {
  data_.mutable_code_layout_params()->set_fallthrough_weight(value);
  return *this;
}

PropellerOptionsBuilder&
PropellerOptionsBuilder::SetCodeLayoutParamsForwardJumpWeight(uint32_t value) {
  data_.mutable_code_layout_params()->set_forward_jump_weight(value);
  return *this;
}

PropellerOptionsBuilder&
PropellerOptionsBuilder::SetCodeLayoutParamsBackwardJumpWeight(uint32_t value) {
  data_.mutable_code_layout_params()->set_backward_jump_weight(value);
  return *this;
}

PropellerOptionsBuilder&
PropellerOptionsBuilder::SetCodeLayoutParamsForwardJumpDistance(
    uint32_t value) {
  data_.mutable_code_layout_params()->set_forward_jump_distance(value);
  return *this;
}

PropellerOptionsBuilder&
PropellerOptionsBuilder::SetCodeLayoutParamsBackwardJumpDistance(
    uint32_t value) {
  data_.mutable_code_layout_params()->set_backward_jump_distance(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsChainSplit(
    bool value) {
  data_.mutable_code_layout_params()->set_chain_split(value);
  return *this;
}

PropellerOptionsBuilder&
PropellerOptionsBuilder::SetCodeLayoutParamsChainSplitThreshold(
    uint32_t value) {
  data_.mutable_code_layout_params()->set_chain_split_threshold(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetVerboseClusterOutput(
    bool value) {
  data_.set_verbose_cluster_output(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCfgDumpDirName(
    const std::string & value) {
  data_.set_cfg_dump_dir_name(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetSplitOnly(bool value) {
  data_.set_split_only(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetLayoutOnly(bool value) {
  data_.set_layout_only(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetHttp(bool value) {
  data_.set_http(value);
  return *this;
}

}  // namespace devtools_crosstool_autofdo
