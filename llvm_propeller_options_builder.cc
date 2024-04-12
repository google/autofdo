// Automatically generated, please sync manually.

#include "llvm_propeller_options_builder.h"

#include <string>

namespace devtools_crosstool_autofdo {

InputProfileBuilder& InputProfileBuilder::SetName(absl::string_view value) {
  data_.set_name(std::string(value));
  return *this;
}

InputProfileBuilder& InputProfileBuilder::SetType(ProfileType value) {
  data_.set_type(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetBinaryName(absl::string_view value) {
  data_.set_binary_name(std::string(value));
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::AddProfileNames(absl::string_view value) {
  data_.add_profile_names(std::string(value));
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetClusterOutName(absl::string_view value) {
  data_.set_cluster_out_name(std::string(value));
  return *this;
}
PropellerOptionsBuilder& PropellerOptionsBuilder::SetSymbolOrderOutName(absl::string_view value) {
  data_.set_symbol_order_out_name(std::string(value));
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetProfiledBinaryName(absl::string_view value) {
  data_.set_profiled_binary_name(std::string(value));
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetIgnoreBuildId(bool value) {
  data_.set_ignore_build_id(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParams(const PropellerCodeLayoutParameters& value) {
  *data_.mutable_code_layout_params() = value;
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsFallthroughWeight(uint32_t value) {
  data_.mutable_code_layout_params()->set_fallthrough_weight(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsForwardJumpWeight(uint32_t value) {
  data_.mutable_code_layout_params()->set_forward_jump_weight(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsBackwardJumpWeight(uint32_t value) {
  data_.mutable_code_layout_params()->set_backward_jump_weight(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsForwardJumpDistance(uint32_t value) {
  data_.mutable_code_layout_params()->set_forward_jump_distance(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsBackwardJumpDistance(uint32_t value) {
  data_.mutable_code_layout_params()->set_backward_jump_distance(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsChainSplit(bool value) {
  data_.mutable_code_layout_params()->set_chain_split(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsChainSplitThreshold(uint32_t value) {
  data_.mutable_code_layout_params()->set_chain_split_threshold(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsCallChainClustering(bool value) {
  data_.mutable_code_layout_params()->set_call_chain_clustering(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsClusterMergeSizeThreshold(uint32_t value) {
  data_.mutable_code_layout_params()->set_cluster_merge_size_threshold(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsSplitFunctions(bool value) {
  data_.mutable_code_layout_params()->set_split_functions(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsReorderHotBlocks(bool value) {
  data_.mutable_code_layout_params()->set_reorder_hot_blocks(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCodeLayoutParamsInterFunctionReordering(bool value) {
  data_.mutable_code_layout_params()->set_inter_function_reordering(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetVerboseClusterOutput(bool value) {
  data_.set_verbose_cluster_output(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetCfgDumpDirName(absl::string_view value) {
  data_.set_cfg_dump_dir_name(std::string(value));
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetHttp(bool value) {
  data_.set_http(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetOutputModuleName(bool value) {
  data_.set_output_module_name(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetFilterNonTextFunctions(bool value) {
  data_.set_filter_non_text_functions(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetClusterOutVersion(ClusterEncodingVersion value) {
  data_.set_cluster_out_version(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::AddInputProfiles(const InputProfile& value) {
  *data_.add_input_profiles() = value;
  return *this;
}

PropellerCodeLayoutParametersBuilder& PropellerCodeLayoutParametersBuilder::SetFallthroughWeight(uint32_t value) {
  data_.set_fallthrough_weight(value);
  return *this;
}

PropellerCodeLayoutParametersBuilder& PropellerCodeLayoutParametersBuilder::SetForwardJumpWeight(uint32_t value) {
  data_.set_forward_jump_weight(value);
  return *this;
}

PropellerCodeLayoutParametersBuilder& PropellerCodeLayoutParametersBuilder::SetBackwardJumpWeight(uint32_t value) {
  data_.set_backward_jump_weight(value);
  return *this;
}

PropellerCodeLayoutParametersBuilder& PropellerCodeLayoutParametersBuilder::SetForwardJumpDistance(uint32_t value) {
  data_.set_forward_jump_distance(value);
  return *this;
}

PropellerCodeLayoutParametersBuilder& PropellerCodeLayoutParametersBuilder::SetBackwardJumpDistance(uint32_t value) {
  data_.set_backward_jump_distance(value);
  return *this;
}

PropellerCodeLayoutParametersBuilder& PropellerCodeLayoutParametersBuilder::SetChainSplit(bool value) {
  data_.set_chain_split(value);
  return *this;
}

PropellerCodeLayoutParametersBuilder& PropellerCodeLayoutParametersBuilder::SetChainSplitThreshold(uint32_t value) {
  data_.set_chain_split_threshold(value);
  return *this;
}

PropellerCodeLayoutParametersBuilder& PropellerCodeLayoutParametersBuilder::SetCallChainClustering(bool value) {
  data_.set_call_chain_clustering(value);
  return *this;
}

PropellerCodeLayoutParametersBuilder& PropellerCodeLayoutParametersBuilder::SetClusterMergeSizeThreshold(uint32_t value) {
  data_.set_cluster_merge_size_threshold(value);
  return *this;
}

PropellerCodeLayoutParametersBuilder& PropellerCodeLayoutParametersBuilder::SetSplitFunctions(bool value) {
  data_.set_split_functions(value);
  return *this;
}

PropellerCodeLayoutParametersBuilder& PropellerCodeLayoutParametersBuilder::SetReorderHotBlocks(bool value) {
  data_.set_reorder_hot_blocks(value);
  return *this;
}

PropellerCodeLayoutParametersBuilder& PropellerCodeLayoutParametersBuilder::SetInterFunctionReordering(bool value) {
  data_.set_inter_function_reordering(value);
  return *this;
}

}  // namespace devtools_crosstool_autofdo