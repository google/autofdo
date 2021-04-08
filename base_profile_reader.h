#ifndef AUTOFDO_BASE_PROFILE_READER_H_
#define AUTOFDO_BASE_PROFILE_READER_H_

#include <string>

namespace devtools_crosstool_autofdo {
class ProfileReader {
 public:
  virtual ~ProfileReader() {}

  virtual bool ReadFromFile(const std::string &output_file) = 0;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_BASE_PROFILE_READER_H_
