// Read profile from the .afdo file. The format of .afdo file is in
// profile_writer.h.

#ifndef AUTOFDO_PROFILE_READER_H_
#define AUTOFDO_PROFILE_READER_H_

#include <string>
#include <vector>

#include "base_profile_reader.h"
#include "symbol_map.h"

namespace devtools_crosstool_autofdo {

class SymbolMap;

class AutoFDOProfileReader : public ProfileReader {
 public:
  // None of the args are owned by this class.
  explicit AutoFDOProfileReader(SymbolMap *symbol_map, bool force_update)
      : symbol_map_(symbol_map), force_update_(force_update) {}

  explicit AutoFDOProfileReader()
      : symbol_map_(nullptr), force_update_(false) {}

  bool ReadFromFile(const std::string &output_file) override;

 private:
  void ReadWorkingSet();
  // Reads the module grouping info into the gcda file.
  // TODO(b/132437226): LIPO has been deprecated so no module grouping info
  // is needed in the gcda file. However, even if no LIPO is used, gcc used
  // by chromeOS kernel will still check the module grouping fields whenever
  // it reads a gcda file. To be compatible, we keep the minimum fields which
  // are necessary for gcc to be able to read a gcda file and remove the
  // rest of LIPO stuff.
  // We can remove the leftover if chromeOS kernel starts using llvm or can
  // change their gcc in sync with autofdo tool.
  //
  // The minimum fields to keep:
  // TAG
  // Length of the section (will always be 0)
  // Number of modules (will always be 0)
  void ReadModuleGroup();
  void ReadFunctionProfile();
  // Reads in profile recursively. Updates the symbol_map_ if update or
  // force_update_ is true. Otherwise just read in and dump the data.
  // The reason we need "update" is because during profile_update, we first
  // read in symbol_map from the binary, which includes the alias info. Then
  // when we read in profiles from profile, we don't want to update the
  // profiles for the aliased symbols as they all point to the same symbol.
  // With force_update_==true, we know that the ReadSymbolProfile request is
  // not from profile_update but tools like profile_merger and profile_dump,
  // where symbol_map was built purely from profile thus alias symbol info
  // is not available. In that case, we should always update the symbol.
  void ReadSymbolProfile(const SourceStack &stack, bool update);
  void ReadNameTable();

  SymbolMap *symbol_map_;
  bool force_update_;
  std::vector<std::pair<std::string, int>> names_;
  std::vector<std::string> file_names_;
};

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_PROFILE_READER_H_
