#ifndef AUTOFDO_LLVM_PROPELLER_ABSTRACT_WHOLE_PROGRAM_INFO_H_
#define AUTOFDO_LLVM_PROPELLER_ABSTRACT_WHOLE_PROGRAM_INFO_H_

#if defined(HAVE_LLVM)

#include <map>
#include <memory>
#include <vector>

#include "llvm_propeller_cfg.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_statistics.h"
#include "third_party/abseil/absl/status/status.h"
#include "llvm/ADT/StringRef.h"

namespace devtools_crosstool_autofdo {

// Struct for specifying whether CFGs must be created for all or only for hot
// functions.
enum class CfgCreationMode {
  kAllFunctions,
  kOnlyHotFunctions,
};

// This is the data interface between the frontend (protobuf / binary+perf) and
// the code layout algorith. The main data carried here:
//   cfgs() - ControlFlowGraphs indexed by name
//   options() - frontend options
class AbstractPropellerWholeProgramInfo {
 public:
  // Ownership of cfgs. Cfgs are indexed with primary name as key only.
  using CFGMapTy = std::map<llvm::StringRef, std::unique_ptr<ControlFlowGraph>>;

  explicit AbstractPropellerWholeProgramInfo(const PropellerOptions &options)
      : options_(options) {}

  virtual ~AbstractPropellerWholeProgramInfo() {}

  // CreateCfgs now can calculate a list of hot functions from profiles and only
  // create CFGs for hot functions, this greatly reduces memory
  // consumption. `cfg_creation_mode=kAllFunctions` will disable this
  // optimization and create CFGs for all functions, which is useful in testing.
  virtual absl::Status CreateCfgs(CfgCreationMode cfg_creation_mode) = 0;

  const CFGMapTy &cfgs() const { return cfgs_; }
  const PropellerOptions &options() const { return options_; }
  const PropellerStats &stats() const { return stats_; }

  // Find control flow graph by name.
  const ControlFlowGraph *FindCfg(llvm::StringRef name) const {
    auto i = cfgs_.find(name);
    return i == cfgs_.end() ? nullptr : i->second.get();
  }

  std::vector<ControlFlowGraph*> GetHotCfgs() const {
    std::vector<ControlFlowGraph*> hot_cfgs;
    for (const auto &cfg_elem : cfgs_) {
      if (cfg_elem.second->IsHot())
        hot_cfgs.push_back(cfg_elem.second.get());
    }
    return hot_cfgs;
  }

 protected:
  // See CFGMapTy.
  CFGMapTy cfgs_;
  const PropellerOptions options_;
  PropellerStats stats_;
};

}  // namespace devtools_crosstool_autofdo

#endif
#endif  // AUTOFDO_LLVM_PROPELLER_ABSTRACT_WHOLE_PROGRAM_INFO_H_
