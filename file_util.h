#ifndef AUTOFDO_FILE_UTIL_H_
#define AUTOFDO_FILE_UTIL_H_

#include <fstream>
#include <sstream>
#include <string>

#include "third_party/abseil/absl/strings/string_view.h"
#include "third_party/abseil/absl/status/status.h"
#include "third_party/abseil/absl/strings/string_view.h"

namespace file {
absl::Status GetContents(absl::string_view file_name, std::string *content) {
  std::string file_name_str(file_name);
  std::ifstream input_file(file_name_str);
  if (!input_file.is_open()) {
    return absl::FailedPreconditionError(
        absl::StrCat("Error opening file:'", file_name, "'."));
  } else {
    *content = std::string((std::istreambuf_iterator<char>(input_file)),
                          std::istreambuf_iterator<char>());
    input_file.close();
  }
  return absl::OkStatus();
}
}  // namespace file

#endif  // AUTOFDO_FILE_UTIL_H_
