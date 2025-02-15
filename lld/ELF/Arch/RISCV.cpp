//===- RISCV.cpp ----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Cheri.h"
#include "InputFiles.h"
#include "OutputSections.h"
#include "Relocations.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/TimeProfiler.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;

namespace {

class RISCV final : public TargetInfo {
public:
  RISCV();
  uint32_t calcEFlags() const override;
  bool calcIsCheriAbi() const override;
  int getCapabilitySize() const override;
  int64_t getImplicitAddend(const uint8_t *buf, RelType type) const override;
  void writeGotHeader(uint8_t *buf) const override;
  void writeGotPlt(uint8_t *buf, const Symbol &s) const override;
  void writeIgotPlt(uint8_t *buf, const Symbol &s) const override;
  void writePltHeader(uint8_t *buf) const override;
  void writePlt(uint8_t *buf, const Symbol &sym,
                uint64_t pltEntryAddr) const override;
  RelType getDynRel(RelType type) const override;
  RelExpr getRelExpr(RelType type, const Symbol &s,
                     const uint8_t *loc) const override;
  void relocate(uint8_t *loc, const Relocation &rel,
                uint64_t val) const override;
  bool relaxOnce(int pass) const override;
  uint64_t cheriRequiredAlignment(uint64_t) const override;
};

} // end anonymous namespace

const uint64_t dtpOffset = 0x800;

enum Op {
  ADDI = 0x13,
  AUIPC = 0x17,
  JALR = 0x67,
  LD = 0x3003,
  LW = 0x2003,
  SRLI = 0x5013,
  SUB = 0x40000033,

  CIncOffsetImm = 0x105b,
  CLC_64 = 0x3003,
  CLC_128 = 0x200f,
  CSub = 0x2800005b,

  AUIPCC = 0x17,
  AUICGP = 0x7b,
};

enum Reg {
  X_RA = 1,
  X_T0 = 5,
  X_T1 = 6,
  X_T2 = 7,
  X_T3 = 28,
};

static uint32_t hi20(uint32_t val) { return (val + 0x800) >> 12; }
static uint32_t lo12(uint32_t val) { return val & 4095; }

static uint32_t itype(uint32_t op, uint32_t rd, uint32_t rs1, uint32_t imm) {
  return op | (rd << 7) | (rs1 << 15) | (imm << 20);
}
static uint32_t rtype(uint32_t op, uint32_t rd, uint32_t rs1, uint32_t rs2) {
  return op | (rd << 7) | (rs1 << 15) | (rs2 << 20);
}
static uint32_t utype(uint32_t op, uint32_t rd, uint32_t imm) {
  return op | (rd << 7) | (imm << 12);
}

RISCV::RISCV() {
  copyRel = R_RISCV_COPY;
  noneRel = R_RISCV_NONE;
  pltRel = R_RISCV_JUMP_SLOT;
  relativeRel = R_RISCV_RELATIVE;
  iRelativeRel = R_RISCV_IRELATIVE;
  sizeRel = R_RISCV_CHERI_SIZE;
  cheriCapRel = R_RISCV_CHERI_CAPABILITY;
  // TODO: R_RISCV_CHERI_JUMP_SLOT in a separate .got.plt / .captable.plt
  cheriCapCallRel = R_RISCV_CHERI_CAPABILITY;
  if (config->is64) {
    symbolicRel = R_RISCV_64;
    tlsModuleIndexRel = R_RISCV_TLS_DTPMOD64;
    tlsOffsetRel = R_RISCV_TLS_DTPREL64;
    tlsGotRel = R_RISCV_TLS_TPREL64;
  } else {
    symbolicRel = R_RISCV_32;
    tlsModuleIndexRel = R_RISCV_TLS_DTPMOD32;
    tlsOffsetRel = R_RISCV_TLS_DTPREL32;
    tlsGotRel = R_RISCV_TLS_TPREL32;
  }
  gotRel = symbolicRel;
  absPointerRel = symbolicRel;

  // .got[0] = _DYNAMIC
  gotBaseSymInGotPlt = false;
  gotHeaderEntriesNum = 1;

  // .got.plt[0] = _dl_runtime_resolve, .got.plt[1] = link_map
  gotPltHeaderEntriesNum = 2;

  pltHeaderSize = 32;
  pltEntrySize = 16;
  ipltEntrySize = 16;
}

static uint32_t getEFlags(InputFile *f) {
  if (config->is64)
    return cast<ObjFile<ELF64LE>>(f)->getObj().getHeader().e_flags;
  return cast<ObjFile<ELF32LE>>(f)->getObj().getHeader().e_flags;
}

int RISCV::getCapabilitySize() const {
  return config->is64 ? 16 : 8;
}

uint32_t RISCV::calcEFlags() const {
  // If there are only binary input files (from -b binary), use a
  // value of 0 for the ELF header flags.
  if (objectFiles.empty())
    return 0;

  uint32_t target = getEFlags(objectFiles.front());

  for (InputFile *f : objectFiles) {
    uint32_t eflags = getEFlags(f);
    if (eflags & EF_RISCV_RVC)
      target |= EF_RISCV_RVC;

    if ((eflags & EF_RISCV_FLOAT_ABI) != (target & EF_RISCV_FLOAT_ABI))
      error(toString(f) +
            ": cannot link object files with different floating-point ABI");

    if ((eflags & EF_RISCV_RVE) != (target & EF_RISCV_RVE))
      error(toString(f) +
            ": cannot link object files with different EF_RISCV_RVE");

    if ((eflags & EF_RISCV_CHERIABI) != (target & EF_RISCV_CHERIABI))
      error(toString(f) +
            ": cannot link object files with different EF_RISCV_CHERIABI");

    if ((eflags & EF_RISCV_CAP_MODE) != (target & EF_RISCV_CAP_MODE))
      error(toString(f) +
            ": cannot link object files with different EF_RISCV_CAP_MODE");
  }

  return target;
}

bool RISCV::calcIsCheriAbi() const {
  bool isCheriAbi = config->eflags & EF_RISCV_CHERIABI;

  if (config->isCheriAbi && !objectFiles.empty() && !isCheriAbi)
    error(toString(objectFiles.front()) +
          ": object file is non-CheriABI but emulation forces it");

  return isCheriAbi;
}

int64_t RISCV::getImplicitAddend(const uint8_t *buf, RelType type) const {
  switch (type) {
  default:
    internalLinkerError(getErrorLocation(buf),
                        "cannot read addend for relocation " + toString(type));
    return 0;
  case R_RISCV_32:
  case R_RISCV_TLS_DTPMOD32:
  case R_RISCV_TLS_DTPREL32:
    return SignExtend64<32>(read32le(buf));
  case R_RISCV_64:
    return read64le(buf);
  case R_RISCV_RELATIVE:
  case R_RISCV_IRELATIVE:
    return config->is64 ? read64le(buf) : read32le(buf);
  case R_RISCV_NONE:
  case R_RISCV_JUMP_SLOT:
    // These relocations are defined as not having an implicit addend.
    return 0;
  }
}

void RISCV::writeGotHeader(uint8_t *buf) const {
  if (config->is64)
    write64le(buf, mainPart->dynamic->getVA());
  else
    write32le(buf, mainPart->dynamic->getVA());
}

void RISCV::writeGotPlt(uint8_t *buf, const Symbol &s) const {
  if (config->is64)
    write64le(buf, in.plt->getVA());
  else
    write32le(buf, in.plt->getVA());
}

void RISCV::writeIgotPlt(uint8_t *buf, const Symbol &s) const {
  if (config->writeAddends) {
    if (config->is64)
      write64le(buf, s.getVA());
    else
      write32le(buf, s.getVA());
  }
}

void RISCV::writePltHeader(uint8_t *buf) const {
  // TODO: Remove once we have a CHERI .got.plt and R_RISCV_CHERI_JUMP_SLOT.
  // Without those there can be no lazy binding support (though the former
  // requirement can be relaxed provided .captable[0] is _dl_runtime_resolve,
  // at least when the PLT is non-empty), so for now we emit a header full of
  // trapping instructions to ensure we don't accidentally end up trying to use
  // it. Ideally we would have a header size of 0, but isCheriAbi isn't known
  // in the constructor.
  if (config->isCheriAbi) {
    memset(buf, 0, pltHeaderSize);
    return;
  }
  // 1: auipc(c) (c)t2, %pcrel_hi(.got.plt)
  // (c)sub t1, (c)t1, (c)t3
  // l[wdc] (c)t3, %pcrel_lo(1b)((c)t2); (c)t3 = _dl_runtime_resolve
  // addi t1, t1, -pltHeaderSize-12; t1 = &.plt[i] - &.plt[0]
  // addi/cincoffset (c)t0, (c)t2, %pcrel_lo(1b)
  // (if shift != 0): srli t1, t1, shift; t1 = &.got.plt[i] - &.got.plt[0]
  // l[wdc] (c)t0, Ptrsize((c)t0); (c)t0 = link_map
  // (c)jr (c)t3
  // (if shift == 0): nop
  uint32_t offset = in.gotPlt->getVA() - in.plt->getVA();
  uint32_t ptrsub = config->isCheriAbi ? CSub : SUB;
  uint32_t ptrload = config->isCheriAbi ? config->is64 ? CLC_128 : CLC_64
                                        : config->is64 ? LD : LW;
  uint32_t ptraddi = config->isCheriAbi ? CIncOffsetImm : ADDI;
  // Shift is log2(pltsize / ptrsize), which is 0 for CHERI-128 so skipped
  uint32_t shift = 2 - config->is64 - config->isCheriAbi;
  uint32_t ptrsize = config->isCheriAbi ? config->capabilitySize
                                        : config->wordsize;
  write32le(buf + 0, utype(AUIPC, X_T2, hi20(offset)));
  write32le(buf + 4, rtype(ptrsub, X_T1, X_T1, X_T3));
  write32le(buf + 8, itype(ptrload, X_T3, X_T2, lo12(offset)));
  write32le(buf + 12, itype(ADDI, X_T1, X_T1, -target->pltHeaderSize - 12));
  write32le(buf + 16, itype(ptraddi, X_T0, X_T2, lo12(offset)));
  if (shift != 0)
    write32le(buf + 20, itype(SRLI, X_T1, X_T1, shift));
  write32le(buf + 24 - 4 * (shift == 0), itype(ptrload, X_T0, X_T0, ptrsize));
  write32le(buf + 28 - 4 * (shift == 0), itype(JALR, 0, X_T3, 0));
  if (shift == 0)
    write32le(buf + 28, itype(ADDI, 0, 0, 0));
}

void RISCV::writePlt(uint8_t *buf, const Symbol &sym,
                     uint64_t pltEntryAddr) const {
  // 1: auipc(c) (c)t3, %pcrel_hi(f@[.got.plt|.captable])
  // l[wdc] (c)t3, %pcrel_lo(1b)((c)t3)
  // (c)jalr (c)t1, (c)t3
  // nop
  uint32_t ptrload = config->isCheriAbi ? config->is64 ? CLC_128 : CLC_64
                                        : config->is64 ? LD : LW;
  uint32_t entryva = config->isCheriAbi ? sym.getCapTableVA(in.plt, 0)
                                        : sym.getGotPltVA();
  uint32_t offset = entryva - pltEntryAddr;
  write32le(buf + 0, utype(AUIPC, X_T3, hi20(offset)));
  write32le(buf + 4, itype(ptrload, X_T3, X_T3, lo12(offset)));
  write32le(buf + 8, itype(JALR, X_T1, X_T3, 0));
  write32le(buf + 12, itype(ADDI, 0, 0, 0));
}

RelType RISCV::getDynRel(RelType type) const {
  return type == target->symbolicRel ? type
                                     : static_cast<RelType>(R_RISCV_NONE);
}

RelExpr RISCV::getRelExpr(const RelType type, const Symbol &s,
                          const uint8_t *loc) const {
  switch (type) {
  case R_RISCV_NONE:
    return R_NONE;
  case R_RISCV_32:
  case R_RISCV_64:
  case R_RISCV_HI20:
  case R_RISCV_LO12_I:
  case R_RISCV_LO12_S:
  case R_RISCV_RVC_LUI:
    return R_ABS;
  case R_RISCV_ADD8:
  case R_RISCV_ADD16:
  case R_RISCV_ADD32:
  case R_RISCV_ADD64:
  case R_RISCV_SET6:
  case R_RISCV_SET8:
  case R_RISCV_SET16:
  case R_RISCV_SET32:
  case R_RISCV_SUB6:
  case R_RISCV_SUB8:
  case R_RISCV_SUB16:
  case R_RISCV_SUB32:
  case R_RISCV_SUB64:
    return R_RISCV_ADD;
  case R_RISCV_JAL:
  case R_RISCV_CHERI_CJAL:
  case R_RISCV_BRANCH:
  case R_RISCV_PCREL_HI20:
  case R_RISCV_RVC_BRANCH:
  case R_RISCV_RVC_JUMP:
  case R_RISCV_CHERI_RVC_CJUMP:
  case R_RISCV_32_PCREL:
    return R_PC;
  case R_RISCV_CALL:
  case R_RISCV_CALL_PLT:
  case R_RISCV_CHERI_CCALL:
    return R_PLT_PC;
  case R_RISCV_GOT_HI20:
    return R_GOT_PC;
  case R_RISCV_PCREL_LO12_I:
  case R_RISCV_PCREL_LO12_S:
    return R_RISCV_PC_INDIRECT;
  case R_RISCV_TLS_GD_HI20:
    return R_TLSGD_PC;
  case R_RISCV_TLS_GOT_HI20:
    config->hasStaticTlsModel = true;
    return R_GOT_PC;
  case R_RISCV_TPREL_HI20:
  case R_RISCV_TPREL_LO12_I:
  case R_RISCV_TPREL_LO12_S:
    return R_TPREL;
  case R_RISCV_TPREL_ADD:
  case R_RISCV_CHERI_TPREL_CINCOFFSET:
    return R_NONE;
  case R_RISCV_ALIGN:
    return R_RELAX_HINT;
  case R_RISCV_CHERI_CAPABILITY:
    return R_CHERI_CAPABILITY;
  case R_RISCV_CHERI_CAPTAB_PCREL_HI20:
    return R_CHERI_CAPABILITY_TABLE_ENTRY_PC;
  case R_RISCV_CHERI_TLS_IE_CAPTAB_PCREL_HI20:
    return R_CHERI_CAPABILITY_TABLE_TLSIE_ENTRY_PC;
  case R_RISCV_CHERI_TLS_GD_CAPTAB_PCREL_HI20:
    return R_CHERI_CAPABILITY_TABLE_TLSGD_ENTRY_PC;
  case R_RISCV_CHERIOT_COMPARTMENT_HI:
    return isPCCRelative(loc, &s) ? R_PC : R_CHERIOT_COMPARTMENT_CGPREL_HI;
  case R_RISCV_CHERIOT_COMPARTMENT_LO_I:
    return R_CHERIOT_COMPARTMENT_CGPREL_LO_I;
  case R_RISCV_CHERIOT_COMPARTMENT_LO_S:
    return R_CHERIOT_COMPARTMENT_CGPREL_LO_S;
  case R_RISCV_CHERIOT_COMPARTMENT_SIZE:
    return R_CHERIOT_COMPARTMENT_SIZE;
  case R_RISCV_RELAX:
    return config->relax ? R_RELAX_HINT : R_NONE;
  default:
    error(getErrorLocation(loc) + "unknown relocation (" + Twine(type) +
          ") against symbol " + toString(s));
    return R_NONE;
  }
}

// Extract bits V[Begin:End], where range is inclusive, and Begin must be < 63.
static uint32_t extractBits(uint64_t v, uint32_t begin, uint32_t end) {
  return (v & ((1ULL << (begin + 1)) - 1)) >> end;
}

void RISCV::relocate(uint8_t *loc, const Relocation &rel, uint64_t val) const {
  const unsigned bits = config->wordsize * 8;

  switch (rel.type) {
  case R_RISCV_32:
    write32le(loc, val);
    return;
  case R_RISCV_64:
    write64le(loc, val);
    return;

  case R_RISCV_RVC_BRANCH: {
    checkInt(loc, static_cast<int64_t>(val) >> 1, 8, rel);
    checkAlignment(loc, val, 2, rel);
    uint16_t insn = read16le(loc) & 0xE383;
    uint16_t imm8 = extractBits(val, 8, 8) << 12;
    uint16_t imm4_3 = extractBits(val, 4, 3) << 10;
    uint16_t imm7_6 = extractBits(val, 7, 6) << 5;
    uint16_t imm2_1 = extractBits(val, 2, 1) << 3;
    uint16_t imm5 = extractBits(val, 5, 5) << 2;
    insn |= imm8 | imm4_3 | imm7_6 | imm2_1 | imm5;

    write16le(loc, insn);
    return;
  }

  case R_RISCV_RVC_JUMP:
  case R_RISCV_CHERI_RVC_CJUMP: {
    checkInt(loc, static_cast<int64_t>(val) >> 1, 11, rel);
    checkAlignment(loc, val, 2, rel);
    uint16_t insn = read16le(loc) & 0xE003;
    uint16_t imm11 = extractBits(val, 11, 11) << 12;
    uint16_t imm4 = extractBits(val, 4, 4) << 11;
    uint16_t imm9_8 = extractBits(val, 9, 8) << 9;
    uint16_t imm10 = extractBits(val, 10, 10) << 8;
    uint16_t imm6 = extractBits(val, 6, 6) << 7;
    uint16_t imm7 = extractBits(val, 7, 7) << 6;
    uint16_t imm3_1 = extractBits(val, 3, 1) << 3;
    uint16_t imm5 = extractBits(val, 5, 5) << 2;
    insn |= imm11 | imm4 | imm9_8 | imm10 | imm6 | imm7 | imm3_1 | imm5;

    write16le(loc, insn);
    return;
  }

  case R_RISCV_RVC_LUI: {
    int64_t imm = SignExtend64(val + 0x800, bits) >> 12;
    checkInt(loc, imm, 6, rel);
    if (imm == 0) { // `c.lui rd, 0` is illegal, convert to `c.li rd, 0`
      write16le(loc, (read16le(loc) & 0x0F83) | 0x4000);
    } else {
      uint16_t imm17 = extractBits(val + 0x800, 17, 17) << 12;
      uint16_t imm16_12 = extractBits(val + 0x800, 16, 12) << 2;
      write16le(loc, (read16le(loc) & 0xEF83) | imm17 | imm16_12);
    }
    return;
  }

  case R_RISCV_JAL:
  case R_RISCV_CHERI_CJAL: {
    checkInt(loc, static_cast<int64_t>(val) >> 1, 20, rel);
    checkAlignment(loc, val, 2, rel);

    uint32_t insn = read32le(loc) & 0xFFF;
    uint32_t imm20 = extractBits(val, 20, 20) << 31;
    uint32_t imm10_1 = extractBits(val, 10, 1) << 21;
    uint32_t imm11 = extractBits(val, 11, 11) << 20;
    uint32_t imm19_12 = extractBits(val, 19, 12) << 12;
    insn |= imm20 | imm10_1 | imm11 | imm19_12;

    write32le(loc, insn);
    return;
  }

  case R_RISCV_BRANCH: {
    checkInt(loc, static_cast<int64_t>(val) >> 1, 12, rel);
    checkAlignment(loc, val, 2, rel);

    uint32_t insn = read32le(loc) & 0x1FFF07F;
    uint32_t imm12 = extractBits(val, 12, 12) << 31;
    uint32_t imm10_5 = extractBits(val, 10, 5) << 25;
    uint32_t imm4_1 = extractBits(val, 4, 1) << 8;
    uint32_t imm11 = extractBits(val, 11, 11) << 7;
    insn |= imm12 | imm10_5 | imm4_1 | imm11;

    write32le(loc, insn);
    return;
  }

  // auipc[c] + [c]jalr pair
  case R_RISCV_CALL:
  case R_RISCV_CALL_PLT:
  case R_RISCV_CHERI_CCALL: {
    int64_t hi = SignExtend64(val + 0x800, bits) >> 12;
    checkInt(loc, hi, 20, rel);
    if (isInt<20>(hi)) {
      relocateNoSym(loc, R_RISCV_PCREL_HI20, val);
      relocateNoSym(loc + 4, R_RISCV_PCREL_LO12_I, val);
    }
    return;
  }

  case R_RISCV_CHERI_CAPTAB_PCREL_HI20:
  case R_RISCV_CHERI_TLS_IE_CAPTAB_PCREL_HI20:
  case R_RISCV_CHERI_TLS_GD_CAPTAB_PCREL_HI20:
  case R_RISCV_GOT_HI20:
  case R_RISCV_PCREL_HI20:
  case R_RISCV_TLS_GD_HI20:
  case R_RISCV_TLS_GOT_HI20:
  case R_RISCV_TPREL_HI20:
  case R_RISCV_HI20: {
    uint64_t hi = val + 0x800;
    checkInt(loc, SignExtend64(hi, bits) >> 12, 20, rel);
    write32le(loc, (read32le(loc) & 0xFFF) | (hi & 0xFFFFF000));
    return;
  }

  case R_RISCV_PCREL_LO12_I:
  case R_RISCV_TPREL_LO12_I:
  case R_RISCV_LO12_I: {
    uint64_t hi = (val + 0x800) >> 12;
    uint64_t lo = val - (hi << 12);
    write32le(loc, (read32le(loc) & 0xFFFFF) | ((lo & 0xFFF) << 20));
    return;
  }

  case R_RISCV_PCREL_LO12_S:
  case R_RISCV_TPREL_LO12_S:
  case R_RISCV_LO12_S: {
    uint64_t hi = (val + 0x800) >> 12;
    uint64_t lo = val - (hi << 12);
    uint32_t imm11_5 = extractBits(lo, 11, 5) << 25;
    uint32_t imm4_0 = extractBits(lo, 4, 0) << 7;
    write32le(loc, (read32le(loc) & 0x1FFF07F) | imm11_5 | imm4_0);
    return;
  }

  case R_RISCV_ADD8:
    *loc += val;
    return;
  case R_RISCV_ADD16:
    write16le(loc, read16le(loc) + val);
    return;
  case R_RISCV_ADD32:
    write32le(loc, read32le(loc) + val);
    return;
  case R_RISCV_ADD64:
    write64le(loc, read64le(loc) + val);
    return;
  case R_RISCV_SUB6:
    *loc = (*loc & 0xc0) | (((*loc & 0x3f) - val) & 0x3f);
    return;
  case R_RISCV_SUB8:
    *loc -= val;
    return;
  case R_RISCV_SUB16:
    write16le(loc, read16le(loc) - val);
    return;
  case R_RISCV_SUB32:
    write32le(loc, read32le(loc) - val);
    return;
  case R_RISCV_SUB64:
    write64le(loc, read64le(loc) - val);
    return;
  case R_RISCV_SET6:
    *loc = (*loc & 0xc0) | (val & 0x3f);
    return;
  case R_RISCV_SET8:
    *loc = val;
    return;
  case R_RISCV_SET16:
    write16le(loc, val);
    return;
  case R_RISCV_SET32:
  case R_RISCV_32_PCREL:
    write32le(loc, val);
    return;

  case R_RISCV_TLS_DTPREL32:
    if (config->isCheriAbi)
      write32le(loc, val);
    else
      write32le(loc, val - dtpOffset);
    break;
  case R_RISCV_TLS_DTPREL64:
    if (config->isCheriAbi)
      write64le(loc, val);
    else
      write64le(loc, val - dtpOffset);
    break;

  case R_RISCV_RELAX:
    return; // Ignored (for now)

  case R_RISCV_CHERIOT_COMPARTMENT_LO_I: {
    if (isPCCRelative(loc, rel.sym)) {
      // Attach a negative sign bit to LO12 if the offset is negative.
      // However, if HI20 alone is enough to reach the target, then this should
      // not be done and LO14 should just be 0 regardless.
      if (int64_t(val) >= 0 || (val & 0x7ff) == 0)
        val &= 0x7ff;
      else
        val = (uint64_t(-1) & ~0x7ff) | (val & 0x7ff);
    }
    checkInt(loc, val, 12, rel);
    write32le(loc, (read32le(loc) & 0x000fffff) | (val << 20));
    break;
  }
  case R_RISCV_CHERIOT_COMPARTMENT_SIZE:
    checkUInt(loc, val, 12, rel);
    write32le(loc, (read32le(loc) & 0x000fffff) | (val << 20));
    break;
  case R_RISCV_CHERIOT_COMPARTMENT_LO_S: {
    // Stores have their immediate fields split because RISC-V prematurely
    // optimises for small pipelines with no FPU.
    uint32_t insn = read32le(loc) & 0x1fff07f;
    uint32_t val_high = val & 0xfe0;
    uint32_t val_low = val & 0x1f;
    write32le(loc, insn | (val_high << 20) | (val_low << 7));
    break;
  }
  case R_RISCV_CHERIOT_COMPARTMENT_HI: {
    // AUICGP
    uint32_t opcode = AUICGP;
    if (isPCCRelative(loc, rel.sym)) {
      opcode = AUIPCC;
      if (int64_t(val) < 0)
        val = (val + 0x7ff) & ~0x7ff;
      val = int64_t(val) >> 11;
    }
    uint32_t existingOpcode = read32le(loc) & 0x7f;
    if ((existingOpcode != AUIPCC) && (existingOpcode != AUICGP))
      warn("R_RISCV_CHERIOT_COMPARTMENT_HI relocation applied to instruction "
           "with unexpected opcode " +
           Twine(existingOpcode));
    checkInt(loc, val, 20, rel);
    // Preserve the target register.  We will rewrite the opcode (source
    // register) to either AUICGP or AUIPCC and set the immediate field.
    uint32_t insn = read32le(loc) & 0x00000f80;
    write32le(loc, insn | (val << 12) | opcode);
    break;
  }

  default:
    llvm_unreachable("unknown relocation");
  }
}

uint64_t RISCV::cheriRequiredAlignment(uint64_t size) const {
  // FIXME: Non-CherIoT targets will have different calculations here
  uint64_t mantissaWidth = 9;
  auto mantissaWidthMinusOneMask = (uint64_t(1) << (mantissaWidth - 1)) - 1;
  uint64_t msbIdxPlusOne = 64 - countLeadingZeros(size);
  uint64_t e = std::max<int64_t>(int64_t(msbIdxPlusOne) - mantissaWidth, 0);
  // If we are very close to the top, then we need to round up one more
  if (((size >> (e + 1)) & mantissaWidthMinusOneMask) ==
      mantissaWidthMinusOneMask)
    ++e;
  return uint64_t(1) << e;
}

namespace {
struct SymbolAnchor {
  uint64_t offset;
  Defined *d;
  bool end; // true for the anchor of st_value+st_size
};
} // namespace

struct elf::RISCVRelaxAux {
  // This records symbol start and end offsets which will be adjusted according
  // to the nearest relocDeltas element.
  SmallVector<SymbolAnchor, 0> anchors;
  // For relocations[i], the actual offset is r_offset - (i ? relocDeltas[i-1] :
  // 0).
  std::unique_ptr<uint32_t[]> relocDeltas;
  // For relocations[i], the actual type is relocTypes[i].
  std::unique_ptr<RelType[]> relocTypes;
  SmallVector<uint32_t, 0> writes;
};

static void initSymbolAnchors() {
  SmallVector<InputSection *, 0> storage;
  for (OutputSection *osec : outputSections) {
    if (!(osec->flags & SHF_EXECINSTR))
      continue;
    for (InputSection *sec : getInputSections(osec)) {
      sec->relaxAux = make<RISCVRelaxAux>();
      if (sec->relocations.size()) {
        sec->relaxAux->relocDeltas =
            std::make_unique<uint32_t[]>(sec->relocations.size());
        sec->relaxAux->relocTypes =
            std::make_unique<RelType[]>(sec->relocations.size());
      }
    }
  }
  // Store anchors (st_value and st_value+st_size) for symbols relative to text
  // sections.
  for (InputFile *file : objectFiles)
    for (Symbol *sym : file->getSymbols()) {
      auto *d = dyn_cast<Defined>(sym);
      if (!d || d->file != file)
        continue;
      if (auto *sec = dyn_cast_or_null<InputSection>(d->section))
        if (sec->flags & SHF_EXECINSTR && sec->relaxAux) {
          // If sec is discarded, relaxAux will be nullptr.
          sec->relaxAux->anchors.push_back({d->value, d, false});
          sec->relaxAux->anchors.push_back({d->value + d->size, d, true});
        }
    }
  // Sort anchors by offset so that we can find the closest relocation
  // efficiently. For a zero size symbol, ensure that its start anchor precedes
  // its end anchor. For two symbols with anchors at the same offset, their
  // order does not matter.
  for (OutputSection *osec : outputSections) {
    if (!(osec->flags & SHF_EXECINSTR))
      continue;
    for (InputSection *sec : getInputSections(osec)) {
      llvm::sort(sec->relaxAux->anchors, [](auto &a, auto &b) {
        return std::make_pair(a.offset, a.end) <
               std::make_pair(b.offset, b.end);
      });
    }
  }
}

// Relax R_RISCV_CALL/R_RISCV_CALL_PLT auipc+jalr to c.j, c.jal, or jal.
// Relax R_RISCV_CHERI_CCALL auipcc+cjalr to c.cj, c.cjal, or cjal.
static void relaxCall(const InputSection &sec, size_t i, uint64_t loc,
                      Relocation &r, uint32_t &remove) {
  // We need to emit the correct relocations for CHERI, although the instruction
  // encodings are exactly the same with vanilla RISC-V.
  auto jalRVCType = (r.type == R_RISCV_CHERI_CCALL) ? R_RISCV_CHERI_RVC_CJUMP
                                                    : R_RISCV_RVC_JUMP;
  auto jalType = (r.type == R_RISCV_CHERI_CCALL) ? R_RISCV_CHERI_CJAL
                                                 : R_RISCV_JAL;
  const bool rvc = config->eflags & EF_RISCV_RVC;
  const Symbol &sym = *r.sym;
  const uint64_t insnPair = read64le(sec.rawData.data() + r.offset);
  const uint32_t rd = extractBits(insnPair, 32 + 11, 32 + 7);
  const uint64_t dest =
      (r.expr == R_PLT_PC ? sym.getPltVA() : sym.getVA()) + r.addend;
  const int64_t displace = dest - loc;

  if (rvc && isInt<12>(displace) && rd == 0) {
    sec.relaxAux->relocTypes[i] = jalRVCType;
    sec.relaxAux->writes.push_back(0xa001); // c.[c]j
    remove = 6;
  } else if (rvc && isInt<12>(displace) && rd == X_RA &&
             !config->is64) { // RV32C only
    sec.relaxAux->relocTypes[i] = jalRVCType;
    sec.relaxAux->writes.push_back(0x2001); // c.[c]jal
    remove = 6;
  } else if (isInt<21>(displace)) {
    sec.relaxAux->relocTypes[i] = jalType;
    sec.relaxAux->writes.push_back(0x6f | rd << 7); // [c]jal
    remove = 4;
  }
}

// Relax auicgp + cincoffset/memop to cincoffset/memop cgp
static void relaxCGP(const InputSection &sec, size_t i, uint64_t loc,
                     Relocation &r, uint32_t &remove) {
  if (isPCCRelative(nullptr, r.sym))
    return;
  uint64_t hival = getBiasedCGPOffset(*r.sym) - getBiasedCGPOffsetLo12(*r.sym);
  // We can only relax when imm == 0 in auicgp rd, imm.
  if (hival != 0)
    return;
  uint32_t insn = read32le(sec.rawData.data() + r.offset);
  switch (r.type) {
  case R_RISCV_CHERIOT_COMPARTMENT_HI: {
    // Remove auicgp rd, 0.
    sec.relaxAux->relocTypes[i] = R_RISCV_RELAX;
    remove = 4;
    break;
  }
  case R_RISCV_CHERIOT_COMPARTMENT_LO_I: {
    // cincoffset/load rd, cs1, %lo(x) => cincoffset/load rd, cgp, %lo(x)
    sec.relaxAux->relocTypes[i] = R_RISCV_CHERIOT_COMPARTMENT_LO_I;
    insn = (insn & ~(31 << 15)) | (3 << 15);
    sec.relaxAux->writes.push_back(insn);
    break;
  }
  case R_RISCV_CHERIOT_COMPARTMENT_LO_S:
    // store cs2, cs1, %lo(x) => store cs2, cgp, %lo(x)
    sec.relaxAux->relocTypes[i] = R_RISCV_CHERIOT_COMPARTMENT_LO_I;
    insn = (insn & ~(31 << 15)) | (3 << 15);
    sec.relaxAux->writes.push_back(insn);
    break;
  }
}

/**
 * Find all R_RISCV_CHERIOT_COMPARTMENT_LO_I relocations that are CGP-relative
 * and rewrite them to be relative to the target of the current relocation.
 * These relocations mirror the HI20/LO12 PC-relative relocations and are
 * written as pairs where the first has the real relocation target as its
 * symbol and the second has the location of the first as its target.  This is
 * necessary for PC-relative relocations because the final address depends on
 * the location of the first instruction.  For CHERIoT, both PCC and
 * CGP-relative relocations use the same relocation types and we don't know
 * whether it is relative to PCC or CGP until we know the target.  That would
 * be fine, except that relaxation can delete the AUICGP, which means that we
 * then can't find the target.  We void this by doing a pass to find these
 * relocation targets and attaching them to the
 * R_RISCV_CHERIOT_COMPARTMENT_LO_I relocations for the cases where the target
 * is CGP-relative.
 *
 * Note: If we ever get direct PC[C]-relative loads in RISC-V then other
 * relocations will want to reuse this path.
 */
static bool rewriteCheriotLowRelocs(InputSection &sec) {
  bool modified = false;
  for (auto it : llvm::enumerate(sec.relocations)) {
    Relocation &r = it.value();
    if (r.type == R_RISCV_CHERIOT_COMPARTMENT_LO_I) {
      // If this is PCC-relative, then the relocation points to the auicgp /
      // auipcc instruction and we need to look there to find the real target.
      if (isPCCRelative(nullptr, r.sym)) {
        const Defined *d = cast<Defined>(r.sym);
        if (!d->section)
          error("R_RISCV_CHERIOT_COMPARTMENT_LO_I relocation points to an "
                "absolute symbol: " +
                r.sym->getName());
        InputSection *isec = cast<InputSection>(d->section);

        // Relocations are sorted by offset, so we can use std::equal_range to
        // do binary search.
        Relocation targetReloc;
        targetReloc.offset = d->value;
        auto range = std::equal_range(
            isec->relocations.begin(), isec->relocations.end(), targetReloc,
            [](const Relocation &lhs, const Relocation &rhs) {
              return lhs.offset < rhs.offset;
            });

        const Relocation *target = nullptr;
        for (auto it = range.first; it != range.second; ++it)
          if (it->type == R_RISCV_CHERIOT_COMPARTMENT_HI) {
            target = &*it;
            break;
          }
        if (!target) {
          error(
              "Could not find R_RISCV_CHERIOT_COMPARTMENT_HI relocation for " +
              toString(*r.sym));
        }
        // If the target is PCC-relative then the auipcc can't be erased and so
        // skip the rewriting.
        if (isPCCRelative(nullptr, target->sym))
          continue;
        // Update our relocation to point to the target thing.
        r.sym = target->sym;
        r.addend = target->addend;
        modified = true;
      }
    }
  }
  return modified;
}

static bool relax(InputSection &sec, int pass) {
  const uint64_t secAddr = sec.getVA();
  auto &aux = *sec.relaxAux;
  bool changed = false;

  // On the first pass, do a scan of LO_I CHERIoT relocations
  if (pass == 0)
    changed |= rewriteCheriotLowRelocs(sec);

  // Get st_value delta for symbols relative to this section from the previous
  // iteration.
  DenseMap<const Defined *, uint64_t> valueDelta;
  ArrayRef<SymbolAnchor> sa = makeArrayRef(aux.anchors);
  uint32_t delta = 0;
  for (auto it : llvm::enumerate(sec.relocations)) {
    for (; sa.size() && sa[0].offset <= it.value().offset; sa = sa.slice(1))
      if (!sa[0].end)
        valueDelta[sa[0].d] = delta;
    delta = aux.relocDeltas[it.index()];
  }
  for (const SymbolAnchor &sa : sa)
    if (!sa.end)
      valueDelta[sa.d] = delta;
  sa = makeArrayRef(aux.anchors);
  delta = 0;

  std::fill_n(aux.relocTypes.get(), sec.relocations.size(), R_RISCV_NONE);
  aux.writes.clear();
  for (auto it : llvm::enumerate(sec.relocations)) {
    Relocation &r = it.value();
    const size_t i = it.index();
    const uint64_t loc = secAddr + r.offset - delta;
    uint32_t &cur = aux.relocDeltas[i], remove = 0;
    switch (r.type) {
    case R_RISCV_ALIGN: {
      const uint64_t nextLoc = loc + r.addend;
      const uint64_t align = PowerOf2Ceil(r.addend + 2);
      // All bytes beyond the alignment boundary should be removed.
      remove = nextLoc - ((loc + align - 1) & -align);
      assert(static_cast<int32_t>(remove) >= 0 &&
             "R_RISCV_ALIGN needs expanding the content");
      break;
    }
    case R_RISCV_CALL:
    case R_RISCV_CALL_PLT:
    case R_RISCV_CHERI_CCALL:
      if (i + 1 != sec.relocations.size() &&
          sec.relocations[i + 1].type == R_RISCV_RELAX)
        relaxCall(sec, i, loc, r, remove);
      break;
    case R_RISCV_CHERIOT_COMPARTMENT_HI:
    case R_RISCV_CHERIOT_COMPARTMENT_LO_I:
    case R_RISCV_CHERIOT_COMPARTMENT_LO_S:
      if (i + 1 != sec.relocations.size() &&
          sec.relocations[i + 1].type == R_RISCV_RELAX)
        relaxCGP(sec, i, loc, r, remove);
    }

    // For all anchors whose offsets are <= r.offset, they are preceded by
    // the previous relocation whose `relocDeltas` value equals `delta`.
    // Decrease their st_value and update their st_size.
    for (; sa.size() && sa[0].offset <= r.offset; sa = sa.slice(1)) {
      if (sa[0].end)
        sa[0].d->size = sa[0].offset - delta - sa[0].d->value;
      else
        sa[0].d->value -= delta - valueDelta.find(sa[0].d)->second;
    }
    delta += remove;
    if (delta != cur) {
      cur = delta;
      changed = true;
    }
  }

  for (const SymbolAnchor &a : sa) {
    if (a.end)
      a.d->size = a.offset - delta - a.d->value;
    else
      a.d->value -= delta - valueDelta.find(a.d)->second;
  }
  // Inform assignAddresses that the size has changed.
  if (!isUInt<16>(delta))
    fatal("section size decrease is too large");
  sec.bytesDropped = delta;
  return changed;
}

// When relaxing just R_RISCV_ALIGN, relocDeltas is usually changed only once in
// the absence of a linker script. For call and load/store R_RISCV_RELAX, code
// shrinkage may reduce displacement and make more relocations eligible for
// relaxation. Code shrinkage may increase displacement to a call/load/store
// target at a higher fixed address, invalidating an earlier relaxation. Any
// change in section sizes can have cascading effect and require another
// relaxation pass.
bool RISCV::relaxOnce(int pass) const {
  llvm::TimeTraceScope timeScope("RISC-V relaxOnce");
  if (config->relocatable)
    return false;

  if (pass == 0)
    initSymbolAnchors();

  SmallVector<InputSection *, 0> storage;
  bool changed = false;
  for (OutputSection *osec : outputSections) {
    if (!(osec->flags & SHF_EXECINSTR))
      continue;
    for (InputSection *sec : getInputSections(osec))
      changed |= relax(*sec, pass);
  }
  return changed;
}

void elf::riscvFinalizeRelax(int passes) {
  llvm::TimeTraceScope timeScope("Finalize RISC-V relaxation");
  log("relaxation passes: " + Twine(passes));
  SmallVector<InputSection *, 0> storage;
  for (OutputSection *osec : outputSections) {
    if (!(osec->flags & SHF_EXECINSTR))
      continue;
    for (InputSection *sec : getInputSections(osec)) {
      RISCVRelaxAux &aux = *sec->relaxAux;
      if (!aux.relocDeltas)
        continue;

      auto &rels = sec->relocations;
      ArrayRef<uint8_t> old = sec->rawData;
      size_t newSize =
          old.size() - aux.relocDeltas[sec->relocations.size() - 1];
      size_t writesIdx = 0;
      uint8_t *p = bAlloc.Allocate<uint8_t>(newSize);
      uint64_t offset = 0;
      int64_t delta = 0;
      sec->rawData = makeArrayRef(p, newSize);
      sec->bytesDropped = 0;

      // Update section content: remove NOPs for R_RISCV_ALIGN and rewrite
      // instructions for relaxed relocations.
      for (size_t i = 0, e = rels.size(); i != e; ++i) {
        uint32_t remove = aux.relocDeltas[i] - delta;
        delta = aux.relocDeltas[i];
        if (remove == 0 && aux.relocTypes[i] == R_RISCV_NONE)
          continue;

        // Copy from last location to the current relocated location.
        const Relocation &r = rels[i];
        uint64_t size = r.offset - offset;
        memcpy(p, old.data() + offset, size);
        p += size;

        // For R_RISCV_ALIGN, we will place `offset` in a location (among NOPs)
        // to satisfy the alignment requirement. If both `remove` and r.addend
        // are multiples of 4, it is as if we have skipped some NOPs. Otherwise
        // we are in the middle of a 4-byte NOP, and we need to rewrite the NOP
        // sequence.
        int64_t skip = 0;
        if (r.type == R_RISCV_ALIGN) {
          if (remove % 4 || r.addend % 4) {
            skip = r.addend - remove;
            int64_t j = 0;
            for (; j + 4 <= skip; j += 4)
              write32le(p + j, 0x00000013); // nop
            if (j != skip) {
              assert(j + 2 == skip);
              write16le(p + j, 0x0001); // c.nop
            }
          }
        } else if (RelType newType = aux.relocTypes[i]) {
          switch (newType) {
          case R_RISCV_RELAX:
            break;
          case R_RISCV_RVC_JUMP:
          case R_RISCV_CHERI_RVC_CJUMP:
            skip = 2;
            write16le(p, aux.writes[writesIdx++]);
            break;
          case R_RISCV_JAL:
          case R_RISCV_CHERI_CJAL:
            skip = 4;
            write32le(p, aux.writes[writesIdx++]);
            break;
          case R_RISCV_CHERIOT_COMPARTMENT_LO_I:
          case R_RISCV_CHERIOT_COMPARTMENT_LO_S:
            skip = 4;
            write32le(p, aux.writes[writesIdx++]);
            break;
          default:
            llvm_unreachable("unsupported type");
          }
        }

        p += skip;
        offset = r.offset + skip + remove;
      }
      memcpy(p, old.data() + offset, old.size() - offset);

      // Substract the previous relocDeltas value from the relocation offset.
      // For a pair of R_RISCV_CALL/R_RISCV_RELAX with the same offset, decrease
      // their r_offset by the same delta.
      delta = 0;
      for (size_t i = 0, e = rels.size(); i != e;) {
        uint64_t cur = rels[i].offset;
        do {
          rels[i].offset -= delta;
          if (aux.relocTypes[i] != R_RISCV_NONE)
            rels[i].type = aux.relocTypes[i];
        } while (++i != e && rels[i].offset == cur);
        delta = aux.relocDeltas[i - 1];
      }
    }
  }
}

TargetInfo *elf::getRISCVTargetInfo() {
  static RISCV target;
  return &target;
}
