#ifndef _LLVM_PROPELLER_PROFILE_FORMAT_H_
#define _LLVM_PROPELLER_PROFILE_FORMAT_H_


#include "llvm_propeller_profile_writer.h"
#include "llvm_propeller_path_profile.h"
#include "llvm/ProfileData/BBSectionsProf.h"
#include "third_party/perf_data_converter/src/quipper/perf_data.pb.h"
#include "third_party/perf_data_converter/src/quipper/perf_data_utils.h"

#include <iomanip>
#include <list>

using llvm::StringRef;
using llvm::propeller::SymbolEntry;

static struct DecOut {
} dec;

static struct HexOut {
} hex;

static struct Hex0xOut {
} hex0x;

struct SymBaseF {
  SymBaseF(const SymbolEntry &sym) : Symbol(sym) {}
  const SymbolEntry &Symbol;
};

static SymbolEntry dummySymbolEntry(0, "", SymbolEntry::AliasesTy(), 0, 0, 0);

struct BBNameF : public SymBaseF {
  BBNameF(const SymbolEntry &sym) : SymBaseF(sym) {}
  BBNameF(const SymbolEntry *sym) : SymBaseF(*sym) {}
};

struct SymNameF : public SymBaseF {
  SymNameF(const SymbolEntry &sym) : SymBaseF(sym), name("") {}
  SymNameF(const SymbolEntry *sym) : SymBaseF(*sym), name("") {}
  // Use explicit to prevent StringRef being casted to SymNameF.
  explicit SymNameF(const StringRef N) : SymBaseF(dummySymbolEntry), name(N) {}

  StringRef name;
};

struct SymOrdinalF : public SymBaseF {
  SymOrdinalF(const SymbolEntry &sym) : SymBaseF(sym) {}
  SymOrdinalF(const SymbolEntry *sym) : SymBaseF(*sym) {}
};

struct SymSizeF : public SymBaseF {
  SymSizeF(const SymbolEntry &sym) : SymBaseF(sym) {}
};

struct SymShortF : public SymBaseF {
  SymShortF(const SymbolEntry &sym) : SymBaseF(sym) {}
  SymShortF(const SymbolEntry *sym) : SymBaseF(*sym) {}
};

struct countF {
  countF(uint64_t c) : cnt(c){};
  uint64_t cnt;
};

struct CommaF {
  CommaF(uint64_t v) : value(v) {}
  uint64_t value;
};

struct PercentageF {
  PercentageF(double d) : value(d) {}
  PercentageF(uint64_t a, uint64_t b) : value((double)a / (double)b) {}
  template <class Clazz>
  PercentageF(Clazz &c, uint64_t b) : value((double)c.size() / (double)b) {}
  template <class Clazz, class D>
  PercentageF(Clazz &c, D &d) : value((double)c.size() / (double)d.size()) {}
  double value;
};

struct BuildIdWrapper {
  BuildIdWrapper(const quipper::PerfDataProto_PerfBuildID &buildId)
      : data(buildId.build_id_hash().c_str()) {}

  BuildIdWrapper(const char *p) : data(p) {}

  const char *data;
};

static std::ostream &operator<<(std::ostream &out, const struct DecOut &) {
  return out << std::dec << std::noshowbase;
}

static std::ostream &operator<<(std::ostream &out, const struct HexOut &) {
  return out << std::hex << std::noshowbase;
}

static std::ostream &operator<<(std::ostream &out, const struct Hex0xOut &) {
  return out << std::hex << std::showbase;
}

static std::ostream &operator<<(std::ostream &out, const SymNameF &nameF) {
  if (!nameF.name.empty()) {
    // In this case, we only care about name field.
    // name is the form of "aaaa.BB.funcname" or "funcname".
    auto r = nameF.name.split(llvm::propeller::BASIC_BLOCK_SEPARATOR);
    if (r.second.empty())
      out << r.first.str();
    else
      out << ::dec << r.first.size() << llvm::propeller::BASIC_BLOCK_SEPARATOR
          << r.second.str();
    return out;
  }
  auto &sym = nameF.Symbol;
  if (sym.bbTag) {
    out << ::dec << sym.name.size() << llvm::propeller::BASIC_BLOCK_SEPARATOR
        << (sym.containingFunc ? sym.containingFunc->name.str() : "null_func");
  } else {
    out << sym.name.str().c_str();
    for (auto a : sym.aliases) out << "/" << a.str();
  }
  return out;
}

static std::ostream &operator<<(std::ostream &out, const BBNameF &nameF) {
  if (nameF.Symbol.containingFunc == &nameF.Symbol) return out << "entry";
  StringRef n = nameF.Symbol.name;
  auto t = n.find_first_of(".BB.");
  if (t != llvm::StringLiteral::npos)
    return out << t;
  return out << n.size();
}

static std::ostream &operator<<(std::ostream &out,
                                const SymOrdinalF &ordinalF) {
  return out << ::dec << ordinalF.Symbol.ordinal;
}

static std::ostream &operator<<(std::ostream &out, const SymSizeF &sizeF) {
  return out << ::dec << sizeF.Symbol.size;
}

static std::ostream &operator<<(std::ostream &out, const SymShortF &symSF) {
  return out << "symbol '" << SymNameF(symSF.Symbol) << "@" << hex0x
             << symSF.Symbol.addr << "'";
}

static std::ostream &operator<<(std::ostream &out, const countF &countF) {
  return out << ::dec << countF.cnt;
}

static std::ostream &operator<<(std::ostream &os, const MMapEntry &me) {
  return os << "[" << hex0x << me.loadAddr << ", " << hex0x << me.getEndAddr()
            << "] (PgOff=" << hex0x << me.pageOffset << ", size=" << hex0x
            << me.loadSize << ")";
};

static std::ostream &operator<<(std::ostream &out, const BuildIdWrapper &bw) {
  for (int i = 0; i < quipper::kBuildIDArraySize; ++i) {
    out << std::setw(2) << std::setfill('0') << std::hex
        << ((int)(bw.data[i]) & 0xFF);
  }
  return out;
}

// Output integer numbers in "," separated format.
static std::ostream &operator<<(std::ostream &out, const CommaF &cf) {
  std::list<int> seg;
  uint64_t value = cf.value;
  while (value) {
    seg.insert(seg.begin(), value % 1000);
    value /= 1000;
  }
  if (seg.empty()) seg.insert(seg.begin(), 0);
  auto of = out.fill();
  auto ow = out.width();
  auto i = seg.begin();
  out << std::setfill('\0') << *i;
  for (++i; i != seg.end(); ++i)
    out << "," << std::setw(3) << std::setfill('0') << ::dec << *i;
  out.fill(of);
  out.width(ow);
  return out;
}

static std::ostream &operator<<(std::ostream &out, const PercentageF &pf) {
  out << std::setprecision(3);
  out << (pf.value * 100) << '%';
  return out;
}

static std::ostream &operator << (std::ostream &out, const Path &p) {
  out << "[nodes=" << ::dec << p.syms.size() << ", weight=" << ::dec << p.weight
      << "]: ";
  auto e = p.cnts.begin();
  auto symi = p.syms.begin();
  auto syme = p.syms.end();
  auto lasti = std::prev(syme);
  while (symi != syme) {
    const auto *s = *symi;
    out << BBNameF(s) << "(" << *(e++) << ")";
    if (symi != lasti) out << " -> ";
    ++symi;
  }
  return out;
}

#endif
