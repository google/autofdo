#include "PropellerConfig.h"
#include "gflags/gflags.h"

DEFINE_int64(propeller_backward_jump_distance, 1024, "Propeller backward jump distance");
DEFINE_int64(propeller_forward_jump_distance, 1024, "Propeller forward jump distance");
DEFINE_bool(propeller_reorder_blocks, true, "Propeller reorder blocks");
DEFINE_bool(propeller_reorder_funcs, true, "Propeller reorder functions");
DEFINE_bool(propeller_split_funcs, true, "Propeller split functions");
DEFINE_bool(propeller_reorder_ip, true, "Propeller reorder inter-procedural");
DEFINE_bool(propeller_print_stats, true, "Propeller print statistics");

namespace llvm {
namespace propeller {

PropellerConfig::PropellerConfig() {
  this->optBackwardJumpDistance = FLAGS_propeller_backward_jump_distance;
  this->optForwardJumpDistance = FLAGS_propeller_forward_jump_distance;
  // Scale weights for use in the computation of ExtTSP score.
  this->optFallthroughWeight *=
      this->optForwardJumpDistance * this->optBackwardJumpDistance;
  this->optBackwardJumpWeight *= this->optForwardJumpDistance;
  this->optForwardJumpWeight *= this->optBackwardJumpDistance;
  this->optReorderBlocks = FLAGS_propeller_reorder_blocks;
  this->optReorderFuncs = FLAGS_propeller_reorder_funcs;
  this->optSplitFuncs = FLAGS_propeller_split_funcs;
  this->optReorderIP = FLAGS_propeller_reorder_ip;
  this->optPrintStats = FLAGS_propeller_print_stats;
}

} // namespace propeller
} // namespace llvm


