// Copyright 2013 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Class to derive inline stack.

#ifndef AUTOFDO_ADDR2LINE_H_
#define AUTOFDO_ADDR2LINE_H_

#include <map>
#include <string>

#include "base/integral_types.h"
#include "base/macros.h"
#include "source_info.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"

namespace devtools_crosstool_autofdo {
class Addr2line {
 public:
  explicit Addr2line(const std::string &binary_name)
      : binary_name_(binary_name) {}

  virtual ~Addr2line() {}

  static Addr2line *Create(const std::string &binary_name);

  static Addr2line *CreateWithSampledFunctions(
      const std::string &binary_name,
      const std::map<uint64, uint64> *sampled_functions);

  // Reads the binary to prepare necessary binary in data.
  // Returns True on success.
  virtual bool Prepare() = 0;

  // Stores the inline stack of ADDR in STACK.
  virtual void GetInlineStack(uint64 addr, SourceStack *stack) const = 0;

 protected:
  std::string binary_name_;

 private:
  DISALLOW_COPY_AND_ASSIGN(Addr2line);
};

class LLVMAddr2line : public Addr2line {
 public:
  explicit LLVMAddr2line(const std::string &binary_name);
  bool Prepare() override;
  void GetInlineStack(uint64 address, SourceStack *stack) const override;

 private:
  // map from cu_offset to the CompileUnit.
  std::map<uint32, llvm::DWARFUnit *> unit_map_;
  llvm::object::OwningBinary<llvm::object::ObjectFile> binary_;
  std::unique_ptr<llvm::DWARFContext> dwarf_info_;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_ADDR2LINE_H_
