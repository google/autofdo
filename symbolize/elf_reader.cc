// Copyright 2014 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Code for reading in ELF files.
//
// For information on the ELF format, see
// http://www.x86.org/ftp/manuals/tools/elf.pdf
//
// I also liked:
// http://www.caldera.com/developers/gabi/1998-04-29/contents.html
//
// A note about types: When dealing with the file format, we use types
// like Elf32_Word, but in the public interfaces we treat all
// addresses as uint64. As a result, we should be able to symbolize
// 64-bit binaries from a 32-bit process (which we don't do,
// anyway). size_t should therefore be avoided, except where required
// by things like mmap().
//
// Although most of this code can deal with arbitrary ELF files of
// either word size, the public ElfReader interface only examines
// files loaded into the current address space, which must all match
// __WORDSIZE. This code cannot handle ELF files with a non-native
// byte ordering.
//
// TODO(chatham): It would be nice if we could accomplish this task
// without using malloc(), so we could use it as the process is dying.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // needed for pread()
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "symbolize/elf_reader.h"
#include "base/common.h"

namespace {

// The lowest bit of an ARM symbol value is used to indicate a Thumb address.
const int kARMThumbBitOffset = 0;

// Converts an ARM Thumb symbol value to a true aligned address value.
template <typename T>
T AdjustARMThumbSymbolValue(const T& symbol_table_value) {
  return symbol_table_value & ~(1 << kARMThumbBitOffset);
}
}  // namespace

namespace autofdo {

template <class ElfArch> class ElfReaderImpl;

// 32-bit and 64-bit ELF files are processed exactly the same, except
// for various field sizes. Elf32 and Elf64 encompass all of the
// differences between the two formats, and all format-specific code
// in this file is templated on one of them.
class Elf32 {
 public:
  typedef Elf32_Ehdr Ehdr;
  typedef Elf32_Shdr Shdr;
  typedef Elf32_Phdr Phdr;
  typedef Elf32_Word Word;
  typedef Elf32_Sym Sym;

  // What should be in the EI_CLASS header.
  static const int kElfClass = ELFCLASS32;

  // Given a symbol pointer, return the binding type (eg STB_WEAK).
  static char Bind(const Elf32_Sym *sym) {
    return ELF32_ST_BIND(sym->st_info);
  }
  // Given a symbol pointer, return the symbol type (eg STT_FUNC).
  static char Type(const Elf32_Sym *sym) {
    return ELF32_ST_TYPE(sym->st_info);
  }
};


class Elf64 {
 public:
  typedef Elf64_Ehdr Ehdr;
  typedef Elf64_Shdr Shdr;
  typedef Elf64_Phdr Phdr;
  typedef Elf64_Word Word;
  typedef Elf64_Sym Sym;

  // What should be in the EI_CLASS header.
  static const int kElfClass = ELFCLASS64;

  static char Bind(const Elf64_Sym *sym) {
    return ELF64_ST_BIND(sym->st_info);
  }
  static char Type(const Elf64_Sym *sym) {
    return ELF64_ST_TYPE(sym->st_info);
  }
};


// ElfSectionReader mmaps a section of an ELF file ("section" is ELF
// terminology). The ElfReaderImpl object providing the section header
// must exist for the lifetime of this object.
//
// The motivation for mmaping individual sections of the file is that
// many Google executables are large enough when unstripped that we
// have to worry about running out of virtual address space.
template<class ElfArch>
class ElfSectionReader {
 public:
  ElfSectionReader(const string &path, int fd,
                   const typename ElfArch::Shdr &section_header)
      : header_(section_header) {
    // Back up to the beginning of the page we're interested in.
    const size_t additional = header_.sh_offset % getpagesize();
    const size_t offset_aligned = header_.sh_offset - additional;
    section_size_ = header_.sh_size;
    size_aligned_ = section_size_ + additional;
    contents_aligned_ = mmap(NULL, size_aligned_, PROT_READ, MAP_SHARED,
                             fd, offset_aligned);
    if (contents_aligned_ == MAP_FAILED)
      PLOG(FATAL) << "Could not mmap " << path;
    // Set where the offset really should begin.
    contents_ = reinterpret_cast<const char*>(contents_aligned_) +
                (header_.sh_offset - offset_aligned);
  }

  ~ElfSectionReader() {
    munmap(contents_aligned_, size_aligned_);
  }

  // Return the section header for this section.
  typename ElfArch::Shdr const &header() const { return header_; }

  // Return memory at the given offset within this section.
  const char *GetOffset(typename ElfArch::Word bytes) const {
    CHECK(contents_ != NULL);
    return contents_ + bytes;
  }

  const char *contents() const { return contents_; }
  size_t section_size() const { return section_size_; }

 private:
  // page-aligned file contents
  void *contents_aligned_;
  // pointer within contents_aligned_ to where the section data begins
  const char *contents_;
  // size of contents_aligned_
  size_t size_aligned_;
  // size of contents.
  size_t section_size_;
  const typename ElfArch::Shdr header_;

  DISALLOW_EVIL_CONSTRUCTORS(ElfSectionReader);
};

// An iterator over symbols in a given section. It handles walking
// through the entries in the specified section and mapping symbol
// entries to their names in the appropriate string table (in
// another section).
template<class ElfArch>
class SymbolIterator {
 public:
  SymbolIterator(ElfReaderImpl<ElfArch> *reader,
                 typename ElfArch::Word section_type)
      : symbol_section_(reader->GetSectionByType(section_type)),
        string_section_(NULL),
        num_symbols_in_section_(0),
        symbol_within_section_(0) {
    CHECK(section_type == SHT_SYMTAB || section_type == SHT_DYNSYM);

    // If this section type doesn't exist, leave
    // num_symbols_in_section_ as zero, so this iterator is already
    // done().
    if (symbol_section_ != NULL) {
      num_symbols_in_section_ = symbol_section_->header().sh_size /
                                symbol_section_->header().sh_entsize;

      // Symbol sections have sh_link set to the section number of
      // the string section containing the symbol names.
      CHECK_NE(symbol_section_->header().sh_link, 0);
      string_section_ = reader->GetSection(symbol_section_->header().sh_link);
    }
  }

  // Return true iff we have passed all symbols in this section.
  bool done() const {
    return symbol_within_section_ >= num_symbols_in_section_;
  }

  // Advance to the next symbol in this section.
  // REQUIRES: !done()
  void Next() { ++symbol_within_section_; }

  // Return a pointer to the current symbol.
  // REQUIRES: !done()
  const typename ElfArch::Sym *GetSymbol() const {
    CHECK(!done());
    return reinterpret_cast<const typename ElfArch::Sym*>(
        symbol_section_->GetOffset(symbol_within_section_ *
                                   symbol_section_->header().sh_entsize));
  }

  // Return the name of the current symbol, NULL if it has none.
  // REQUIRES: !done()
  const char *GetSymbolName() const {
    int name_offset = GetSymbol()->st_name;
    if (name_offset == 0)
      return NULL;
    return string_section_->GetOffset(name_offset);
  }

 private:
  const ElfSectionReader<ElfArch> *const symbol_section_;
  const ElfSectionReader<ElfArch> *string_section_;
  int num_symbols_in_section_;
  int symbol_within_section_;
  DISALLOW_EVIL_CONSTRUCTORS(SymbolIterator);
};

// Copied from strings/strutil.h.  Per chatham,
// this library should not depend on strings.

static inline bool MyHasSuffixString(const string& str, const string& suffix) {
  int len = str.length();
  int suflen = suffix.length();
  return (suflen <= len) && (str.compare(len-suflen, suflen, suffix) == 0);
}


// ElfReader loads an ELF binary and can provide information about its
// contents. It is most useful for matching addresses to function
// names. It does not understand debugging formats (eg dwarf2), so it
// can't print line numbers. It takes a path to an elf file and a
// readable file descriptor for that file, which it does not assume
// ownership of.
template<class ElfArch>
class ElfReaderImpl {
 public:
  explicit ElfReaderImpl(const string &path, int fd)
      : path_(path),
        fd_(fd),
        section_headers_(NULL),
        program_headers_(NULL) {
    CHECK_GE(fd_, 0);
    string error;
    CHECK(IsArchElfFile(fd, &error)) << " Could not parse file: " << error;
    is_dwp_ = MyHasSuffixString(path, ".dwp");
    ParseHeaders(fd, path);
  }

  ~ElfReaderImpl() {
    for (int i = 0; i < sections_.size(); ++i)
      delete sections_[i];
    delete [] section_headers_;
    delete [] program_headers_;
  }

  // Examine the headers of the file and return whether the file looks
  // like an ELF file for this architecture. Takes an already-open
  // file descriptor for the candidate file, reading in the prologue
  // to see if the ELF file appears to match the current
  // architecture. If error is non-NULL, it will be set with a reason
  // in case of failure.
  static bool IsArchElfFile(int fd, string *error) {
    unsigned char header[EI_NIDENT];
    if (pread(fd, header, sizeof(header), 0) != sizeof(header)) {
      if (error != NULL) *error = "Could not read header";
      return false;
    }

    if (memcmp(header, ELFMAG, SELFMAG) != 0) {
      if (error != NULL) *error = "Missing ELF magic";
      return false;
    }

    if (header[EI_CLASS] != ElfArch::kElfClass) {
      if (error != NULL) *error = "Different word size";
      return false;
    }

    int endian = 0;
    if (header[EI_DATA] == ELFDATA2LSB)
      endian = __LITTLE_ENDIAN;
    else if (header[EI_DATA] == ELFDATA2MSB)
      endian = __BIG_ENDIAN;
    if (endian != __BYTE_ORDER) {
      if (error != NULL) *error = "Different byte order";
      return false;
    }

    return true;
  }

  void VisitSymbols(typename ElfArch::Word section_type,
                    ElfReader::SymbolSink *sink) {
    VisitSymbols(section_type, sink, -1, -1, false);
  }

  void VisitSymbols(typename ElfArch::Word section_type,
                    ElfReader::SymbolSink *sink,
                    int symbol_binding,
                    int symbol_type,
                    bool get_raw_symbol_values) {
    for (SymbolIterator<ElfArch> it(this, section_type);
         !it.done(); it.Next()) {
      const char *name = it.GetSymbolName();
      if (!name) continue;
      const typename ElfArch::Sym *sym = it.GetSymbol();
      if ((symbol_binding < 0 || ElfArch::Bind(sym) == symbol_binding) &&
          (symbol_type < 0 || ElfArch::Type(sym) == symbol_type)) {
        typename ElfArch::Sym symbol = *sym;
        if (!get_raw_symbol_values)
          AdjustSymbolValue(&symbol);
        sink->AddSymbol(name, symbol.st_value, symbol.st_size);
      }
    }
  }

  // Return an ElfSectionReader for the first section of the given
  // type by iterating through all section headers. Returns NULL if
  // the section type is not found.
  const ElfSectionReader<ElfArch> *GetSectionByType(
      typename ElfArch::Word section_type) {
    for (int k = 0; k < GetNumSections(); ++k) {
      if (section_headers_[k].sh_type == section_type) {
        return GetSection(k);
      }
    }
    return NULL;
  }

  // Return the name of section "shndx".  Returns NULL if the section
  // is not found.
  const char *GetSectionNameByIndex(int shndx) {
    return GetSectionName(section_headers_[shndx].sh_name);
  }

  // Return a pointer to section "shndx", and store the size in
  // "size".  Returns NULL if the section is not found.
  const char *GetSectionContentsByIndex(int shndx, size_t *size) {
    const ElfSectionReader<ElfArch> *section = GetSection(shndx);
    if (section != NULL) {
      *size = section->section_size();
      return section->contents();
    }
    return NULL;
  }

  // Return a pointer to the first section of the given name by
  // iterating through all section headers, and store the size in
  // "size".  Returns NULL if the section name is not found.
  const char *GetSectionContentsByName(const string &section_name,
                                       size_t *size) {
    for (int k = 0; k < GetNumSections(); ++k) {
      // When searching for sections in a .dwp file, the sections
      // we're looking for will always be at the end of the section
      // table, so reverse the direction of iteration.
      int shndx = is_dwp_ ? GetNumSections() - k - 1 : k;
      const char *name = GetSectionName(section_headers_[shndx].sh_name);
      if (name != NULL && ElfReader::SectionNamesMatch(section_name, name)) {
        const ElfSectionReader<ElfArch> *section = GetSection(shndx);
        if (section == NULL) {
          return NULL;
        } else {
          *size = section->section_size();
          return section->contents();
        }
      }
    }
    return NULL;
  }

  // This is like GetSectionContentsByName() but it returns a lot of extra
  // information about the section.
  const char *GetSectionInfoByName(const string &section_name,
                                   ElfReader::SectionInfo *info) {
    for (int k = 0; k < GetNumSections(); ++k) {
      // When searching for sections in a .dwp file, the sections
      // we're looking for will always be at the end of the section
      // table, so reverse the direction of iteration.
      int shndx = is_dwp_ ? GetNumSections() - k - 1 : k;
      const char *name = GetSectionName(section_headers_[shndx].sh_name);
      if (name != NULL && ElfReader::SectionNamesMatch(section_name, name)) {
        const ElfSectionReader<ElfArch> *section = GetSection(shndx);
        if (section == NULL) {
          return NULL;
        } else {
          info->type = section->header().sh_type;
          info->flags = section->header().sh_flags;
          info->addr = section->header().sh_addr;
          info->offset = section->header().sh_offset;
          info->size = section->header().sh_size;
          info->link = section->header().sh_link;
          info->info = section->header().sh_info;
          info->addralign = section->header().sh_addralign;
          info->entsize = section->header().sh_entsize;
          return section->contents();
        }
      }
    }
    return NULL;
  }

  // p_vaddr of the first PT_LOAD segment (if any), or 0 if no PT_LOAD
  // segments are present. This is the address an ELF image was linked
  // (by static linker) to be loaded at. Usually (but not always) 0 for
  // shared libraries and position-independent executables.
  uint64 VaddrOfFirstLoadSegment() const {
    // Relocatable objects (of type ET_REL) do not have LOAD segments.
    if (header_.e_type == ET_REL) {
      return 0;
    }
    for (int i = 0; i < GetNumProgramHeaders(); ++i) {
      if (program_headers_[i].p_type == PT_LOAD) {
        return program_headers_[i].p_vaddr;
      }
    }
    LOG(ERROR) << "Could not find LOAD from program header: " << path_;
    return 0;
  }

  // According to the LSB ("ELF special sections"), sections with debug
  // info are prefixed by ".debug".  The names are not specified, but they
  // look like ".debug_line", ".debug_info", etc.
  bool HasDebugSections() {
    // Debug sections are likely to be near the end, so reverse the
    // direction of iteration.
    for (int k = GetNumSections() - 1; k >= 0; --k) {
      const char *name = GetSectionName(section_headers_[k].sh_name);
      if (strncmp(name, ".debug", strlen(".debug")) == 0)
        return true;
    }
    return false;
  }

  bool IsDynamicSharedObject() const {
    return header_.e_type == ET_DYN;
  }

 private:
  typedef vector<pair<uint64, const typename ElfArch::Sym *> > AddrToSymMap;

  static bool AddrToSymSorter(const typename AddrToSymMap::value_type& lhs,
                              const typename AddrToSymMap::value_type& rhs) {
    return lhs.first < rhs.first;
  }

  static bool AddrToSymEquals(const typename AddrToSymMap::value_type& lhs,
                              const typename AddrToSymMap::value_type& rhs) {
    return lhs.first == rhs.first;
  }

  // Does this ELF file have too many sections to fit in the program header?
  bool HasManySections() const {
    return header_.e_shnum == SHN_UNDEF;
  }

  // Return the number of program headers.
  int GetNumProgramHeaders() const {
    if (HasManySections() && header_.e_phnum == 0xffff &&
        first_section_header_.sh_info != 0)
      return first_section_header_.sh_info;
    return header_.e_phnum;
  }

  // Return the number of sections.
  int GetNumSections() const {
    if (HasManySections())
      return first_section_header_.sh_size;
    return header_.e_shnum;
  }

  // Return the index of the string table.
  int GetStringTableIndex() const {
    if (HasManySections()) {
      if (header_.e_shstrndx == 0xffff)
        return first_section_header_.sh_link;
      else if (header_.e_shstrndx >= GetNumSections())
        return 0;
    }
    return header_.e_shstrndx;
  }

  // Given an offset into the section header string table, return the
  // section name.
  const char *GetSectionName(typename ElfArch::Word sh_name) {
    const ElfSectionReader<ElfArch> *shstrtab =
        GetSection(GetStringTableIndex());
    if (shstrtab != NULL) {
      CHECK_GE(shstrtab->section_size(), sh_name);
      return shstrtab->GetOffset(sh_name);
    }
    return NULL;
  }

  // Return an ElfSectionReader for the given section. The reader will
  // be freed when this object is destroyed.
  const ElfSectionReader<ElfArch> *GetSection(int num) {
    CHECK_LT(num, GetNumSections());
    const char *name;
    // Hard-coding the name for the section-name string table prevents
    // infinite recursion.
    if (num == GetStringTableIndex())
      name = ".shstrtab";
    else
      name = GetSectionNameByIndex(num);
    ElfSectionReader<ElfArch> *& reader = sections_[num];
    if (reader == NULL)
      reader = new ElfSectionReader<ElfArch>(path_, fd_,
                                             section_headers_[num]);
    return reader;
  }

  // Parse out the overall header information from the file and assert
  // that it looks sane. This contains information like the magic
  // number and target architecture.
  bool ParseHeaders(int fd, const string &path) {
    // Read in the global ELF header.
    if (pread(fd, &header_, sizeof(header_), 0) != sizeof(header_)) {
      LOG(ERROR) << "Could not read ELF header: " << path;
      return false;
    }

    // Must be an executable, dynamic shared object or relocatable object
    if (header_.e_type != ET_EXEC &&
        header_.e_type != ET_DYN &&
        header_.e_type != ET_REL) {
      LOG(ERROR) << "Not an executable, shared object or relocatable object "
                    "file: " << path;
      return false;
    }
    // Need a section header.
    if (header_.e_shoff == 0) {
      LOG(ERROR) << "No section header: " << path;
      return false;
    }

    if (header_.e_shnum == SHN_UNDEF) {
      // The number of sections in the program header is only a 16-bit value. In
      // the event of overflow (greater than SHN_LORESERVE sections), e_shnum
      // will read SHN_UNDEF and the true number of section header table entries
      // is found in the sh_size field of the first section header.
      // See: http://www.sco.com/developers/gabi/2003-12-17/ch4.sheader.html
      if (pread(fd, &first_section_header_, sizeof(first_section_header_),
                header_.e_shoff) != sizeof(first_section_header_)) {
        LOG(ERROR) << "Failed to read first section header: " << path;
        return false;
      }
    }

    // Dynamically allocate enough space to store the section headers
    // and read them out of the file.
    const int section_headers_size =
        GetNumSections() * sizeof(*section_headers_);
    section_headers_ = new typename ElfArch::Shdr[section_headers_size];
    if (pread(fd, section_headers_, section_headers_size, header_.e_shoff) !=
        section_headers_size) {
      LOG(ERROR) << "Could not read section headers: " << path;
      return false;
    }

    // Dynamically allocate enough space to store the program headers
    // and read them out of the file.
    const int program_headers_size =
        GetNumProgramHeaders() * sizeof(*program_headers_);
    program_headers_ = new typename ElfArch::Phdr[GetNumProgramHeaders()];
    if (pread(fd, program_headers_, program_headers_size, header_.e_phoff) !=
        program_headers_size) {
      LOG(ERROR) << "Could not read program headers: " << path
                 << " Continue anyway";
    }

    // Presize the sections array for efficiency.
    sections_.resize(GetNumSections(), NULL);
    return true;
  }

  void AdjustSymbolValue(typename ElfArch::Sym* sym) {
    switch (header_.e_machine) {
    case EM_ARM:
      // For ARM architecture, if the LSB of the function symbol offset is set,
      // it indicates a Thumb function.  This bit should not be taken literally.
      // Clear it.
      if (ElfArch::Type(sym) == STT_FUNC)
        sym->st_value = AdjustARMThumbSymbolValue(sym->st_value);
      break;
    case EM_386:
      // No adjustment needed for Intel x86 architecture.  However, explicitly
      // define this case as we use it quite often.
      break;
    case EM_PPC:
      // PowerPC architecture may need adjustment in the future.
      break;
    default:
      break;
    }
  }

  friend class SymbolIterator<ElfArch>;

  // The file we're reading.
  const string path_;
  // Open file descriptor for path_. Not owned by this object.
  const int fd_;

  // The global header of the ELF file.
  typename ElfArch::Ehdr header_;

  // The header of the first section. This may be used to supplement the ELF
  // file header.
  typename ElfArch::Shdr first_section_header_;

  // Array of GetNumSections() section headers, allocated when we read
  // in the global header.
  typename ElfArch::Shdr *section_headers_;

  // Array of GetNumProgramHeaders() program headers, allocated when we read
  // in the global header.
  typename ElfArch::Phdr *program_headers_;

  // An array of pointers to ElfSectionReaders. Sections are
  // mmaped as they're needed and not released until this object is
  // destroyed.
  vector<ElfSectionReader<ElfArch>*> sections_;

  // True if this is a .dwp file.
  bool is_dwp_;

  DISALLOW_EVIL_CONSTRUCTORS(ElfReaderImpl);
};

ElfReader::ElfReader(const string &path)
    : path_(path), fd_(-1), impl32_(NULL), impl64_(NULL) {
  // linux 2.6.XX kernel can show deleted files like this:
  //   /var/run/nscd/dbYLJYaE (deleted)
  // and the kernel-supplied vdso and vsyscall mappings like this:
  //   [vdso]
  //   [vsyscall]
  if (MyHasSuffixString(path, " (deleted)"))
    return;
  if (path == "[vdso]")
    return;
  if (path == "[vsyscall]")
    return;

  fd_ = open(path.c_str(), O_RDONLY);
  if (fd_ == -1) {
    // Not ERROR, since this gets called with things like "[heap]".
    PLOG(INFO) << "Could not open " << path_;
  }
}

ElfReader::~ElfReader() {
  if (fd_ != -1)
    close(fd_);
  if (impl32_ != NULL)
    delete impl32_;
  if (impl64_ != NULL)
    delete impl64_;
}


// The only word-size specific part of this file is IsNativeElfFile().
#if __WORDSIZE == 32
#define NATIVE_ELF_ARCH Elf32
#elif __WORDSIZE == 64
#define NATIVE_ELF_ARCH Elf64
#else
#error "Invalid word size"
#endif

template <typename ElfArch>
static bool IsElfFile(const int fd, const string &path) {
  if (fd < 0)
    return false;
  if (!ElfReaderImpl<ElfArch>::IsArchElfFile(fd, NULL)) {
    // No error message here.  IsElfFile gets called many times.
    return false;
  }
  return true;
}

bool ElfReader::IsNativeElfFile() const {
  return IsElfFile<NATIVE_ELF_ARCH>(fd_, path_);
}

bool ElfReader::IsElf32File() const {
  return IsElfFile<Elf32>(fd_, path_);
}

bool ElfReader::IsElf64File() const {
  return IsElfFile<Elf64>(fd_, path_);
}

void ElfReader::VisitSymbols(ElfReader::SymbolSink *sink) {
  VisitSymbols(sink, -1, -1);
}

void ElfReader::VisitSymbols(ElfReader::SymbolSink *sink,
                             int symbol_binding,
                             int symbol_type) {
  VisitSymbols(sink, symbol_binding, symbol_type, false);
}

void ElfReader::VisitSymbols(ElfReader::SymbolSink *sink,
                             int symbol_binding,
                             int symbol_type,
                             bool get_raw_symbol_values) {
  if (IsElf32File()) {
    GetImpl32()->VisitSymbols(SHT_SYMTAB, sink, symbol_binding, symbol_type,
                              get_raw_symbol_values);
    GetImpl32()->VisitSymbols(SHT_DYNSYM, sink, symbol_binding, symbol_type,
                              get_raw_symbol_values);
  } else if (IsElf64File()) {
    GetImpl64()->VisitSymbols(SHT_SYMTAB, sink, symbol_binding, symbol_type,
                              get_raw_symbol_values);
    GetImpl64()->VisitSymbols(SHT_DYNSYM, sink, symbol_binding, symbol_type,
                              get_raw_symbol_values);
  }
}

uint64 ElfReader::VaddrOfFirstLoadSegment() {
  if (IsElf32File()) {
    return GetImpl32()->VaddrOfFirstLoadSegment();
  } else if (IsElf64File()) {
    return GetImpl64()->VaddrOfFirstLoadSegment();
  } else {
    LOG(ERROR) << "not an elf binary: " << path_;
    return 0;
  }
}

const char *ElfReader::GetSectionName(int shndx) {
  if (IsElf32File()) {
    return GetImpl32()->GetSectionNameByIndex(shndx);
  } else if (IsElf64File()) {
    return GetImpl64()->GetSectionNameByIndex(shndx);
  } else {
    LOG(ERROR) << "not an elf binary: " << path_;
    return NULL;
  }
}

const char *ElfReader::GetSectionByIndex(int shndx, size_t *size) {
  if (IsElf32File()) {
    return GetImpl32()->GetSectionContentsByIndex(shndx, size);
  } else if (IsElf64File()) {
    return GetImpl64()->GetSectionContentsByIndex(shndx, size);
  } else {
    LOG(ERROR) << "not an elf binary: " << path_;
    return NULL;
  }
}

const char *ElfReader::GetSectionByName(const string &section_name,
                                        size_t *size) {
  if (IsElf32File()) {
    return GetImpl32()->GetSectionContentsByName(section_name, size);
  } else if (IsElf64File()) {
    return GetImpl64()->GetSectionContentsByName(section_name, size);
  } else {
    LOG(ERROR) << "not an elf binary: " << path_;
    return NULL;
  }
}

const char *ElfReader::GetSectionInfoByName(const string &section_name,
                                            SectionInfo *info) {
  if (IsElf32File()) {
    return GetImpl32()->GetSectionInfoByName(section_name, info);
  } else if (IsElf64File()) {
    return GetImpl64()->GetSectionInfoByName(section_name, info);
  } else {
    LOG(ERROR) << "not an elf binary: " << path_;
    return NULL;
  }
}

bool ElfReader::SectionNamesMatch(const string &name, const string &sh_name) {
  if ((name.find(".debug_", 0) == 0) && (sh_name.find(".zdebug_", 0) == 0)) {
    const string name_suffix(name, strlen(".debug_"));
    const string sh_name_suffix(sh_name, strlen(".zdebug_"));
    return name_suffix == sh_name_suffix;
  }
  return name == sh_name;
}

string ElfReader::GetBuildId() {
  size_t size;

  // Hex dump of section '.note.gnu.build-id':
  // 0x00400280 04000000 10000000 03000000 474e5500 ............GNU.
  // 0x00400290 76fe55ee a70df375 dd752205 334a51e0 v.U....u.u".3JQ.
  const char *build_id_section = ".note.gnu.build-id";
  const char *build_id_suffix = "00000000";
  const char *data = GetSectionByName(build_id_section, &size);
  if (size != 32) {
    LOG(ERROR) << "Malformed .note.gnu.build-id section.";
    return "";
  }

  char build_id[33];
  const char *src = data + 16;
  for (int i = 0; i < 16; i++) {
    snprintf(&build_id[2*i], sizeof(build_id[i]) * 3, "%02x", src[i]);
  }
  return string(build_id) + build_id_suffix;
}

bool ElfReader::IsDynamicSharedObject() {
  if (IsElf32File()) {
    return GetImpl32()->IsDynamicSharedObject();
  } else if (IsElf64File()) {
    return GetImpl64()->IsDynamicSharedObject();
  } else {
    LOG(ERROR) << "not an elf binary: " << path_;
    return false;
  }
}

ElfReaderImpl<Elf32> *ElfReader::GetImpl32() {
  if (impl32_ == NULL) {
    impl32_ = new ElfReaderImpl<Elf32>(path_, fd_);
  }
  return impl32_;
}

ElfReaderImpl<Elf64> *ElfReader::GetImpl64() {
  if (impl64_ == NULL) {
    impl64_ = new ElfReaderImpl<Elf64>(path_, fd_);
  }
  return impl64_;
}

// Return true if file is an ELF binary of ElfArch, with unstripped
// debug info (debug_only=true) or symbol table (debug_only=false).
// Otherwise, return false.
template <typename ElfArch>
static bool IsNonStrippedELFBinaryImpl(const string &path, const int fd,
                                       bool debug_only) {
  if (!ElfReaderImpl<ElfArch>::IsArchElfFile(fd, NULL)) return false;
  ElfReaderImpl<ElfArch> elf_reader(path, fd);
  return debug_only ?
      elf_reader.HasDebugSections()
      : (elf_reader.GetSectionByType(SHT_SYMTAB) != NULL);
}

// Helper for the IsNon[Debug]StrippedELFBinary functions.
static bool IsNonStrippedELFBinaryHelper(const string &path,
                                         bool debug_only) {
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd == -1) {
    return false;
  }

  if (IsNonStrippedELFBinaryImpl<Elf32>(path, fd, debug_only) ||
      IsNonStrippedELFBinaryImpl<Elf64>(path, fd, debug_only)) {
    close(fd);
    return true;
  }
  close(fd);
  return false;
}

bool ElfReader::IsNonStrippedELFBinary(const string &path) {
  return IsNonStrippedELFBinaryHelper(path, false);
}

bool ElfReader::IsNonDebugStrippedELFBinary(const string &path) {
  return IsNonStrippedELFBinaryHelper(path, true);
}
}  // namespace autofdo
