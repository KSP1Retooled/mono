/**
 * \file
 * \brief Recognition of the compare-and-branch forms that arm64 null checks
 * use.
 */

#include "arch/arch.hpp"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineInstr.h>
#include <llvm/CodeGen/MachineOperand.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/CodeGen/TargetRegisterInfo.h>
#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/Support/ErrorHandling.h>

using namespace llvm;

namespace mono::arch {
namespace {

using MachineBranchPredicate = TargetInstrInfo::MachineBranchPredicate;

/// Finds an AArch64 register by name, because the target's register enum is
/// private.
unsigned
find_register (const TargetRegisterInfo &tri, StringRef name)
{
	for (unsigned i = 1, n = tri.getNumRegs (); i < n; ++i)
		if (tri.getName (i) == name)
			return i;

	report_fatal_error (Twine ("mono: this LLVM's AArch64 target has no ") + name
	                    + " register - check this against the installed LLVM version");
}

/// Finds an AArch64 opcode by name, because the target's opcode enum is
/// private.
unsigned
find_opcode (const TargetInstrInfo &tii, StringRef name)
{
	for (unsigned i = 0, n = tii.getNumOpcodes (); i < n; ++i)
		if (tii.getName (i) == name)
			return i;

	report_fatal_error (Twine ("mono: this LLVM's AArch64 target has no ") + name
	                    + " opcode - check this against the installed LLVM version");
}

/// AArch64CC values for b.eq and b.ne.
constexpr int64_t cond_code_eq = 0;
constexpr int64_t cond_code_ne = 1;

/// Matches `cmp xN, #0` followed by `b.eq` or `b.ne`, and fills \p mbp the way
/// analyzeBranchPredicate () does. Returns false on success.
bool
match_zero_compare_branch (MachineBasicBlock &mbb, const TargetInstrInfo &tii,
                           MachineBranchPredicate &mbp)
{
	SmallVector<MachineOperand, 4> cond;

	if (tii.analyzeBranch (mbb, mbp.TrueDest, mbp.FalseDest, cond,
	                       /*AllowModify=*/false))
		return true;

	// A folded compare-and-branch puts -1 and its opcode in front of its
	// operands, so only a b.cc leaves a single condition operand.
	if (cond.size () != 1 || mbp.TrueDest == nullptr)
		return true;

	if (mbp.FalseDest == nullptr)
		mbp.FalseDest = mbb.getNextNode ();

	const TargetRegisterInfo &tri = *mbb.getParent ()->getSubtarget ().getRegisterInfo ();
	static const unsigned nzcv = find_register (tri, "NZCV");
	static const unsigned subs_imm = find_opcode (tii, "SUBSXri");

	MachineInstr *condition_def = nullptr;
	bool single_use = true;

	for (MachineInstr &mi : llvm::reverse (mbb)) {
		if (mi.isTerminator ())
			continue;

		if (mi.modifiesRegister (nzcv, &tri)) {
			condition_def = &mi;
			break;
		}

		if (mi.readsRegister (nzcv, &tri))
			single_use = false;
	}

	if (condition_def == nullptr)
		return true;

	if (single_use) {
		for (MachineBasicBlock *succ : mbb.successors ())
			if (succ->isLiveIn (nzcv))
				single_use = false;
	}

	int64_t cc = cond[0].getImm ();

	// SUBSXri <dead>, Rn, imm12, shift is `cmp Rn, #imm`.
	if ((cc != cond_code_eq && cc != cond_code_ne)
	    || condition_def->getOpcode () != subs_imm
	    || !condition_def->getOperand (1).isReg ()
	    || condition_def->getOperand (2).getImm () != 0
	    || condition_def->getOperand (3).getImm () != 0)
		return true;

	mbp.ConditionDef = condition_def;
	mbp.SingleUseCondition = single_use;
	mbp.LHS = condition_def->getOperand (1);
	mbp.RHS = MachineOperand::CreateImm (0);
	mbp.Predicate = cc == cond_code_ne ? MachineBranchPredicate::PRED_NE
	                                   : MachineBranchPredicate::PRED_EQ;
	return false;
}

} // namespace

bool
analyze_null_check_branch (MachineBasicBlock &mbb, const TargetInstrInfo &tii,
                           MachineBranchPredicate &mbp)
{
	// cbz and cbnz test the pointer itself, so no compare precedes them to
	// remove. While the pointer is still a virtual register,
	// analyzeBranchPredicate () reports the pointer's own definition as the
	// condition, and that definition is not a compare either.
	if (!tii.analyzeBranchPredicate (mbb, mbp, /*AllowModify=*/false)
	    && mbp.LHS.isReg () && mbp.RHS.isImm ()) {
		mbp.ConditionDef = nullptr;
		return false;
	}

	mbp = MachineBranchPredicate ();
	return match_zero_compare_branch (mbb, tii, mbp);
}

} // namespace mono::arch
