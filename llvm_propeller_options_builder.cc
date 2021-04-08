// Automatically generated, please sync manually.

#include "llvm_propeller_options_builder.h"

namespace devtools_crosstool_autofdo {

PropellerOptionsBuilder& PropellerOptionsBuilder::SetBinaryName(
    const std::string& value) {
  data_.set_binary_name(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::AddPerfNames(
    const std::string& value) {
  data_.add_perf_names(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetClusterOutName(
    const std::string& value) {
  data_.set_cluster_out_name(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetSymbolOrderOutName(
    const std::string& value) {
  data_.set_symbol_order_out_name(value);
  return *this;
}

PropellerOptionsBuilder& PropellerOptionsBuilder::SetProfiledBinaryName(
    const std::string& value) {
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

}  // namespace devtools_crosstool_autofdo
