/**
 * \file
 * \brief The arch hooks tier-1 code generation calls.
 *
 * arm64 has no tier-1 code generator, so a hook that has to emit tier-1 code
 * aborts.
 */

#include "arch/arch.hpp"

#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

namespace mono::arch {

[[noreturn]] static void
no_tier1 ()
{
	llvm::report_fatal_error ("mono: arm64 has no tier-1 code generator", false);
}

/* The thrower's module is plain C and reaches this pass too. */
llvm::PreservedAnalyses
MonoAbiPass::run (llvm::Module &m, llvm::ModuleAnalysisManager &mam)
{
	for (llvm::Function &f : m)
		for (llvm::BasicBlock &bb : f)
			for (llvm::Instruction &i : bb)
				if (auto *call = llvm::dyn_cast<llvm::CallBase> (&i);
				    call && call->getFnAttr (mono_cc_attribute).isValid ())
					no_tier1 ();

	return llvm::PreservedAnalyses::all ();
}

llvm::Function *
create_mono_entry_thunk (llvm::Module &m, llvm::StringRef name, llvm::Function *target,
                         llvm::Value *through)
{
	no_tier1 ();
}

void
emit_faulting_byte_read (llvm::MachineBasicBlock &mbb,
                         llvm::MachineBasicBlock::iterator at,
                         const llvm::TargetInstrInfo &tii, llvm::Register pointer,
                         llvm::MachineBasicBlock *handler, llvm::DebugLoc dl)
{
	no_tier1 ();
}

bool
analyze_null_check_branch (llvm::MachineBasicBlock &mbb, const llvm::TargetInstrInfo &tii,
                           llvm::TargetInstrInfo::MachineBranchPredicate &mbp)
{
	return tii.analyzeBranchPredicate (mbb, mbp, false);
}

} // namespace mono::arch
