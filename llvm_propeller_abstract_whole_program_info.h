#ifndef AUTOFDO_LLVM_PROPELLER_ABSTRACT_WHOLE_PROGRAM_INFO_H_  // NOLINT
#define AUTOFDO_LLVM_PROPELLER_ABSTRACT_WHOLE_PROGRAM_INFO_H_

#if defined(HAVE_LLVM)

#include <map>

#include "llvm/ADT/StringRef.h"
#include "llvm_propeller_cfg.h"  // NOLINT
#include "llvm_propeller_options.h"  // NOLINT
#include "llvm_propeller_statistics.h"  // NOLINT

namespace devtools_crosstool_autofdo {

// This is the data interface between the frontend (protobuf / binary+perf) and
// the code layout algorith. The main data carried here:
//   cfgs() - ControlFlowGraphs indexed by name
//   address_map() - SymbolEntries ownership and indexed by address.
//   options() - frontend options
class AbstractPropellerWholeProgramInfo {
 public:
  // Ownership of cfgs. Cfgs are indexed with primary name as key only.
  using CFGMapTy = std::map<llvm::StringRef, std::unique_ptr<ControlFlowGraph>>;
  // Ownership of SymbolEntries. Symbol ordinal -> SymbolEntry ptr.
  using OrdinalMapTy = std::map<uint64_t, std::unique_ptr<SymbolEntry>>;

  explicit AbstractPropellerWholeProgramInfo(const PropellerOptions &options)
      : options_(options) {}

  virtual ~AbstractPropellerWholeProgramInfo() {}

  virtual bool CreateCfgs() = 0;

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
#endif  // AUTOFDO_LLVM_PROPELLER_ABSTRACT_WHOLE_PROGRAM_INFO_H_  // NOLINT
