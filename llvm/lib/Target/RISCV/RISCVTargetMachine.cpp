//===-- RISCVTargetMachine.cpp - Define TargetMachine for RISCV -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the info about RISCV target spec.
//
//===----------------------------------------------------------------------===//

#include "RISCVTargetMachine.h"
#include "MCTargetDesc/RISCVBaseInfo.h"
#include "RISCV.h"
#include "RISCVTargetObjectFile.h"
#include "RISCVTargetTransformInfo.h"
#include "TargetInfo/RISCVTargetInfo.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/GlobalISel/IRTranslator.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelect.h"
#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/RegBankSelect.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Cheri.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetOptions.h"
using namespace llvm;

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeRISCVTarget() {
  RegisterTargetMachine<RISCVTargetMachine> X(getTheRISCV32Target());
  RegisterTargetMachine<RISCVTargetMachine> Y(getTheRISCV64Target());
  auto *PR = PassRegistry::getPassRegistry();
  initializeGlobalISel(*PR);
  initializeRISCVMergeBaseOffsetOptPass(*PR);
  initializeRISCVExpandPseudoPass(*PR);
  initializeRISCVInsertVSETVLIPass(*PR);
}

static std::string computeDataLayout(const Triple &TT, StringRef FS,
                                     const TargetOptions &Options) {
  assert((TT.isArch32Bit() || TT.isArch64Bit()) &&
         "only RV32 and RV64 are currently supported");

  StringRef IntegerTypes;
  if (TT.isArch64Bit()) {
    IntegerTypes = "-p:64:64-i64:64-i128:128-n64";
  } else {
    IntegerTypes = "-p:32:32-i64:64-n32";
  }

  StringRef CapTypes = "";
  StringRef PurecapOptions = "";
  if (FS.contains("+xcheri")) {
    if (TT.isArch64Bit())
      CapTypes = "-pf200:128:128:128:64";
    else
      CapTypes = "-pf200:64:64:64:32";

    RISCVABI::ABI ABI = RISCVABI::getTargetABI(Options.MCOptions.getABIName());
    if (ABI != RISCVABI::ABI_Unknown && RISCVABI::isCheriPureCapABI(ABI))
      PurecapOptions = "-A200-P200-G200";
  }

  return ("e-m:e" + CapTypes + IntegerTypes + "-S128" + PurecapOptions).str();
}

static Reloc::Model getEffectiveRelocModel(const Triple &TT,
                                           Optional<Reloc::Model> RM) {
  if (!RM.hasValue())
    return Reloc::Static;
  return *RM;
}

static CodeModel::Model getEffectiveCodeModel(Optional<CodeModel::Model> CM,
                                              CodeModel::Model Default,
                                              const TargetOptions &Options) {
  if ((Options.MCOptions.ABIName == "cheriot") && CM &&
      (*CM == CodeModel::Tiny))
    return *CM;
  return getEffectiveCodeModel(CM, Default);
}

RISCVTargetMachine::RISCVTargetMachine(const Target &T, const Triple &TT,
                                       StringRef CPU, StringRef FS,
                                       const TargetOptions &Options,
                                       Optional<Reloc::Model> RM,
                                       Optional<CodeModel::Model> CM,
                                       CodeGenOpt::Level OL, bool JIT)
    : LLVMTargetMachine(T, computeDataLayout(TT, FS, Options), TT, CPU, FS,
                        Options, getEffectiveRelocModel(TT, RM),
                        ::getEffectiveCodeModel(CM, CodeModel::Small, Options),
                        OL),
      TLOF(std::make_unique<RISCVELFTargetObjectFile>()) {
  initAsmInfo();

  // RISC-V supports the MachineOutliner.
  setMachineOutliner(true);
}

const RISCVSubtarget *
RISCVTargetMachine::getSubtargetImpl(const Function &F) const {
  Attribute CPUAttr = F.getFnAttribute("target-cpu");
  Attribute TuneAttr = F.getFnAttribute("tune-cpu");
  Attribute FSAttr = F.getFnAttribute("target-features");

  std::string CPU =
      CPUAttr.isValid() ? CPUAttr.getValueAsString().str() : TargetCPU;
  std::string TuneCPU =
      TuneAttr.isValid() ? TuneAttr.getValueAsString().str() : CPU;
  std::string FS =
      FSAttr.isValid() ? FSAttr.getValueAsString().str() : TargetFS;
  std::string Key = CPU + TuneCPU + FS;
  auto &I = SubtargetMap[Key];
  if (!I) {
    // This needs to be done before we create a new subtarget since any
    // creation will depend on the TM and the code generation flags on the
    // function that reside in TargetOptions.
    resetTargetOptions(F);
    auto ABIName = Options.MCOptions.getABIName();
    if (const MDString *ModuleTargetABI = dyn_cast_or_null<MDString>(
            F.getParent()->getModuleFlag("target-abi"))) {
      auto TargetABI = RISCVABI::getTargetABI(ABIName);
      if (TargetABI != RISCVABI::ABI_Unknown &&
          ModuleTargetABI->getString() != ABIName) {
        report_fatal_error("-target-abi option != target-abi module flag");
      }
      ABIName = ModuleTargetABI->getString();
    }
    I = std::make_unique<RISCVSubtarget>(TargetTriple, CPU, TuneCPU, FS, ABIName, *this);
  }
  return I.get();
}

TargetTransformInfo
RISCVTargetMachine::getTargetTransformInfo(const Function &F) {
  return TargetTransformInfo(RISCVTTIImpl(this, F));
}

// A RISC-V hart has a single byte-addressable address space of 2^XLEN bytes
// for all memory accesses, so it is reasonable to assume that an
// implementation has no-op address space casts. If an implementation makes a
// change to this, they can override it here.
bool RISCVTargetMachine::isNoopAddrSpaceCast(unsigned SrcAS,
                                             unsigned DstAS) const {
  // TODO: We should get the DataLayout instead of hardcoding AS200.
  const bool SrcIsCheri = isCheriPointer(SrcAS, nullptr);
  const bool DestIsCheri = isCheriPointer(DstAS, nullptr);
  if ((SrcIsCheri || DestIsCheri) && (SrcIsCheri != DestIsCheri))
    return false;
  return true;
}

namespace {
class RISCVPassConfig : public TargetPassConfig {
public:
  RISCVPassConfig(RISCVTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  RISCVTargetMachine &getRISCVTargetMachine() const {
    return getTM<RISCVTargetMachine>();
  }

  void addIRPasses() override;
  bool addInstSelector() override;
  bool addIRTranslator() override;
  bool addLegalizeMachineIR() override;
  bool addRegBankSelect() override;
  bool addGlobalInstructionSelect() override;
  void addPreEmitPass() override;
  void addPreEmitPass2() override;
  void addPreSched2() override;
  void addPreRegAlloc() override;
};
} // namespace

TargetPassConfig *RISCVTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new RISCVPassConfig(*this, PM);
}

void RISCVPassConfig::addIRPasses() {
  addPass(createAtomicExpandPass());
  addPass(createCheriBoundAllocasPass());
  TargetPassConfig::addIRPasses();
}

bool RISCVPassConfig::addInstSelector() {
  addPass(createRISCVISelDag(getRISCVTargetMachine()));

  return false;
}

bool RISCVPassConfig::addIRTranslator() {
  addPass(new IRTranslator(getOptLevel()));
  return false;
}

bool RISCVPassConfig::addLegalizeMachineIR() {
  addPass(new Legalizer());
  return false;
}

bool RISCVPassConfig::addRegBankSelect() {
  addPass(new RegBankSelect());
  return false;
}

bool RISCVPassConfig::addGlobalInstructionSelect() {
  addPass(new InstructionSelect(getOptLevel()));
  return false;
}

void RISCVPassConfig::addPreSched2() {}

void RISCVPassConfig::addPreEmitPass() { addPass(&BranchRelaxationPassID); }

void RISCVPassConfig::addPreEmitPass2() {
  addPass(createRISCVExpandPseudoPass(
      getTM<RISCVTargetMachine>().ImportedFunctions));
  // Schedule the expansion of AMOs at the last possible moment, avoiding the
  // possibility for other passes to break the requirements for forward
  // progress in the LR/SC block.
  addPass(createRISCVExpandAtomicPseudoPass());
}

void RISCVPassConfig::addPreRegAlloc() {
  if (TM->getOptLevel() != CodeGenOpt::None) {
    addPass(createRISCVCheriCleanupOptPass());
    addPass(createRISCVMergeBaseOffsetOptPass());
  }
  addPass(createRISCVInsertVSETVLIPass());
}
