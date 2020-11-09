#ifndef DEVTOOLS_CROSSTOOL_AUTOFDO_BASE_PROFILE_READER_H_
#define DEVTOOLS_CROSSTOOL_AUTOFDO_BASE_PROFILE_READER_H_

#include <string>

namespace devtools_crosstool_autofdo {
class ProfileReader {
 public:
  virtual ~ProfileReader() {}

  virtual bool ReadFromFile(const std::string &output_file) = 0;
};
}  // namespace devtools_crosstool_autofdo

#endif  // DEVTOOLS_CROSSTOOL_AUTOFDO_BASE_PROFILE_READER_H_
