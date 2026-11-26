/**
 * \file
 * \brief An arm64 memory read whose only purpose is to fault on a null pointer.
 */

#include "arch/arch.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/CodeGen/FaultMaps.h>
#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineInstrBuilder.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/CodeGen/TargetOpcodes.h>
#include <llvm/CodeGen/TargetRegisterInfo.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono::arch {
namespace {

/// Finds the LDRBBui opcode by name, because the AArch64 target's opcode enum
/// is private.
unsigned
ldrbbui_opcode (const TargetInstrInfo &tii)
{
	static const unsigned opcode = [&] {
		for (unsigned i = 0, n = tii.getNumOpcodes (); i < n; ++i)
			if (tii.getName (i) == "LDRBBui")
				return i;

		report_fatal_error ("mono: this LLVM's AArch64 target has no LDRBBui opcode - "
		                    "check this against the installed LLVM version");
	} ();

	return opcode;
}

/// Finds the WZR register by name, because the AArch64 target's register enum
/// is private.
Register
wzr_register (const TargetRegisterInfo &tri)
{
	static const Register reg = [&] {
		for (unsigned i = 1, n = tri.getNumRegs (); i < n; ++i)
			if (tri.getName (i) == StringRef ("WZR"))
				return Register (i);

		report_fatal_error ("mono: this LLVM's AArch64 target has no WZR register - "
		                    "check this against the installed LLVM version");
	} ();

	return reg;
}

} // namespace

void
emit_faulting_byte_read (MachineBasicBlock &mbb, MachineBasicBlock::iterator at,
                         const TargetInstrInfo &tii, Register pointer,
                         MachineBasicBlock *handler, DebugLoc dl)
{
	const TargetRegisterInfo &tri = *mbb.getParent ()->getSubtarget ().getRegisterInfo ();

	// This is `ldrb wzr, [pointer]`. The load must name a destination, and the
	// zero register discards the loaded byte.
	BuildMI (mbb, at, dl, tii.get (TargetOpcode::FAULTING_OP))
		.addReg (wzr_register (tri), RegState::Define | RegState::Dead)
		.addImm (FaultMaps::FaultingLoad)
		.addMBB (handler)
		.addImm (ldrbbui_opcode (tii))
		.addReg (pointer)
		.addImm (0);
}

} // namespace mono::arch
