/**
 * This tool is used for profiling register usage using pref sampling data.
 * Usage:
 *   create_reg_prof --binary=/path/to/binary --profile=/path/to/perf.data
 * This tool will generate a file which shows the data is .
 **/

#include <cstdint>
#include <fstream>
#include <functional>
#include <ios>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>


#include "base/logging.h"
#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_whole_program_info.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/str_split.h"

#if defined(HAVE_LLVM)
using devtools_crosstool_autofdo::PropellerOptions;
using devtools_crosstool_autofdo::PropellerOptionsBuilder;
using devtools_crosstool_autofdo::PropellerWholeProgramInfo;

ABSL_FLAG(std::string, binary, "", "Path to the binary");
ABSL_FLAG(std::string, profile, "", "Path to the profile data ");
ABSL_FLAG(std::string, output, "", "Path to the output");
ABSL_FLAG(std::string, detail, "", "Path to the detailed output file");
ABSL_FLAG(uint32_t, hot_threshold, 5,
            "prof samples that indicate the function is hot");
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
    for (const std::string& pf : perf_files)
      if (!pf.empty()) option_builder.AddPerfNames(pf);
  }
  return PropellerOptions(
      option_builder.SetBinaryName(absl::GetFlag(FLAGS_binary)));
}

}  // namespace

struct Record {
  std::string name;
  uint64_t freq;
  bool operator<(const Record& other) const {
    if (freq < other.freq) return true;
    if (freq == other.freq) return name < other.name;
    return false;
  }
};

int main(int argc, char* argv[]) {
  auto options = getOptions();
  auto whole_program_info = PropellerWholeProgramInfo::Create(options);
  auto s = whole_program_info->CreateCfgs();
  if (!s) {
    LOG(ERROR) << s;
    return 1;
  }

  std::vector<Record> hot, cold, normal;
  uint32_t t = absl::GetFlag(FLAGS_hot_threshold);
  std::fstream output(absl::GetFlag(FLAGS_output), std::ios_base::out);
  for (auto* p : whole_program_info->GetHotCfgs()) {
    bool hot_on_entry = p->GetEntryNode()->freq() >= t;
    if (hot_on_entry) output << p->GetPrimaryName().str() << std::endl;

    if (p->IsHot()) {
      hot.push_back(
          Record{p->GetPrimaryName().str(), p->GetEntryNode()->freq()});
    } else if (p->GetEntryNode()->freq() >= t) {
      normal.push_back(
          Record{p->GetPrimaryName().str(), p->GetEntryNode()->freq()});
    } else {
      normal.push_back(
          Record{p->GetPrimaryName().str(), p->GetEntryNode()->freq()});
    }
  }

  std::sort(hot.begin(), hot.end());
  std::sort(normal.begin(), normal.end());
  std::sort(cold.begin(), cold.end());

  std::vector<Record>* buffer[3] = {&hot, &cold, &normal};
  std::vector<std::string> names = {"hot", "cold", "normal"};
  std::ofstream fout(absl::GetFlag(FLAGS_detail), std::ios_base::out);
  for (int i = 0; i < 3; ++i) {
    fout << names[i] << " functions:" << std::endl;
    for (int j = 0; j < buffer[i]->size(); ++j) {
      Record& c = buffer[i]->at(j);
      fout << c.name << " " << c.freq << std::endl;
    }
  }

  return 0;
}

#else
int main(int argc, char* argv[]) {
  std::cerr << "Error: This tool must be built with LLVM support" << std::endl;
  return 0;
}

#endif