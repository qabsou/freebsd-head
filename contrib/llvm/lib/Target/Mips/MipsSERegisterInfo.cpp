//===-- MipsSERegisterInfo.cpp - MIPS32/64 Register Information -== -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the MIPS32/64 implementation of the TargetRegisterInfo
// class.
//
//===----------------------------------------------------------------------===//

#include "MipsSERegisterInfo.h"
#include "Mips.h"
#include "MipsAnalyzeImmediate.h"
#include "MipsSEInstrInfo.h"
#include "MipsSubtarget.h"
#include "MipsMachineFunction.h"
#include "llvm/Constants.h"
#include "llvm/DebugInfo.h"
#include "llvm/Type.h"
#include "llvm/Function.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/Target/TargetFrameLowering.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"

using namespace llvm;

MipsSERegisterInfo::MipsSERegisterInfo(const MipsSubtarget &ST,
                                       const TargetInstrInfo &TII)
  : MipsRegisterInfo(ST, TII) {}

// This function eliminate ADJCALLSTACKDOWN,
// ADJCALLSTACKUP pseudo instructions
void MipsSERegisterInfo::
eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator I) const {
  const TargetFrameLowering *TFI = MF.getTarget().getFrameLowering();

  if (!TFI->hasReservedCallFrame(MF)) {
    int64_t Amount = I->getOperand(0).getImm();

    if (I->getOpcode() == Mips::ADJCALLSTACKDOWN)
      Amount = -Amount;

    const MipsSEInstrInfo *II = static_cast<const MipsSEInstrInfo*>(&TII);
    unsigned SP = Subtarget.isABI_N64() ? Mips::SP_64 : Mips::SP;

    II->adjustStackPtr(SP, Amount, MBB, I);
  }

  MBB.erase(I);
}

void MipsSERegisterInfo::eliminateFI(MachineBasicBlock::iterator II,
                                     unsigned OpNo, int FrameIndex,
                                     uint64_t StackSize,
                                     int64_t SPOffset) const {
  MachineInstr &MI = *II;
  MachineFunction &MF = *MI.getParent()->getParent();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  MipsFunctionInfo *MipsFI = MF.getInfo<MipsFunctionInfo>();

  const std::vector<CalleeSavedInfo> &CSI = MFI->getCalleeSavedInfo();
  int MinCSFI = 0;
  int MaxCSFI = -1;

  if (CSI.size()) {
    MinCSFI = CSI[0].getFrameIdx();
    MaxCSFI = CSI[CSI.size() - 1].getFrameIdx();
  }

  // The following stack frame objects are always referenced relative to $sp:
  //  1. Outgoing arguments.
  //  2. Pointer to dynamically allocated stack space.
  //  3. Locations for callee-saved registers.
  // Everything else is referenced relative to whatever register
  // getFrameRegister() returns.
  unsigned FrameReg;

  if (MipsFI->isOutArgFI(FrameIndex) ||
      (FrameIndex >= MinCSFI && FrameIndex <= MaxCSFI))
    FrameReg = Subtarget.isABI_N64() ? Mips::SP_64 : Mips::SP;
  else
    FrameReg = getFrameRegister(MF);

  // Calculate final offset.
  // - There is no need to change the offset if the frame object is one of the
  //   following: an outgoing argument, pointer to a dynamically allocated
  //   stack space or a $gp restore location,
  // - If the frame object is any of the following, its offset must be adjusted
  //   by adding the size of the stack:
  //   incoming argument, callee-saved register location or local variable.
  int64_t Offset;

  if (MipsFI->isOutArgFI(FrameIndex))
    Offset = SPOffset;
  else
    Offset = SPOffset + (int64_t)StackSize;

  Offset    += MI.getOperand(OpNo + 1).getImm();

  DEBUG(errs() << "Offset     : " << Offset << "\n" << "<--------->\n");

  // If MI is not a debug value, make sure Offset fits in the 16-bit immediate
  // field.
  if (!MI.isDebugValue() && !isInt<16>(Offset)) {
    MachineBasicBlock &MBB = *MI.getParent();
    DebugLoc DL = II->getDebugLoc();
    unsigned ADDu = Subtarget.isABI_N64() ? Mips::DADDu : Mips::ADDu;
    unsigned ATReg = Subtarget.isABI_N64() ? Mips::AT_64 : Mips::AT;
    MipsAnalyzeImmediate::Inst LastInst(0, 0);

    MipsFI->setEmitNOAT();
    Mips::loadImmediate(Offset, Subtarget.isABI_N64(), TII, MBB, II, DL, true,
                        &LastInst);
    BuildMI(MBB, II, DL, TII.get(ADDu), ATReg).addReg(FrameReg).addReg(ATReg);

    FrameReg = ATReg;
    Offset = SignExtend64<16>(LastInst.ImmOpnd);
  }

  MI.getOperand(OpNo).ChangeToRegister(FrameReg, false);
  MI.getOperand(OpNo + 1).ChangeToImmediate(Offset);
}
