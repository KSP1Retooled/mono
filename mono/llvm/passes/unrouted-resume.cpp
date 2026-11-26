/**
 * \file
 * \brief Turning the `resume`s that no inline used back into `unreachable`.
 */

#include "unrouted-resume.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/Transforms/Utils/Local.h>

using namespace llvm;

namespace mono {

PreservedAnalyses
DropUnroutedResumePass::run (Function &f, FunctionAnalysisManager &fam)
{
	SmallVector<WeakVH, 8> blocks;

	for (BasicBlock &block : f)
		if (isa<ResumeInst> (block.getTerminator ()))
			blocks.push_back (&block);

	if (blocks.empty ())
		return PreservedAnalyses::all ();

	const TargetTransformInfo &tti = fam.getResult<TargetIRAnalysis> (f);

	for (WeakVH &block : blocks)
		changeToUnreachable (cast<BasicBlock> (block)->getTerminator ());

	// Tier 1 runs no simplification after this pass, so this pass removes the
	// dead default arm of each pad's switch itself. simplifyCFG can erase a
	// block that is later in the list, so the list holds weak handles.
	for (WeakVH &block : blocks)
		if (auto *dead = cast_or_null<BasicBlock> (block))
			simplifyCFG (dead, tti);

	return PreservedAnalyses::none ();
}

} // namespace mono
