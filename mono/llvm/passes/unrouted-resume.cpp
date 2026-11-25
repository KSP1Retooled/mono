/**
 * \file
 * \brief Taking the `resume` back off the pads of a compiled method.
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

	// Tier 1 has no simplification behind this to take the dead arm of each
	// pad's dispatch away. A simplification can erase a block further down.
	for (WeakVH &block : blocks)
		if (auto *dead = cast_or_null<BasicBlock> (block))
			simplifyCFG (dead, tti);

	return PreservedAnalyses::none ();
}

} // namespace mono
