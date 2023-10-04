// Copyright 2013 Google Inc. All Rights Reserved.
// Author: dehao@google.com (Dehao Chen)

// Class to derive inline stack.

#ifndef AUTOFDO_ADDR2LINE_H_
#define AUTOFDO_ADDR2LINE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "base/integral_types.h"
#include "source_info.h"
#include "third_party/abseil/absl/strings/string_view.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"

namespace devtools_crosstool_autofdo {
class Addr2line {
 public:
  explicit Addr2line(absl::string_view binary_name)
      : binary_name_(binary_name) {}

  // This type is neither copyable nor movable.
  Addr2line(const Addr2line &) = delete;
  Addr2line &operator=(const Addr2line &) = delete;

  virtual ~Addr2line() {}

  static Addr2line *Create(absl::string_view binary_name);

  // Reads the binary to prepare necessary binary in data.
  // Returns True on success.
  virtual bool Prepare() = 0;

  // Stores the inline stack of ADDR in STACK.
  virtual void GetInlineStack(uint64_t addr, SourceStack *stack) const = 0;

  // Return the object file.
  virtual const llvm::object::ObjectFile *getObject() const { return nullptr; }

 protected:
  std::string binary_name_;
};

class LLVMAddr2line : public Addr2line {
 public:
  explicit LLVMAddr2line(absl::string_view binary_name);
  bool Prepare() override;
  void GetInlineStack(uint64_t address, SourceStack *stack) const override;
  const llvm::object::ObjectFile *getObject() const override {
    return binary_.getBinary();
  }

 private:
  // map from cu_offset to the CompileUnit.
  std::map<uint32_t, llvm::DWARFUnit *> unit_map_;
  llvm::object::OwningBinary<llvm::object::ObjectFile> binary_;
  std::unique_ptr<llvm::DWARFContext> dwarf_info_;
};
}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_ADDR2LINE_H_
