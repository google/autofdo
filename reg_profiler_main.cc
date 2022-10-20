/**
 * This tool is used for profiling register usage using pref sampling data.
 * Usage:
 *   reg_profiler --binary=/path/to/binary --profile=/path/to/perf.data
 * This tool will print out the number of push and pop actions in the sampling
 *data.
 **/

#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_whole_program_info.h"
#include "reg_profiler.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_split.h"

using devtools_crosstool_autofdo::PropellerOptions;
using devtools_crosstool_autofdo::PropellerOptionsBuilder;
using devtools_crosstool_autofdo::PropellerWholeProgramInfo;
using devtools_crosstool_autofdo::PushPopCounter;

ABSL_FLAG(std::string, binary, "", "Path to the binary");
ABSL_FLAG(std::string, profile, "", "Path to the profile data ");

ABSL_FLAG(std::string, namelist, "",
          "Show registers usage of functions in this list");
ABSL_FLAG(bool, not_in_list, false,
          "Show registers usage of functions not in the name list");

namespace {

PropellerOptions getOptions() {
  PropellerOptionsBuilder option_builder;
  std::string pstr = absl::GetFlag(FLAGS_profile);
  if (!pstr.empty() && pstr[0] == '@') {
    std::ifstream fin(pstr.substr(1));
    std::string pf;
    while (std::getline(fin, pf)) {
      if (!pf.empty() && pf[0] != '#') {
        option_builder.AddPerfNames(pf);
      }
    }
  } else {
    std::vector<std::string> perf_files = absl::StrSplit(pstr, ';');
    for (const std::string &pf : perf_files)
      if (!pf.empty()) option_builder.AddPerfNames(pf);
  }
  return PropellerOptions(
      option_builder.SetBinaryName(absl::GetFlag(FLAGS_binary)));
}

}  // namespace

int main(int argc, char *argv[]) {
  auto options = getOptions();
  auto whole_program_info = PropellerWholeProgramInfo::Create(options);
  auto s = whole_program_info->CreateCfgs();
  if (!s) {
    LOG(ERROR) << s;
    return 1;
  }
  PushPopCounter counter(*whole_program_info);

  // add function filter to only count partial results
  if (!absl::GetFlag(FLAGS_namelist).empty()) {
    counter.setFilterFunctions(absl::GetFlag(FLAGS_namelist));
    bool not_in_list = absl::GetFlag(FLAGS_not_in_list);
    counter.setNotInList(not_in_list);
    std::cout << "Count register usage " << (not_in_list? "not" : "")
              << " in the name list: " << std::endl;
    std::cout << "  " << absl::GetFlag(FLAGS_namelist)  << std::endl;
  }

  counter.Count();
  std::string N[5] = {"all", "in_unlikely", "in_hot", "in_split", "in_startup"};
  auto *data = counter.getData();
  for (int i = 0; i < 5; ++i) {
    std::cout << N[i] << " push: " << data[i].push << std::endl;
    std::cout << N[i] << " pop:  " << data[i].pop << std::endl;
  }

  return 0;
}
