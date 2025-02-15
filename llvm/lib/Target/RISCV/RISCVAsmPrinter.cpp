//===-- RISCVAsmPrinter.cpp - RISCV LLVM assembly writer ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to the RISCV assembly language.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/RISCVInstPrinter.h"
#include "MCTargetDesc/RISCVMCExpr.h"
#include "MCTargetDesc/RISCVTargetStreamer.h"
#include "RISCV.h"
#include "RISCVTargetMachine.h"
#include "TargetInfo/RISCVTargetInfo.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "asm-printer"

STATISTIC(RISCVNumInstrsCompressed,
          "Number of RISC-V Compressed instructions emitted");

namespace {
class RISCVAsmPrinter : public AsmPrinter {
  const MCSubtargetInfo *STI;

public:
  explicit RISCVAsmPrinter(TargetMachine &TM,
                           std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer)), STI(TM.getMCSubtargetInfo()) {}

  StringRef getPassName() const override { return "RISCV Assembly Printer"; }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void emitInstruction(const MachineInstr *MI) override;

  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                       const char *ExtraCode, raw_ostream &OS) override;
  bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                             const char *ExtraCode, raw_ostream &OS) override;

  void EmitToStreamer(MCStreamer &S, const MCInst &Inst);
  bool emitPseudoExpansionLowering(MCStreamer &OutStreamer,
                                   const MachineInstr *MI);

  // Wrapper needed for tblgenned pseudo lowering.
  bool lowerOperand(const MachineOperand &MO, MCOperand &MCOp) const {
    return LowerRISCVMachineOperandToMCOperand(MO, MCOp, *this);
  }

  void emitStartOfAsmFile(Module &M) override;
  void emitEndOfAsmFile(Module &M) override;

private:
  /**
   * Struct describing compartment exports that must be emitted for this
   * compilation unit.
   */
  struct CompartmentExport
  {
    /// The compartment name for the function.
    std::string CompartmentName;
    /// The IR function corresponding to the function.
    const Function &Fn;
    /// The symbol for the function
    MCSymbol *FnSym;
    /// The number of registers that are live on entry to this function
    int LiveIns;
    /// Emit this export as a local symbol even if the function is not local.
    bool forceLocal = false;
    /// The size in bytes of the stack frame, 0 if not used.
    uint32_t stackSize = 0;
  };
  SmallVector<CompartmentExport, 1> CompartmentEntries;
  void emitAttributes();
};
}

#define GEN_COMPRESS_INSTR
#include "RISCVGenCompressInstEmitter.inc"
void RISCVAsmPrinter::EmitToStreamer(MCStreamer &S, const MCInst &Inst) {
  MCInst CInst;
  bool Res = compressInst(CInst, Inst, *STI, OutStreamer->getContext());
  if (Res)
    ++RISCVNumInstrsCompressed;
  AsmPrinter::EmitToStreamer(*OutStreamer, Res ? CInst : Inst);
}

// Simple pseudo-instructions have their lowering (with expansion to real
// instructions) auto-generated.
#include "RISCVGenMCPseudoLowering.inc"

void RISCVAsmPrinter::emitInstruction(const MachineInstr *MI) {
  // Do any auto-generated pseudo lowerings.
  if (emitPseudoExpansionLowering(*OutStreamer, MI))
    return;

  MCInst TmpInst;
  if (!lowerRISCVMachineInstrToMCInst(MI, TmpInst, *this))
    EmitToStreamer(*OutStreamer, TmpInst);
}

bool RISCVAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                      const char *ExtraCode, raw_ostream &OS) {
  // First try the generic code, which knows about modifiers like 'c' and 'n'.
  if (!AsmPrinter::PrintAsmOperand(MI, OpNo, ExtraCode, OS))
    return false;

  const MachineOperand &MO = MI->getOperand(OpNo);
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1] != 0)
      return true; // Unknown modifier.

    switch (ExtraCode[0]) {
    default:
      return true; // Unknown modifier.
    case 'z':      // Print zero register if zero, regular printing otherwise.
      if (MO.isImm() && MO.getImm() == 0) {
        OS << RISCVInstPrinter::getRegisterName(RISCV::X0);
        return false;
      }
      break;
    case 'i': // Literal 'i' if operand is not a register.
      if (!MO.isReg())
        OS << 'i';
      return false;
    }
  }

  switch (MO.getType()) {
  case MachineOperand::MO_Immediate:
    OS << MO.getImm();
    return false;
  case MachineOperand::MO_Register:
    OS << RISCVInstPrinter::getRegisterName(MO.getReg());
    return false;
  case MachineOperand::MO_GlobalAddress:
    PrintSymbolOperand(MO, OS);
    return false;
  case MachineOperand::MO_BlockAddress: {
    MCSymbol *Sym = GetBlockAddressSymbol(MO.getBlockAddress());
    Sym->print(OS, MAI);
    return false;
  }
  default:
    break;
  }

  return true;
}

bool RISCVAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                            unsigned OpNo,
                                            const char *ExtraCode,
                                            raw_ostream &OS) {
  if (!ExtraCode) {
    const MachineOperand &MO = MI->getOperand(OpNo);
    // For now, we only support register memory operands in registers and
    // assume there is no addend
    if (!MO.isReg())
      return true;

    OS << "0(" << RISCVInstPrinter::getRegisterName(MO.getReg()) << ")";
    return false;
  }

  return AsmPrinter::PrintAsmMemoryOperand(MI, OpNo, ExtraCode, OS);
}

bool RISCVAsmPrinter::runOnMachineFunction(MachineFunction &MF) {
  // Set the current MCSubtargetInfo to a copy which has the correct
  // feature bits for the current MachineFunction
  MCSubtargetInfo &NewSTI =
    OutStreamer->getContext().getSubtargetCopy(*TM.getMCSubtargetInfo());
  NewSTI.setFeatureBits(MF.getSubtarget().getFeatureBits());
  STI = &NewSTI;

  SetupMachineFunction(MF);
  emitFunctionBody();
  auto &Fn = MF.getFunction();
  // The low 3 bits of the flags field specify the number of registers to
  // clear.  The next two provide the set of
  int interruptFlag = 0;
  if (Fn.hasFnAttribute("interrupt-state"))
    interruptFlag = StringSwitch<int>(
                        Fn.getFnAttribute("interrupt-state").getValueAsString())
                        .Case("enabled", 1 << 3)
                        .Case("disabled", 2 << 3)
                        .Default(0);

  // For the CHERI MCU ABI, find the highest used argument register.  The
  // switcher will zero all of the ones above this.
  auto countUsedArgRegisters = [](auto const &MF) -> int {
    static constexpr int ArgRegCount = 7;
    static const MCPhysReg ArgGPCRsE[ArgRegCount] = {
        RISCV::C10, RISCV::C11, RISCV::C12, RISCV::C13,
        RISCV::C14, RISCV::C15, RISCV::C5};
    auto LiveIns = MF.getRegInfo().liveins();
    auto *TRI = MF.getRegInfo().getTargetRegisterInfo();
    int NumArgRegs = 0;
    for (auto LI : LiveIns)
      for (int i = 0; i < ArgRegCount; i++)
        if ((ArgGPCRsE[i] == LI.first) ||
            TRI->isSubRegister(ArgGPCRsE[i], LI.first)) {
          NumArgRegs = std::max(NumArgRegs, i + 1);
          break;
        }
    return NumArgRegs;
  };

  if (Fn.getCallingConv() == CallingConv::CHERI_CCallee)
    // FIXME: Get stack size as function attribute if specified
    CompartmentEntries.push_back(
        {std::string(Fn.getFnAttribute("cheri-compartment").getValueAsString()),
         Fn, OutStreamer->getContext().getOrCreateSymbol(MF.getName()),
         countUsedArgRegisters(MF) + interruptFlag, false,
         static_cast<uint32_t>(MF.getFrameInfo().getStackSize())});
  else if (Fn.getCallingConv() == CallingConv::CHERI_LibCall)
    CompartmentEntries.push_back(
        {"libcalls", Fn,
         OutStreamer->getContext().getOrCreateSymbol(MF.getName()),
         countUsedArgRegisters(MF) + interruptFlag});
  else if (interruptFlag != 0)
    CompartmentEntries.push_back(
        {std::string(Fn.getFnAttribute("cheri-compartment").getValueAsString()),
         Fn, OutStreamer->getContext().getOrCreateSymbol(MF.getName()),
         countUsedArgRegisters(MF) + interruptFlag, true});

  return false;
}

void RISCVAsmPrinter::emitStartOfAsmFile(Module &M) {
  if (TM.getTargetTriple().isOSBinFormatELF())
    emitAttributes();
}

void RISCVAsmPrinter::emitEndOfAsmFile(Module &M) {
  RISCVTargetStreamer &RTS =
      static_cast<RISCVTargetStreamer &>(*OutStreamer->getTargetStreamer());

  if (!CompartmentEntries.empty()) {
    auto &C = OutStreamer->getContext();
    auto *Exports = C.getELFSection(".compartment_exports", ELF::SHT_PROGBITS,
                                    ELF::SHF_ALLOC | ELF::SHF_GNU_RETAIN);
    OutStreamer->SwitchSection(Exports);
    auto CompartmentStartSym = C.getOrCreateSymbol("__compartment_pcc_start");
    for (auto &Entry : CompartmentEntries) {
      std::string ExportName = getImportExportTableName(
          Entry.CompartmentName, Entry.Fn.getName(), Entry.Fn.getCallingConv(),
          /*IsImport*/ false);
      auto Sym = C.getOrCreateSymbol(ExportName);
      OutStreamer->emitSymbolAttribute(Sym, MCSA_ELF_TypeObject);
      // If the function isn't global, don't make its export table entry global
      // either.  Two different compilation units in the same compartment may
      // export different static things.
      if (Entry.Fn.hasExternalLinkage() && !Entry.forceLocal)
        OutStreamer->emitSymbolAttribute(Sym, MCSA_Global);
      OutStreamer->emitValueToAlignment(4);
      OutStreamer->emitLabel(Sym);
      emitLabelDifference(Entry.FnSym, CompartmentStartSym, 2);
      auto stackSize = Entry.stackSize;
      // Round up to multiple of 8 and divide by 8.
      stackSize = (stackSize + 7) / 8;
      // TODO: We should probably warn if the std::min truncates here.
      OutStreamer->emitIntValue(std::min(uint32_t(255), stackSize), 1);
      OutStreamer->emitIntValue(Entry.LiveIns, 1);
      OutStreamer->emitELFSize(Sym, MCConstantExpr::create(4, C));
    }
  }
  // Generate CHERIoT imports if there are any.
  auto &CHERIoTCompartmentImports =
      static_cast<RISCVTargetMachine &>(TM).ImportedFunctions;
  if (!CHERIoTCompartmentImports.empty()) {
    auto &C = OutStreamer->getContext();

    for (auto &Entry : CHERIoTCompartmentImports) {
      // Import entries are capability-sized entries.  The second word is
      // zero, the first is the address of the corresponding export table
      // entry.

      // Public symbols must be COMDATs so that they can be merged across
      // compilation units.  Private ones must not be.
      auto *Section =
          Entry.IsPublic
              ? C.getELFSection(".compartment_imports", ELF::SHT_PROGBITS,
                                ELF::SHF_ALLOC | ELF::SHF_GROUP, 0,
                                Entry.ImportName, true)
              : C.getELFSection(".compartment_imports", ELF::SHT_PROGBITS,
                                ELF::SHF_ALLOC);
      OutStreamer->SwitchSection(Section);
      auto Sym = C.getOrCreateSymbol(Entry.ImportName);
      auto ExportSym = C.getOrCreateSymbol(Entry.ExportName);
      OutStreamer->emitSymbolAttribute(Sym, MCSA_ELF_TypeObject);
      if (Entry.IsPublic)
        OutStreamer->emitSymbolAttribute(Sym, MCSA_Weak);
      OutStreamer->emitValueToAlignment(8);
      OutStreamer->emitLabel(Sym);
      // Library imports have their low bit set.
      if (Entry.IsLibrary)
        OutStreamer->emitValue(
            MCBinaryExpr::createAdd(MCSymbolRefExpr::create(ExportSym, C),
                                    MCConstantExpr::create(1, C), C),
            4);
      else
        OutStreamer->emitValue(MCSymbolRefExpr::create(ExportSym, C), 4);
      OutStreamer->emitIntValue(0, 4);
      OutStreamer->emitELFSize(Sym, MCConstantExpr::create(8, C));
    }
  }

  if (TM.getTargetTriple().isOSBinFormatELF())
    RTS.finishAttributeSection();

}

void RISCVAsmPrinter::emitAttributes() {
  RISCVTargetStreamer &RTS =
      static_cast<RISCVTargetStreamer &>(*OutStreamer->getTargetStreamer());
  RTS.emitTargetAttributes(*STI);
}

// Force static initialization.
extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeRISCVAsmPrinter() {
  RegisterAsmPrinter<RISCVAsmPrinter> X(getTheRISCV32Target());
  RegisterAsmPrinter<RISCVAsmPrinter> Y(getTheRISCV64Target());
}
